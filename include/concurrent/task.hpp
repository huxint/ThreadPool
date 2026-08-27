#pragma once
#include "concurrent/detail/sbo_function.hpp"
#include "concurrent/detail/spinlock.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

namespace concurrent {

    /// 提交失败的错误类别
    enum class submit_error : std::uint8_t {
        stopped,       ///< 池已关闭, 拒绝新任务
        out_of_memory, ///< 内部分配失败(库内唯一允许 catch 的位置)
    };

    /// 被取消任务的错误标记(经 get() 的错误通道返回)
    struct operation_cancelled {};

    /// 无效任务(默认构造后 get)的空错误指针; 请以 valid() 预检
    inline const std::exception_ptr invalid_task_error{};

    /// 从错误通道辨识"提交阶段失败". 零 throw 契约允许库内 catch - 异常不外泄
    /// @return 若该错误由 submit_error 承载则返回之, 否则 nullopt
    [[nodiscard]]
    inline std::optional<submit_error> submit_error_of(const std::exception_ptr& e) noexcept {
        if (!e) {
            return std::nullopt;
        }
        try {
            std::rethrow_exception(e);
        } catch (submit_error se) {
            return se;
        } catch (...) {
            return std::nullopt;
        }
    }

    /// 该错误是否表示任务在排队期间被取消(任务体未曾执行)
    [[nodiscard]]
    inline bool is_cancelled(const std::exception_ptr& e) noexcept {
        if (!e) {
            return false;
        }
        try {
            std::rethrow_exception(e);
        } catch (const operation_cancelled&) {
            return true;
        } catch (...) {
            return false;
        }
    }

    /// 任务句柄前向声明
    template <typename T>
    class task;

    namespace detail {

        struct void_passthrough {};

        /// 任务节点: 窃取队列与全局队列上流转的实体. 稳态由每 worker 空闲链表回收;
        /// 队列槽中仅存指针(平凡可拷贝), 保证 Chase-Lev 读-CAS 语义安全
        ///
        /// 两个链接字段服务于互斥的三种归属(环/溢出链, 空闲链), 故不会同时使用:
        struct task_node {
            sbo_function<64> body;
            /// 排队中被关闭丢弃时的状态收尾(仅 submit 路径设置). 必须先于
            /// body 析构调用: 闭包持有共享状态引用, 若随节点直接湮灭,
            /// 用户侧 get() 将在 done 等待上永久阻塞
            void (*discard)(void* state) noexcept = nullptr;
            void* discard_ctx = nullptr;
            task_node* next_free = nullptr; ///< 归属"每 worker 空闲链"时的后继
            task_node* next_q = nullptr;    ///< 归属"全局队列溢出链"时的后继
        };

        /// 续延节点: 任务完成时被内联执行一次的类型擦除回调
        /// 不用虚函数是为了让节点保持平凡布局, 并省去 vptr 与虚析构的间接开销
        struct cont_node {
            cont_node* next = nullptr;
            /// 永不抛出: 一切失败进入子状态错误通道
            void (*invoke)(cont_node* self, void* parent_state) noexcept = nullptr;
            /// 以正确派生类型销毁自身(避免虚析构开销)
            void (*destroy)(cont_node* self) noexcept = nullptr;
        };

        /// 组合子续延节点基座: 构造时把 Derived::run 绑定到类型擦除入口
        template <typename ParentState, typename Derived>
        struct cont_impl : cont_node {
            cont_impl() noexcept {
                invoke = [](cont_node* self, void* p) noexcept {
                    static_cast<Derived*>(self)->run(*reinterpret_cast<ParentState*>(p));
                };
                destroy = [](cont_node* self) noexcept { delete static_cast<Derived*>(self); };
            }
        };

        /// 值存储: 非 void 用对齐裸存储; void 用零体积 monostate 占位
        /// 特化而非 std::conditional_t, 避免 sizeof(void)/alignof(void) 在非选中分支被实例化
        /// (不用 std::aligned_storage_t - C++23 起已弃用)
        template <typename T>
        struct value_storage {
            struct type {
                alignas(T) std::byte buf[sizeof(T)];
            };
        };
        template <>
        struct value_storage<void> {
            using type = std::monostate;
        };

