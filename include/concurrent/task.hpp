#pragma once
#include "concurrent/detail/sbo_function.hpp"
#include "concurrent/detail/spinlock.hpp"
#include <atomic>
#include <cstddef>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace concurrent {

    /// 提交失败的错误类别
    enum class submit_error : std::uint8_t {
        stopped,       ///< 池已关闭, 拒绝新任务
        out_of_memory, ///< 内部分配失败(库内唯一允许 catch 的位置)
    };

    /// 被取消任务的错误标记(经 get() 的错误通道返回)
    struct operation_cancelled {};

    /// 无效任务的错误标记: 默认构造的句柄经 get(), 或结果已被消费后的
    /// 再次 get(). 具名类型使该路径可安全重抛, 也可被 is_cancelled /
    /// submit_error_of 判别为"非取消, 非提交失败"
    struct invalid_task {};

    /// 错误通道中 invalid_task 的错误指针
    [[nodiscard]]
    inline std::exception_ptr invalid_task_error() noexcept {
        return std::make_exception_ptr(invalid_task{});
    }

    namespace detail {
        /// 错误通道的类型判别. 零 throw 契约允许库内 catch - 异常不外泄
        template <typename E>
        [[nodiscard]]
        bool error_is(const std::exception_ptr& e) noexcept {
            if (!e) {
                return false;
            }
            try {
                std::rethrow_exception(e);
            } catch (const E&) {
                return true;
            } catch (...) {
                return false;
            }
        }
    } // namespace detail

    /// 该错误是否表示任务句柄无效(默认构造 / 结果已被消费)
    [[nodiscard]]
    inline bool is_invalid_task(const std::exception_ptr& e) noexcept {
        return detail::error_is<invalid_task>(e);
    }

    /// 该错误是否表示任务在排队期间被取消(任务体未曾执行)
    [[nodiscard]]
    inline bool is_cancelled(const std::exception_ptr& e) noexcept {
        return detail::error_is<operation_cancelled>(e);
    }

    /// 从错误通道辨识"提交阶段失败"
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
            /// SBO=64 时节点 112B 落 malloc 128B 桶(浪费 16); 80 恰好 128B
            /// 填满桶, 96 则跨到 160B 桶. 80 是零内存代价下的最大容量
            sbo_function<80> body;
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

        /// 前置声明: 深度守卫基础设施的签名需要(定义在下方)
        template <typename T>
        class shared_state;

        /// 续延链深度守卫: finish -> invoke -> run -> dst->finish() 是递归,
        /// 栈深与 map/inspect 链长线性相关, 长链爆栈. 深度超限时把子状态的
        /// finish 登记到待重放队列, 由最外层 finish 的收尾在同线程排空
        /// (trampoline): 每次重放从浅深度重新进入, 栈深有界; 队列元素持
        /// shared_ptr 保活, 节点销毁不悬挂
        inline constexpr int cont_depth_limit = 64;

        inline thread_local int cont_depth = 0;

        /// 待重放的 finish 请求: 状态引用 + 类型擦除的 finish 入口
        struct pending_finish {
            std::shared_ptr<void> state;
            void (*finish)(const std::shared_ptr<void>&) noexcept;
        };

        /// 待重放队列(函数内 TLS: 命名模块下 namespace 级 inline thread_local
        /// 非平凡变量会在模块单元与导入方各生成一份 TLS init wrapper, 链接冲突;
        /// 函数内 TLS 符号内部链接, 且仅首次超限时构造. cont_depth / in_drain
        /// 平凡可零初始化, 无 wrapper, 不受此限)
        inline std::vector<pending_finish>& pending_finishes() noexcept {
            thread_local std::vector<pending_finish> q;
            return q;
        }

        /// drain 进行中标志: 重入的排空请求直接返回(见 drain_pending_finishes)
        inline thread_local bool in_drain = false;

        /// 排空待重放队列. 仅最外层 finish 收尾调用; 重入时直接返回 -
        /// 内层 finish 的收尾看见队列非空也无需自己动手, 本循环会继续消费,
        /// 嵌套 drain 会把栈深重新与链长挂钩
        inline void drain_pending_finishes() noexcept {
            if (in_drain) {
                return;
            }
            in_drain = true;
            auto& q = pending_finishes();
            while (!q.empty()) {
                pending_finish e = std::move(q.back());
                q.pop_back();
                e.finish(e.state); // 重放的 finish 可能再登记, 循环继续消费
            }
            in_drain = false;
        }

        /// finish 的深度守卫入口(续延末尾调用): 浅深度内联继续, 超限登记待重放.
        /// 登记需分配, 失败(bad_alloc)时退回内联 - 本路径及上游全链 noexcept,
        /// 逃逸的异常只会 terminate 并丢失结果值; OOM 下栈深风险是较小恶
        template <typename T>
        void finish_or_defer(std::shared_ptr<shared_state<T>> st) noexcept {
            if (cont_depth >= cont_depth_limit) [[unlikely]] {
                try {
                    pending_finishes().push_back(pending_finish{
                        st, // 拷贝而非移动: 分配失败时 st 仍持有引用, 可退回内联
                        [](const std::shared_ptr<void>& p) noexcept {
                            static_cast<shared_state<T>*>(p.get())->finish();
                        }});
                    return;
                } catch (...) {
                    // 登记失败: 落入下方内联路径
                }
            }
            st->finish();
        }

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
         *        热路径无互斥, 无条件变量; 取消标志是状态内的一个 atomic<bool>,
         *        使可取消与非可取消任务共用同一套类型
         *
         * 真正的 std::stop_source 仅在 callable 接受 stop_token 时实体化 -
         * libstdc++ 的 stop_source 默认构造即一次堆分配, 而不轮询 token 的
         * 任务只可能被 task::request_stop() 在开跑前取消, 一个原子标志足矣
         *
         * finish 先发布完成, 再内联续延 - 续延内部对父任务的等待不会死锁
         */
        template <typename T>
        class shared_state {
        public:
            using value_type = T;

            std::uint64_t id = 0; ///< trace 任务编号

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

            /// 实体化真正的取消源. 只由提交路径在状态尚未交给任何其他线程时
            /// 调用(callable 接受 stop_token 时), 此后 source_ 只读
            void enable_stop() { source_ = std::stop_source{}; }

            [[nodiscard]]
            std::stop_token get_token() const noexcept {
                return source_.get_token();
            }

            void request_stop() noexcept {
                stop_.store(true, std::memory_order_release);
                source_.request_stop(); // 无状态源(未 enable_stop)上是无操作
            }

            [[nodiscard]]
            bool stop_requested() const noexcept {
                return stop_.load(std::memory_order_acquire);
            }

            /// 完成路径(任务体外壳恰好调用一次)
            void finish() noexcept {
                // 续延链是 Treiber 栈, 以哨兵值封口: exchange 一步既取走全链
                // 又拒绝后来的 attach, 无需互斥. acq_rel 使 attach 侧看到
                // 封口时, 本状态的结果/异常/取消字段已然可见
                cont_node* list = conts_.exchange(closed(), std::memory_order_acq_rel);
                if (list == closed()) [[unlikely]] {
                    return; // 已经完成过: 封口值不是链表, 不可遍历
                }
                done_.store(1, std::memory_order_release);
                // 等待者自计数: 无人等待时连库内的等待者查表都免掉
                // (Dekker 配对: 等待方先登记再复查 done_, 见 wait_done)
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (waiters_.load(std::memory_order_acquire) != 0) [[unlikely]] {
                    done_.notify_all();
                }
                ++cont_depth; // 本帧的续延以内联深度计(见 cont_depth_limit)
                while (list) {
                    cont_node* n = std::exchange(list, list->next);
                    n->invoke(n, reinterpret_cast<void*>(this));
                    n->destroy(n); // 续延节点一次性消耗
                }
                --cont_depth;
                if (cont_depth == 0) {
                    // 最外层帧: 排空深层链登记的待重放 finish, 每次重放
                    // 从浅深度进入, 栈深不随链长增长
                    drain_pending_finishes();
                }
            }

            /// 附加续延; 若已完成返回 false(调用方需立即内联执行)
            [[nodiscard]]
            bool attach(cont_node* c) noexcept {
                cont_node* head = conts_.load(std::memory_order_acquire);
                do {
                    if (head == closed()) {
                        return false;
                    }
                    c->next = head;
                } while (!conts_.compare_exchange_weak(head, c, std::memory_order_acq_rel,
                                                       std::memory_order_acquire));
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

            void wait_done() const {
                // 注意形态: acquire 观测必须发生在本线程的循环条件里
                // libstdc++ 的 wait 唤醒重载不保证携带 acquire 序(TSan 实证缺边),
                // 若依赖其内部重载读取, 将看不到 finish 所发布的值/异常/取消字段
                if (done_.load(std::memory_order_acquire) != 0) [[likely]] {
                    return;
                }
                // 登记 -> 栅栏 -> 复查: 与 finish 的"发布 -> 栅栏 -> 读登记"
                // 构成 Dekker 配对. 复查漏看完成则对方必已看到本次登记,
                // 从而照常 notify; 反之 wait(0) 见 done_ 非零即刻返回
                waiters_.fetch_add(1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                while (done_.load(std::memory_order_acquire) == 0) {
                    done_.wait(0, std::memory_order_acquire);
                }
                waiters_.fetch_sub(1, std::memory_order_release);
            }

            /// 取走结果. invalid_task 标记表示结果已被消费
            [[nodiscard]]
            std::expected<T, std::exception_ptr> take_result() {
                wait_done();
                if (exc_) {
                    return std::unexpected(exc_);
                }
                if constexpr (!std::is_void_v<T>) {
                    if (!has_value_) {
                        return std::unexpected(invalid_task_error());
                    }
                    return take_value_unchecked();
                } else {
                    return {};
                }
            }

        private:
            /// 续延链的封口哨兵: conts_ 取此值即"已完成, 不再接受 attach".
            /// 用状态自身的地址, 与任何真实续延节点地址必然不同
            [[nodiscard]]
            cont_node* closed() const noexcept {
                return reinterpret_cast<cont_node*>(const_cast<shared_state*>(this));
            }

            std::atomic<cont_node*> conts_{nullptr};
            std::atomic<std::uint32_t> done_{0};
            mutable std::atomic<std::uint32_t> waiters_{0};
            std::stop_source source_{std::nostopstate};
            std::atomic<bool> stop_{false};
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
                finish_or_defer(std::move(dst));
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
                finish_or_defer(std::move(dst));
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
                    finish_or_defer(std::move(dst));
                    return;
                }
                if (auto e = parent.raw_exception()) {
                    dst->set_exception(e);
                    finish_or_defer(std::move(dst));
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
                    finish_or_defer(std::move(dst));
                    return;
                }
                if (!inner_st) {
                    dst->set_exception(invalid_task_error());
                    finish_or_defer(std::move(dst));
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
                        finish_or_defer(std::move(dst));
                    }
                };
                auto* n = new (std::nothrow) fwd{{}, dst};
                if (!n) {
                    dst->set_exception(std::make_exception_ptr(std::bad_alloc{}));
                    finish_or_defer(std::move(dst));
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
                std::scoped_lock g{lk}; // 首错优先; RAII 免去手写 unlock
                if (!errored) {
                    errored = true;
                    first_err = std::move(e);
                }
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
                    // dst 由本分支独占(remaining 归零仅一次): 移交守卫入口
                    finish_or_defer(std::move(dst));
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
                return std::unexpected(invalid_task_error());
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
                st_->request_stop();
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
                    if (!t.st_) [[unlikely]] { // 无效入参: 以具名标记沉淀, 不解引用
                        core->record_error(invalid_task_error());
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
