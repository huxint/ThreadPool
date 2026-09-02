#pragma once
#include "concurrent/detail/sbo_function.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <optional>
#include <stop_token>
#include <tuple>
#include <type_traits>
#include <utility>
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
    /// 再次领取. 具名类型使该路径可安全重抛, 也可被 is_cancelled /
    /// submit_error_of 判别为"非取消, 非提交失败"
    struct invalid_task {};

    /// 错误通道中 invalid_task 的错误指针
    [[nodiscard]]
    inline std::exception_ptr invalid_task_error() noexcept {
        return std::make_exception_ptr(invalid_task{});
    }

    namespace detail {
        /// 错误通道的类型判别与还原: exception_ptr 的动态类型只能经重抛
        /// 辨识. 零 throw 契约允许库内 catch - 异常不外泄
        template <typename E>
        [[nodiscard]]
        std::optional<E> error_as(const std::exception_ptr& e) noexcept {
            if (!e) {
                return std::nullopt;
            }
            try {
                std::rethrow_exception(e);
            } catch (const E& v) {
                return v;
            } catch (...) {
            }
            return std::nullopt;
        }
    } // namespace detail

    /// 该错误是否表示任务句柄无效(默认构造 / 结果已被消费)
    [[nodiscard]]
    inline bool is_invalid_task(const std::exception_ptr& e) noexcept {
        return detail::error_as<invalid_task>(e).has_value();
    }

    /// 该错误是否表示任务在排队期间被取消(任务体未曾执行)
    [[nodiscard]]
    inline bool is_cancelled(const std::exception_ptr& e) noexcept {
        return detail::error_as<operation_cancelled>(e).has_value();
    }

    /// 从错误通道辨识"提交阶段失败"
    /// @return 若该错误由 submit_error 承载则返回之, 否则 nullopt
    [[nodiscard]]
    inline std::optional<submit_error> submit_error_of(const std::exception_ptr& e) noexcept {
        return detail::error_as<submit_error>(e);
    }

    /// 任务句柄前向声明
    template <typename T>
    class task;

    namespace detail {

        /// 任务节点: 窃取队列与全局队列上流转的实体. 稳态由每 worker 空闲链表回收;
        /// 队列槽中仅存指针(平凡可拷贝), 保证 Chase-Lev 读-CAS 语义安全
        struct task_node {
            /// 88 使节点恰好 128B 填满 malloc 的 128B 桶: 零内存代价下的最大 SBO 容量
            sbo_function<88> body;
            /// 排队中被关闭丢弃时的状态收尾(仅 submit 路径设置). 必须先于
            /// body 析构调用: 闭包持有共享状态引用, 若随节点直接湮灭,
            /// 用户侧 get() 将在 done 等待上永久阻塞
            void (*discard)(void* state) noexcept = nullptr;
            void* discard_ctx = nullptr;
            /// 侵入式链接: 归属全局队列溢出链或 worker 空闲链时的后继. 节点
            /// 同一时刻只在一处, 两种归属共用一个字段
            task_node* next = nullptr;
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
                    static_cast<Derived*>(self)->run(*static_cast<ParentState*>(p));
                };
                destroy = [](cont_node* self) noexcept { delete static_cast<Derived*>(self); };
            }
        };

        /// 挂接续延; 父任务已完成则立即内联执行并销毁节点
        template <typename State, typename Node>
        void attach_or_run(State& st, Node* n) noexcept {
            if (!st.attach(n)) {
                n->run(st);
                delete n;
            }
        }

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

        /// 显式生命周期的值槽: 对齐裸存储 + 存活位. 析构时仍存活即销毁 -
        /// 结果值未被取走的状态(只 wait 不 get / 丢弃的组合子中间态 / 只迭代
        /// 半个 parallel_view)不漏掉 T 的析构
        template <typename T>
        class value_slot {
        public:
            value_slot() = default;
            value_slot(const value_slot&) = delete;
            value_slot& operator=(const value_slot&) = delete;
            ~value_slot() {
                if (live_) {
                    std::destroy_at(ptr());
                }
            }

            template <typename... A>
            void emplace(A&&... a) noexcept(std::is_nothrow_constructible_v<T, A...>) {
                ::new (static_cast<void*>(buf_)) T(std::forward<A>(a)...);
                live_ = true;
            }

            /// 按值取走并销毁源: 只移动不销毁, 移动后残壳持有的资源乃至只可
            /// 拷贝类型的整个对象都会滞留. 移动构造抛出时存活位不变, 交由析构收尾
            [[nodiscard]]
            T take() noexcept(std::is_nothrow_move_constructible_v<T>) {
                T out{std::move(*ptr())};
                std::destroy_at(ptr());
                live_ = false;
                return out;
            }

            [[nodiscard]]
            bool has_value() const noexcept {
                return live_;
            }

        private:
            T* ptr() noexcept { return std::launder(reinterpret_cast<T*>(buf_)); }

            alignas(T) std::byte buf_[sizeof(T)];
            bool live_ = false;
        };

        /// void 无值: 零体积占位, emplace() 为空操作, 使"发布值"在 void 与
        /// 非 void 上写法一致
        template <>
        class value_slot<void> {
        public:
            void emplace() noexcept {}
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
                    n->invoke(n, static_cast<void*>(this));
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

            /// 取消即以 operation_cancelled 占据错误通道: get 与续延都只看
            /// 该通道, 无须另设标志
            void set_cancelled() noexcept {
                exc_ = std::make_exception_ptr(operation_cancelled{});
            }

            template <typename... A>
            void emplace_value(A&&... a) noexcept(std::is_nothrow_constructible_v<T, A...>) {
                value_.emplace(std::forward<A>(a)...);
            }

            /// 结果的一次性闸门: 首个领取者(get 或某个续延)得 true, 其余 false.
            /// void 任务无值可搬, 单凭值槽存活位分不出"再次领取", 故闸门独立
            /// 于值槽; 两份句柄并发领取也由此收敛成一次原子交换
            [[nodiscard]]
            bool claim() noexcept {
                if (consumed_.exchange(true, std::memory_order_relaxed)) {
                    return false;
                }
                if constexpr (std::is_void_v<T>) {
                    return true;
                } else {
                    return value_.has_value();
                }
            }

            /// 搬走值. @pre claim() 已返回 true. 成员模板延迟实例化, T=void 不成形
            template <typename U = T>
                requires(!std::is_void_v<U>)
            [[nodiscard]]
            U take_value() noexcept(std::is_nothrow_move_constructible_v<U>) {
                return value_.take();
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

            /// 取走结果. 错误可重复观测; 成功值恰好一次, 再次领取以 invalid_task 标记
            [[nodiscard]]
            std::expected<T, std::exception_ptr> take_result() {
                wait_done();
                if (exc_) {
                    return std::unexpected(exc_);
                }
                if (!claim()) {
                    return std::unexpected(invalid_task_error());
                }
                if constexpr (std::is_void_v<T>) {
                    return {};
                } else {
                    return take_value();
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
            std::atomic<bool> consumed_{false};
            std::exception_ptr exc_ = nullptr;
            [[no_unique_address]] value_slot<T> value_;
        };

        template <typename T>
        [[nodiscard]]
        std::shared_ptr<shared_state<T>> make_state() {
            return std::make_shared<shared_state<T>>(); // bad_alloc 由边界捕获
        }

        /// 续延的共同骨架: 父任务失败(异常或取消)原样转交 on_error; 成功则
        /// 领取值交付 on_value(void 父任务无参). 领取失败(值已被 get 或另一
        /// 续延取走)与 on_value 抛出一律进 on_error, 一切出口 noexcept
        /// @return 值是否成功交付
        template <typename Parent, typename OnValue, typename OnError>
        bool deliver(Parent& parent, OnValue&& on_value, OnError&& on_error) noexcept {
            if (auto e = parent.raw_exception()) {
                on_error(std::move(e));
                return false;
            }
            if (!parent.claim()) {
                on_error(invalid_task_error());
                return false;
            }
            try {
                if constexpr (std::is_void_v<typename Parent::value_type>) {
                    on_value();
                } else {
                    on_value(parent.take_value());
                }
                return true;
            } catch (...) {
                on_error(std::current_exception());
                return false;
            }
        }

        template <typename ParentState, typename F, typename U>
        struct map_cont final : cont_impl<ParentState, map_cont<ParentState, F, U>> {
            std::shared_ptr<shared_state<U>> dst;
            F fn;

            void run(ParentState& parent) noexcept {
                deliver(
                    parent,
                    [&](auto&&... v) {
                        if constexpr (std::is_void_v<U>) {
                            std::invoke(fn, std::forward<decltype(v)>(v)...);
                        } else {
                            dst->emplace_value(std::invoke(fn, std::forward<decltype(v)>(v)...));
                        }
                    },
                    [&](std::exception_ptr e) { dst->set_exception(std::move(e)); });
                finish_or_defer(std::move(dst));
            }
        };

        template <typename ParentState, typename F>
        struct inspect_cont final : cont_impl<ParentState, inspect_cont<ParentState, F>> {
            std::shared_ptr<ParentState> dst;
            F fn;

            void run(ParentState& parent) noexcept {
                deliver(
                    parent,
                    [&](auto&&... v) {
                        std::invoke(fn, v...); // 旁观以左值交付, 值随后原样转入子状态
                        dst->emplace_value(std::move(v)...);
                    },
                    [&](std::exception_ptr e) { dst->set_exception(std::move(e)); });
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

            /// 内层任务完成 -> 结果原样转入本状态
            struct forward final : cont_impl<inner_state_t, forward> {
                std::shared_ptr<shared_state<U>> dst;

                void run(inner_state_t& src) noexcept {
                    deliver(
                        src,
                        [&](auto&&... v) { dst->emplace_value(std::forward<decltype(v)>(v)...); },
                        [&](std::exception_ptr e) { dst->set_exception(std::move(e)); });
                    finish_or_defer(std::move(dst));
                }
            };

            void run(ParentState& parent) noexcept {
                auto fail = [&](std::exception_ptr e) {
                    dst->set_exception(std::move(e));
                    finish_or_defer(std::move(dst));
                };
                std::shared_ptr<inner_state_t> inner;
                if (!deliver(
                        parent,
                        [&](auto&&... v) {
                            inner = std::invoke(fn, std::forward<decltype(v)>(v)...).st_;
                        },
                        fail)) {
                    return;
                }
                if (!inner) {
                    return fail(invalid_task_error()); // 绑定返回了无效任务
                }
                auto* n = new (std::nothrow) forward{{}, dst};
                if (!n) {
                    return fail(std::make_exception_ptr(std::bad_alloc{}));
                }
                attach_or_run(*inner, n); // 本状态的完成绑定到内层任务完成
            }
        };

        template <typename... Ts>
            requires((!std::is_void_v<Ts>) && ...)
        class when_all_core {
        public:
            explicit when_all_core(std::shared_ptr<shared_state<std::tuple<Ts...>>> d) noexcept
                : dst_(std::move(d)) {}

            /// 首错优先: 交换闸门决出唯一写者, 其写入经 remaining_ 上 RMW 的
            /// 释放序列传递给最后一个 settle_one(acq_rel), 无须互斥
            void record_error(std::exception_ptr e) noexcept {
                if (!errored_.exchange(true, std::memory_order_relaxed)) {
                    first_err_ = std::move(e);
                }
            }

            template <std::size_t I, typename V>
            void put(V&& v) noexcept(std::is_nothrow_move_constructible_v<V>) {
                std::get<I>(slots_).emplace(std::forward<V>(v));
            }

            void settle_one() noexcept {
                if (remaining_.fetch_sub(1, std::memory_order_acq_rel) != 1) {
                    return;
                }
                if (errored_.load(std::memory_order_relaxed)) {
                    // 部分槽未填充: 绝不可组装 tuple, 直接以首错失败
                    dst_->set_exception(std::move(first_err_));
                } else {
                    try {
                        dst_->emplace_value(assemble());
                    } catch (...) {
                        dst_->set_exception(std::current_exception());
                    }
                }
                // remaining_ 归零仅一次, dst_ 由本分支独占: 移交守卫入口
                finish_or_defer(std::move(dst_));
            }

        private:
            /// 结构化绑定包(C++26 P1061)展开各槽, 免去 index_sequence 辅助
            [[nodiscard]]
            std::tuple<Ts...> assemble() noexcept(
                (std::is_nothrow_move_constructible_v<Ts> && ...)) {
                auto& [...slot] = slots_;
                return std::tuple<Ts...>(slot.take()...);
            }

            std::shared_ptr<shared_state<std::tuple<Ts...>>> dst_;
            std::atomic<std::uint32_t> remaining_{sizeof...(Ts)};
            std::atomic<bool> errored_{false};
            std::exception_ptr first_err_;
            std::tuple<value_slot<Ts>...> slots_;
        };

        template <std::size_t I, typename ParentState, typename Core>
        struct deposit_cont final : cont_impl<ParentState, deposit_cont<I, ParentState, Core>> {
            std::shared_ptr<Core> core;

            void run(ParentState& parent) noexcept {
                deliver(
                    parent, [&](auto&& v) { core->template put<I>(std::forward<decltype(v)>(v)); },
                    [&](std::exception_ptr e) { core->record_error(std::move(e)); });
                core->settle_one();
            }
        };

        /// OOM 出口声明(定义置于 task 完整定义之后)
        template <typename T>
        [[nodiscard]]
        task<T> failed_task() noexcept;

        /// 续延体的结果类型: void 父任务的续延无参, 非 void 接收 T&&
        /// 特化而非 std::conditional_t, 避免 T=void 时形成 void&&
        template <typename T, typename F>
        struct cont_result {
            using type = std::invoke_result_t<F, T&&>;
        };
        template <typename F>
        struct cont_result<void, F> {
            using type = std::invoke_result_t<F>;
        };
        template <typename T, typename F>
        using cont_result_t = typename cont_result<T, F>::type;

    } // namespace detail

    /**
     * @brief 任务句柄. 零 throw 契约下的结果载体: get() 返回 expected,
     *        任务体异常经 exception_ptr 透传, 取消以 operation_cancelled 标记
     *
     * 单子表面: map(变换)/ and_then(绑定)/ inspect(旁观)均在完成任务
     * 的工作线程上内联执行, 构造期零入队. 结果值恰好可领取一次 - 不论经
     * get 还是某个续延, 其余领取者得到 invalid_task
     *
     * @note st_ 为库内接线成员, 勿在库外触碰
     */
    template <typename T>
    class task {
        using state_t = detail::shared_state<T>;

    public:
        using value_type = T;

        task() = default;

        /// 内部构造(池与组合子使用)
        explicit task(std::shared_ptr<state_t> s) noexcept : st_(std::move(s)) {}

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
        task<detail::cont_result_t<T, F>> map(F&& f) {
            using U = detail::cont_result_t<T, F>;
            return attach_cont<U, detail::map_cont<state_t, std::decay_t<F>, U>>(
                std::forward<F>(f));
        }

        /// 绑定: f 接收 T&& 返回后续 task, 其结果透传为本次结果
        template <typename F>
        auto and_then(F&& f) -> task<typename detail::cont_result_t<T, F>::value_type> {
            using inner_t = detail::cont_result_t<T, F>;
            return attach_cont<typename inner_t::value_type,
                               detail::and_then_cont<state_t, std::decay_t<F>, inner_t>>(
                std::forward<F>(f));
        }

        /// 旁观副作用: f 接收 T&(void 任务无参), 不改变结果与错误通道
        template <typename F>
        task inspect(F&& f) {
            return attach_cont<T, detail::inspect_cont<state_t, std::decay_t<F>>>(
                std::forward<F>(f));
        }

    private:
        /// 组合子共用骨架: 无效任务的组合仍是无效任务; 否则建子状态 -> 续延
        /// 节点 nothrow 分配 -> 挂接(父任务已完成则立即内联执行) -> 返回子任务
        template <typename U, typename Node, typename... A>
        task<U> attach_cont(A&&... a) {
            if (!st_) [[unlikely]] {
                return task<U>{};
            }
            try {
                auto child = detail::make_state<U>();
                auto* n = new (std::nothrow) Node{{}, child, std::forward<A>(a)...};
                if (!n) {
                    return detail::failed_task<U>();
                }
                detail::attach_or_run(*st_, n);
                return task<U>{std::move(child)};
            } catch (...) {
                return detail::failed_task<U>(); // make_state 的 bad_alloc
            }
        }

    public:
        std::shared_ptr<state_t> st_;
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
        try {
            auto dst = detail::make_state<std::tuple<Ts...>>();
            if constexpr (sizeof...(Ts) == 0) {
                dst->emplace_value();
                dst->finish();
            } else {
                using core_t = detail::when_all_core<Ts...>;
                auto core = std::make_shared<core_t>(dst);
                auto miss = [&](std::exception_ptr e) {
                    core->record_error(std::move(e));
                    core->settle_one();
                };
                auto attach_one = [&]<std::size_t I>(task<Ts...[I]>& t) {
                    if (!t.st_) [[unlikely]] {
                        return miss(invalid_task_error()); // 无效入参: 具名标记沉淀, 不解引用
                    }
                    auto* node = new (std::nothrow)
                        detail::deposit_cont<I, detail::shared_state<Ts...[I]>, core_t>{{}, core};
                    if (!node) {
                        // 单槽 OOM 降级为该槽失败, 不放大为整批失败
                        return miss(std::make_exception_ptr(std::bad_alloc{}));
                    }
                    detail::attach_or_run(*t.st_, node);
                };
                [&]<std::size_t... I>(std::index_sequence<I...>) {
                    (attach_one.template operator()<I>(ts...[I]), ...);
                }(std::index_sequence_for<Ts...>{});
            }
            return task<std::tuple<Ts...>>{std::move(dst)};
        } catch (...) {
            return detail::failed_task<std::tuple<Ts...>>();
        }
    }

} // namespace concurrent