        /**
         * @brief 任务共享状态: 单次堆分配. 完成等待走 futex(atomic::wait),
         *        热路径无互斥, 无条件变量; 无条件内嵌 stop_source(约 16B),
         *        使可取消与非可取消任务共用同一套类型
         *
         * finish 先发布完成, 再内联续延 - 续延内部对父任务的等待不会死锁
         */
        template <typename T>
        class shared_state {
        public:
            using value_type = T;

            std::uint64_t id = 0;      ///< trace 任务编号
            std::stop_source source{}; ///< 取消源; 非取消提交时不可触发

            /// value_ 是裸字节缓冲, 不会自行调用 T 的析构函数. 结果值未被取走
            /// 就析构本状态时(只 wait 不 get / 丢弃组合子中间态 / 只迭代半个
            /// parallel_view), 若不在此收尾则 T 的析构函数永不执行
            ~shared_state() {
                if constexpr (!std::is_void_v<T>) {
                    if (has_value_) {
                        std::destroy_at(std::launder(reinterpret_cast<T*>(&value_)));
                    }
                }
            }

            shared_state() = default;
            shared_state(const shared_state&) = delete ("task states are shared, never copied");
            shared_state& operator=(const shared_state&) = delete ("task states are never copied");

            /// 完成路径(任务体外壳恰好调用一次)
            void finish() noexcept {
                cont_node* list;
                {
                    spinlock::guard g{lock_};
                    list = std::exchange(conts_, nullptr);
                    done_.store(1, std::memory_order_release);
                }
                done_.notify_all();
                while (list) {
                    cont_node* n = std::exchange(list, list->next);
                    n->invoke(n, reinterpret_cast<void*>(this));
                    n->destroy(n); // 续延节点一次性消耗
                }
            }

            /// 附加续延; 若已完成返回 false(调用方需立即内联执行)
            [[nodiscard]]
            bool attach(cont_node* c) noexcept {
                spinlock::guard g{lock_};
                if (done_.load(std::memory_order_relaxed) == 1) {
                    return false;
                }
                c->next = std::exchange(conts_, c);
                return true;
            }

            void set_exception(std::exception_ptr e) noexcept { exc_ = std::move(e); }

            void set_cancelled() noexcept {
                cancelled_ = true;
                exc_ = std::make_exception_ptr(operation_cancelled{});
            }

            template <typename U = T>
                requires(!std::is_void_v<U>)
            void emplace_value(U&& v) noexcept(std::is_nothrow_constructible_v<U, U&&>) {
                ::new (static_cast<void*>(&value_)) T(std::forward<U>(v));
                has_value_ = true;
            }

            /// 取走值(恰好一次; 外壳保证调用次序). 成员模板延迟实例化, 避免 T=void 时形成
            /// void&
            ///
            /// 按值返回而非 T&&: 只有这样才能在调用方消费完之后销毁缓冲区里的源对象.
            /// 移动后的残壳仍可能持有资源, 而只可拷贝的 T 更是整个对象都留在原处 -
            /// 二者都必须析构. 代价是续延跳转多一次 T 的移动(get() 路径不变, 原本
            /// 也是两次移动)
            template <typename U = T>
                requires(!std::is_void_v<U>)
            [[nodiscard]]
            U take_value_unchecked() noexcept(std::is_nothrow_move_constructible_v<U>) {
                U* p = std::launder(reinterpret_cast<U*>(&value_));
                U out{std::move(*p)}; // 抛出则 has_value_ 仍为真, 交由析构函数收尾
                std::destroy_at(p);
                has_value_ = false;
                return out;
            }

            /// 非消耗性观察(inspect 用). 成员模板延迟实例化, 避免 T=void 时形成 void&
            template <typename U = T>
                requires(!std::is_void_v<U>)
            [[nodiscard]]
            U& peek_value() noexcept {
                return *std::launder(reinterpret_cast<U*>(&value_));
            }

            [[nodiscard]]
            bool cancelled() const noexcept {
                return cancelled_;
            }

            [[nodiscard]]
            std::exception_ptr raw_exception() const noexcept {
                return exc_;
            }

            [[nodiscard]]
            std::atomic<std::uint32_t>& raw_done() noexcept {
                return done_;
            }

            void wait_done() const {
                // 注意形态: acquire 观测必须发生在本线程的循环条件里
                // libstdc++ 的 wait 唤醒重载不保证携带 acquire 序(TSan 实证缺边),
                // 若依赖其内部重载读取, 将看不到 finish 所发布的值/异常/取消字段
                while (done_.load(std::memory_order_acquire) == 0) {
                    done_.wait(0, std::memory_order_acquire);
                }
            }

            /// 取走结果. 空错误指针表示无效任务或结果已被消费
            [[nodiscard]]
            std::expected<T, std::exception_ptr> take_result() {
                wait_done();
                if (exc_) {
                    return std::unexpected(exc_);
                }
                if constexpr (!std::is_void_v<T>) {
                    if (!has_value_) {
                        return std::unexpected(invalid_task_error);
                    }
                    return take_value_unchecked();
                } else {
                    return {};
                }
            }

        private:
            spinlock lock_{};
            cont_node* conts_ = nullptr;
            std::atomic<std::uint32_t> done_{0};
            std::exception_ptr exc_ = nullptr;
            [[no_unique_address]]
            typename value_storage<T>::type value_{};
            bool has_value_ = false;
            bool cancelled_ = false;
        };

        template <typename T>
        [[nodiscard]]
        std::shared_ptr<shared_state<T>> make_state() {
            return std::make_shared<shared_state<T>>(); // bad_alloc 由边界捕获
        }

        template <typename ParentState, typename F, typename U>
        struct map_cont final : cont_impl<ParentState, map_cont<ParentState, F, U>> {
            std::shared_ptr<shared_state<U>> dst;
            F fn;

            void run(ParentState& parent) noexcept {
                using V = typename ParentState::value_type;
                if (parent.cancelled()) {
                    dst->set_cancelled();
                } else if (auto e = parent.raw_exception()) {
                    dst->set_exception(e);
                } else {
                    try {
                        if constexpr (std::is_void_v<V>) {
                            if constexpr (std::is_void_v<U>) {
                                fn();
                            } else {
                                dst->emplace_value(fn());
                            }
                        } else {
                            dst->emplace_value(fn(parent.take_value_unchecked()));
                        }
                    } catch (...) {
                        dst->set_exception(std::current_exception());
                    }
                }
                dst->finish();
            }
        };

        template <typename ParentState, typename F>
        struct inspect_cont final : cont_impl<ParentState, inspect_cont<ParentState, F>> {
            std::shared_ptr<ParentState> dst;
            F fn;

            void run(ParentState& parent) noexcept {
                using V = typename ParentState::value_type;
                if (parent.cancelled()) {
                    dst->set_cancelled();
                } else if (auto e = parent.raw_exception()) {
                    dst->set_exception(e);
                } else {
                    try {
                        if constexpr (std::is_void_v<V>) {
                            fn();
                        } else {
                            fn(parent.peek_value());
                            dst->emplace_value(parent.take_value_unchecked());
                        }
                    } catch (...) {
                        dst->set_exception(std::current_exception());
                    }
                }
                dst->finish();
            }
        };

        /// @tparam InnerTask 用户绑定返回的任务类型(仅经其公共 st_ 成员接线,
        ///                   完整性在实例化点成立)
        template <typename ParentState, typename F, typename InnerTask>
        struct and_then_cont final
            : cont_impl<ParentState, and_then_cont<ParentState, F, InnerTask>> {
            using U = typename InnerTask::value_type;
            using inner_state_t = shared_state<U>;

            std::shared_ptr<shared_state<U>> dst;
            F fn;

            void run(ParentState& parent) noexcept {
                using V = typename ParentState::value_type;
                if (parent.cancelled()) {
                    dst->set_cancelled();
                    dst->finish();
                    return;
                }
                if (auto e = parent.raw_exception()) {
                    dst->set_exception(e);
                    dst->finish();
                    return;
                }
                std::shared_ptr<inner_state_t> inner_st;
                try {
                    if constexpr (std::is_void_v<V>) {
                        InnerTask inner = fn();
                        inner_st = inner.st_;
                    } else {
                        InnerTask inner = fn(parent.take_value_unchecked());
                        inner_st = inner.st_;
                    }
                } catch (...) {
                    dst->set_exception(std::current_exception());
                    dst->finish();
                    return;
                }
                if (!inner_st) {
                    dst->set_exception(invalid_task_error);
                    dst->finish();
                    return;
                }
                // 本状态的完成绑定到内层任务完成
                struct fwd final : cont_impl<inner_state_t, fwd> {
                    std::shared_ptr<shared_state<U>> dst;
                    void run(inner_state_t& src) noexcept {
                        if (src.cancelled()) {
                            dst->set_cancelled();
                        } else if (auto e = src.raw_exception()) {
                            dst->set_exception(e);
                        } else if constexpr (!std::is_void_v<U>) {
                            dst->emplace_value(src.take_value_unchecked());
                        }
                        dst->finish();
                    }
                };
                auto* n = new (std::nothrow) fwd{{}, dst};
                if (!n) {
                    dst->set_exception(std::make_exception_ptr(std::bad_alloc{}));
                    dst->finish();
                    return;
                }
                if (!inner_st->attach(n)) {
                    n->run(*inner_st); // 内层已完成: 立即内联
                    delete n;
                }
            }
        };

        template <typename T>
        struct slot_store {
            alignas(T) std::byte buf[sizeof(T)];
            bool live = false;

            slot_store() = default;
            slot_store(const slot_store&) = delete;
            slot_store& operator=(const slot_store&) = delete;
            ~slot_store() {
                if (live) {
                    std::destroy_at(value());
                }
            }

            void put(T&& v) noexcept(std::is_nothrow_move_constructible_v<T>) {
                ::new (static_cast<void*>(buf)) T(std::move(v));
                live = true;
            }
            /// 按值返回并销毁源对象: 与 shared_state::take_value_unchecked 同理,
            /// 若只把值移走而把残壳留在 buf 里(且 live 已置假), 析构函数就再也
            /// 收不到它
            [[nodiscard]]
            T take() noexcept(std::is_nothrow_move_constructible_v<T>) {
                T* p = value();
                T out{std::move(*p)};
                std::destroy_at(p);
                live = false;
                return out;
            }

        private:
            [[nodiscard]]
            T* value() noexcept {
                return std::launder(reinterpret_cast<T*>(buf));
            }
        };

        template <typename... Ts>
            requires((!std::is_void_v<Ts>) && ...)
        class when_all_core {
        public:
            static constexpr std::size_t n = sizeof...(Ts);

            explicit when_all_core(std::shared_ptr<shared_state<std::tuple<Ts...>>> d) noexcept
                : dst(std::move(d)) {}

            std::shared_ptr<shared_state<std::tuple<Ts...>>> dst;
            std::atomic<std::uint32_t> remaining{n};
            std::exception_ptr first_err = nullptr;
            bool errored = false; ///< 独立标志: 空 exception_ptr(无效任务)也算错误
            std::tuple<slot_store<Ts>...> slots;

            void record_error(std::exception_ptr e) noexcept {
                lk.lock(); // 首错优先
                if (!errored) {
                    errored = true;
                    first_err = e;
                }
                lk.unlock();
            }

            template <std::size_t I, typename V>
            void put(V&& v) noexcept(std::is_nothrow_move_constructible_v<V>) {
                std::get<I>(slots).put(std::forward<V>(v));
            }

            void settle_one() noexcept {
                if (remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    if (errored) {
                        // 部分槽未填充: 绝不可组装 tuple, 直接以首错失败
                        dst->set_exception(first_err);
                    } else {
                        try {
                            dst->emplace_value(assemble());
                        } catch (...) {
                            dst->set_exception(std::current_exception());
                        }
                    }
                    dst->finish();
                }
            }

        private:
            /// 结构化绑定包(C++26 P1061)展开各槽, 免去 index_sequence 辅助
            [[nodiscard]]
            std::tuple<Ts...> assemble() noexcept(
                (std::is_nothrow_move_constructible_v<Ts> && ...)) {
                auto& [...slot] = slots;
                return std::tuple<Ts...>(slot.take()...);
            }

            spinlock lk{};
        };

        template <std::size_t I, typename ParentState, typename Core>
        struct deposit_cont final : cont_impl<ParentState, deposit_cont<I, ParentState, Core>> {
            std::shared_ptr<Core> core;

            void run(ParentState& parent) noexcept {
                if (parent.cancelled()) {
                    core->record_error(std::make_exception_ptr(operation_cancelled{}));
                } else if (auto e = parent.raw_exception()) {
                    core->record_error(e);
                } else {
                    core->template put<I>(parent.take_value_unchecked());
                }
                core->settle_one();
            }
        };

        /// OOM 出口声明(定义置于 task 完整定义之后)
        template <typename T>
        [[nodiscard]]
        task<T> failed_task() noexcept;

        /// map/and_then 的结果类型计算: void 父任务 -> 续延无参; 非 void -> 续延接收 T&&
        /// 用特化而非 std::conditional_t, 避免 T=void 时形成 void&&
        template <typename T, typename F>
        struct map_result_of {
            using type = std::invoke_result_t<F, T&&>;
        };
        template <typename F>
        struct map_result_of<void, F> {
            using type = std::invoke_result_t<F>;
        };

        template <typename T, typename F>
        struct and_then_inner_of {
            using type = std::invoke_result_t<F, T&&>;
        };
        template <typename F>
        struct and_then_inner_of<void, F> {
            using type = std::invoke_result_t<F>;
        };

    } // namespace detail

    /**
     * @brief 任务句柄. 零 throw 契约下的结果载体: get() 返回 expected,
     *        任务体异常经 exception_ptr 透传, 取消以 operation_cancelled 标记
     *
     * 单子表面: map(变换)/ and_then(绑定)/ inspect(旁观)均在完成任务
     * 的工作线程上内联执行, 构造期零入队; 结果值恰好可取一次
     *
     * @note st_ 为库内接线成员, 勿在库外触碰
     */
    template <typename T>
    class task {
    public:
        using value_type = T;

    private:
        // 尾置返回类型不属于完整类上下文, 别名必须先于使用声明
        using state_t = detail::shared_state<T>;

    public:
        task() = default;

        /// 内部构造(池与组合子使用)
        explicit task(std::shared_ptr<detail::shared_state<T>> s) noexcept : st_(std::move(s)) {}

        /// 阻塞直至完成并取走结果
        [[nodiscard]]
        std::expected<T, std::exception_ptr> get() {
            if (!st_) {
                return std::unexpected(invalid_task_error);
            }
            return st_->take_result();
        }

        /// 阻塞等待完成但不取值
        void wait() const {
            if (st_) {
                st_->wait_done();
            }
        }

        /// 请求取消(仅当任务体轮询 token 或尚未开跑时有意义)
        void request_stop() noexcept {
            if (st_) {
                st_->source.request_stop();
            }
        }

        [[nodiscard]]
        bool valid() const noexcept {
            return st_ != nullptr;
        }

        /// 变换成功值: f 接收 T&&(void 任务无参), 在完成任务的工作线程上内联执行
        template <typename F>
        auto map(this auto&& self, F&& f) -> task<typename detail::map_result_of<T, F>::type> {
            using U = typename detail::map_result_of<T, F>::type;
            if (!self.st_) [[unlikely]] {
                return task<U>{}; // 无效任务的组合仍是无效任务
            }
            return self
                .template attach_cont<U, detail::map_cont<state_t, std::decay_t<F>, U>>(
                    std::forward<F>(f));
        }

        /// 绑定: f 接收 T&& 返回后续 task, 其结果透传为本次结果
        template <typename F>
        auto and_then(this auto&& self, F&& f)
            -> task<typename detail::and_then_inner_of<T, F>::type::value_type> {
            using Inner = typename detail::and_then_inner_of<T, F>::type;
            using U = typename Inner::value_type;
            if (!self.st_) [[unlikely]] {
                return task<U>{}; // 无效任务的组合仍是无效任务
            }
            return self
                .template attach_cont<U, detail::and_then_cont<state_t, std::decay_t<F>, Inner>>(
                    std::forward<F>(f));
        }

        /// 旁观副作用: f 接收 T&(void 任务无参), 不改变结果与错误通道
        template <typename F>
        auto inspect(this auto&& self, F&& f) -> task<T> {
            if (!self.st_) [[unlikely]] {
                return task{}; // 无效任务的组合仍是无效任务
            }
            return self.template attach_cont<T, detail::inspect_cont<state_t, std::decay_t<F>>>(
                std::forward<F>(f));
        }

    private:
        /// 组合子共用骨架: 建子状态 -> 续延节点 nothrow 分配 -> 挂接
        /// (父任务已完成则立即内联执行) -> 返回子任务
        template <typename U, typename NodeT, typename... A>
        task<U> attach_cont(A&&... a) {
            try {
                auto child = detail::make_state<U>();
                auto* n = new (std::nothrow) NodeT{{}, child, std::forward<A>(a)...};
                if (!n) {
                    return detail::failed_task<U>();
                }
                if (!st_->attach(n)) {
                    n->run(*st_); // 父任务已完成: 立即内联
                    delete n;
                }
                return task<U>{std::move(child)};
            } catch (...) {
                return detail::failed_task<U>(); // make_state 的 bad_alloc
            }
        }

    public:
        std::shared_ptr<detail::shared_state<T>> st_;
    };

    namespace detail {

        template <typename T>
        [[nodiscard]]
        task<T> failed_task() noexcept {
            try {
                auto st = make_state<T>();
                st->set_exception(std::make_exception_ptr(std::bad_alloc{}));
                return task<T>{std::move(st)};
            } catch (...) {
                return task<T>{};
            }
        }

    } // namespace detail

    /**
     * @brief 汇合全部任务: 全部成功 -> task<tuple<...>>; 任一失败/取消 -> 以首个错误失败
     * @note 不接受 task<void>; "等全部跑完"请用 execute + pool::wait 表达
     */
    template <typename... Ts>
        requires((!std::is_void_v<Ts>) && ...)
    [[nodiscard]]
    task<std::tuple<Ts...>> when_all(task<Ts>... ts) {
        if constexpr (sizeof...(Ts) == 0) {
            auto st = detail::make_state<std::tuple<>>();
            st->emplace_value(std::tuple<>{});
            st->finish();
            return task<std::tuple<>>{std::move(st)};
        } else {
            try {
                auto dst = detail::make_state<std::tuple<Ts...>>();
                using core_t = detail::when_all_core<Ts...>;
                auto core = std::make_shared<core_t>(dst);

                [[maybe_unused]]
                auto attach_one = [&]<std::size_t I, typename TIn>(
                                      std::integral_constant<std::size_t, I>, task<TIn>& t) {
                    if (!t.st_) [[unlikely]] { // 无效入参: 以空错误指针沉淀, 不解引用
                        core->record_error(invalid_task_error);
                        core->settle_one();
                        return;
                    }
                    using parent_t = detail::shared_state<TIn>;
                    using node_t = detail::deposit_cont<I, parent_t, core_t>;
                    auto* n = new (std::nothrow) node_t{{}, core};
                    if (!n) { // 单槽 OOM 降级为该槽失败, 不放大为整批失败
                        core->record_error(std::make_exception_ptr(std::bad_alloc{}));
                        core->settle_one();
                        return;
                    }
                    if (!t.st_->attach(n)) {
                        n->run(*t.st_); // 已完成: 内联沉淀
                        delete n;
                    }
                };

                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (attach_one(std::integral_constant<std::size_t, I>{}, ts), ...);
                }(std::index_sequence_for<Ts...>{});

                return task<std::tuple<Ts...>>{std::move(dst)};
            } catch (...) {
                return detail::failed_task<std::tuple<Ts...>>();
            }
        }
    }

} // namespace concurrent
