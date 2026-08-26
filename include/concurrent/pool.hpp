#pragma once
#include "concurrent/detail/chase_lev.hpp"
#include "concurrent/detail/cpu_relax.hpp"
#include "concurrent/detail/global_queue.hpp"
#include "concurrent/tags.hpp"
#include "concurrent/task.hpp"
#include "concurrent/trace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <inplace_vector>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace concurrent {

    /// 关闭策略
    enum class shutdown_policy : std::uint8_t {
        drain,   ///< 排空全部排队任务后退出(析构默认)
        discard, ///< 丢弃未开始的任务立即退出(对被丢弃任务发取消信号的语义等价物)
    };

    namespace detail {

        /// worker 线程上下文: 身份 + 空闲节点回收链 + 分层本地 deque
        template <std::size_t Levels, std::size_t LocalCap>
        struct alignas(64) worker_ctx {
            std::atomic<task_node*> free_head{nullptr}; ///< MPSC 入 / 单消费者出
            std::size_t index = 0;
            std::array<chase_lev_deque<task_node*, LocalCap>, Levels> local{};
        };

        /// 线程内当前池与 worker 身份(嵌套提交路由用)
        inline thread_local const void* tls_pool = nullptr;
        inline thread_local std::size_t tls_worker = 0;

        /// MPSC Treiber 压栈(任意线程)/ 单消费者弹出(仅所有者)
        /// 仅单一消费者 -> 无 ABA
        inline void freelist_push(std::atomic<task_node*>& head, task_node* n) noexcept {
            task_node* h = head.load(std::memory_order_relaxed);
            do {
                n->next_free = h;
            } while (!head.compare_exchange_weak(h, n, std::memory_order_release,
                                                 std::memory_order_relaxed));
        }
        inline task_node* freelist_pop(std::atomic<task_node*>& head) noexcept {
            auto h = head.load(std::memory_order_acquire);
            while (h) {
                if (head.compare_exchange_weak(h, h->next_free, std::memory_order_acquire,
                                               std::memory_order_acquire)) {
                    return h;
                }
            }
            return nullptr;
        }

        /// submit 的结果类型: token 感知调用优先匹配
        template <typename F, typename... Args>
        struct submit_result {
            using type = std::invoke_result_t<F, Args...>;
        };
        template <typename F, typename... Args>
            requires std::invocable<F, std::stop_token, Args...>
        struct submit_result<F, Args...> {
            using type = std::invoke_result_t<F, std::stop_token, Args...>;
        };
        template <typename F, typename... Args>
        using submit_result_t = typename submit_result<F, Args...>::type;

        template <typename F, typename... Args>
        inline constexpr bool takes_token_v = std::invocable<F, std::stop_token, Args...>;

    } // namespace detail

    /**
     * @brief 固定容量工作窃取线程池
     *
     * 架构: 每线程分层本地 deque(LIFO)+ Chase-Lev 窃取(FIFO, 随机 victim 起点)
     * + 分层全局可扩容队列兜底(Vyukov 环 + 溢出链). 外部提交进全局,
     * worker 内嵌套提交进本地, 本地溢出落全局; 全局环满则转入同序溢出链,
     * 提交永不阻塞, 永不拒绝
     *
     * 零 throw: 一切失败经 std::expected 报告; 任务体异常被捕获并透传至结果通道;
     * execute 要求 callable 为 noexcept - 从类型系统保证遗忘型任务零异常逃逸
     *
     * 取消: 所有状态无条件内嵌 stop_source, 未开跑即取消的任务体被跳过并标记
     * operation_cancelled
     *
     * @tparam Flags 特性标签: priority / cancellable / trace / worker_cap<N>
     */
    template <typename... Flags>
    class basic_pool {
        static constexpr bool PRIORITY = detail::has_priority_v<Flags...>;
        static constexpr bool TRACE = detail::has_trace_v<Flags...>;
        /// cancellable 标签约束"返回取消源的 execute 重载"的可见性;
        /// submit 的 token 感知不受此限制(状态无条件携带取消源)
        static constexpr bool CANCELLABLE_TAG = detail::has_cancellable_v<Flags...>;
        static constexpr int LEVELS = PRIORITY ? 3 : 1;
        static constexpr std::size_t WORKER_CAP = detail::worker_capacity_v<Flags...>;
        static constexpr std::size_t LOCAL_CAP = 256; ///< 本地 deque 容量(2 的幂)
        static constexpr std::size_t GLOBAL_CAP =
            16384; ///< 全局无锁环容量/层(超出部分落入保序溢出链)
        /// 睡眠前的忙等时间预算: 任务常成簇到达(嵌套提交, 往返模式),
        /// 一小段自旋即可吸收"生产者已在路上"的窗口, 免去每次 futex 睡/醒
        /// 的内核往返 - 实测将空池往返 P50 从 ~55µs 降至亚微秒级
        /// 以时间而非次数计, 跨主频可移植; 到期仍无任务才真正睡眠
        static constexpr auto SPIN_BUDGET = std::chrono::microseconds(64);
        static constexpr int PAUSE_BATCH = 16; ///< 每轮探测间的让核步长

        using node_t = detail::task_node;
        using gq_t = detail::global_queue<node_t, GLOBAL_CAP>;
        using worker_ctx_t = detail::worker_ctx<LEVELS, LOCAL_CAP>;
        using outcome_kind = task_outcome;
        using phase_kind = task_phase;

    public:
        struct options {
            std::size_t threads = 0; ///< 0 -> hardware_concurrency()
            trace_hooks hooks{};     ///< 仅 trace 标签下生效
        };

        /**
         * @brief 直接构造
         *
         * 这是全库唯一可能抛出的入口 - 构造期资源获取(worker 上下文分配,
         * 线程创建)失败无法经返回值表达. 需要严格零 throw 时请改用 try_create()
         *
         * @pre opts.threads ≤ 65536
         * @pre 带 worker_cap<N> 标签时 opts.threads ≤ N
         */
        explicit basic_pool(options opts = {}) pre(opts.threads <= 65536)
            pre(WORKER_CAP == 0 || opts.threads <= WORKER_CAP)
            : hooks_(std::move(opts.hooks)) {
            n_threads_ = opts.threads
                             ? opts.threads
                             : std::max<std::size_t>(std::jthread::hardware_concurrency(), 1);
            // 契约在 release 构建下关闭, 而 inplace_vector 溢出会抛 -> 无条件收紧,
            // 保证"零 throw"不依赖构建选项
            if constexpr (WORKER_CAP != 0) {
                n_threads_ = std::min(n_threads_, WORKER_CAP);
            }

            ctxs_ = std::make_unique<worker_ctx_t[]>(n_threads_);
            for (std::size_t i = 0; i < n_threads_; ++i) {
                ctxs_[i].index = i;
            }
            try {
                spawn_workers();
            } catch (...) {
                // 已就位的 worker 只在看到 stopping_ 后才退出循环; 若跳过这步,
                // 成员析构中 jthread 的 join 会永久阻塞
                abort_partial_construction();
                throw;
            }
        }

        /**
         * @brief 零 throw 构造入口: 构造期资源获取失败经 expected 报告
         *
         * @return 池不可移动, 故以 unique_ptr 交付
         */
        [[nodiscard]]
        static std::expected<std::unique_ptr<basic_pool>, submit_error>
        try_create(options opts = {}) {
            try {
                return std::unique_ptr<basic_pool>(new basic_pool(std::move(opts)));
            } catch (...) { // bad_alloc(上下文/线程句柄)或 system_error(线程创建)
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        ~basic_pool() { shutdown(shutdown_policy::drain); }

        basic_pool(const basic_pool&) = delete ("pools are not copyable");
        basic_pool& operator=(const basic_pool&) = delete ("pools are not copy-assignable");
        basic_pool(basic_pool&&) = delete ("pools are not movable");
        basic_pool& operator=(basic_pool&&) = delete ("pools are not move-assignable");

        /**
         * @brief 提交有返回值的任务. 任务体异常经 exception_ptr 透传至结果通道
         *
         * callable 可接受 `const std::stop_token&` 首参以获得协作取消能力;
         * 返回的 task 携带 request_stop(). 结果值恰好可取一次
         */
        template <typename F, typename... Args>
            requires(!std::same_as<std::remove_cvref_t<F>, task_priority>)
        [[nodiscard]]
        auto submit(F&& f, Args&&... args)
            -> std::expected<task<detail::submit_result_t<F, Args...>>, submit_error> {
            using R = detail::submit_result_t<F, Args...>;
            try {
                return submit_impl<R>(task_priority::normal, std::forward<F>(f),
                                      std::forward<Args>(args)...);
            } catch (const std::bad_alloc&) {
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        /// 提交带优先级的任务 @requires priority 标签
        template <typename F, typename... Args>
            requires(PRIORITY)
        [[nodiscard]]
        auto submit(task_priority prio, F&& f, Args&&... args)
            -> std::expected<task<detail::submit_result_t<F, Args...>>, submit_error> {
            using R = detail::submit_result_t<F, Args...>;
            try {
                return submit_impl<R>(prio, std::forward<F>(f), std::forward<Args>(args)...);
            } catch (const std::bad_alloc&) {
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        /**
         * @brief 即发即忘执行(无结果通道). callable 必须 noexcept - 编译期强制
         *
         * @return 失败仅在提交边界发生(池已停 / 内存不足)
         */
        template <typename F, typename... Args>
            requires(std::is_nothrow_invocable_v<F, Args...>)
        [[nodiscard]]
        std::expected<void, submit_error> execute(F&& f, Args&&... args) {
            try {
                return execute_impl(task_priority::normal, std::forward<F>(f),
                                    std::forward<Args>(args)...);
            } catch (const std::bad_alloc&) {
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        /// 即发即忘 + 优先级 @requires priority 标签
        template <typename F, typename... Args>
            requires(PRIORITY && std::is_nothrow_invocable_v<F, Args...>)
        [[nodiscard]]
        std::expected<void, submit_error> execute(task_priority prio, F&& f, Args&&... args) {
            try {
                return execute_impl(prio, std::forward<F>(f), std::forward<Args>(args)...);
            } catch (const std::bad_alloc&) {
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        /// 可取消的即发即忘执行, 返回取消源 @requires cancellable 标签
        template <typename F, typename... Args>
            requires(CANCELLABLE_TAG && detail::takes_token_v<F, Args...> &&
                     std::is_nothrow_invocable_v<F, std::stop_token, Args...>)
        [[nodiscard]]
        std::expected<std::stop_source, submit_error> execute(F&& f, Args&&... args) {
            try {
                return execute_cancellable_impl(task_priority::normal, std::forward<F>(f),
                                                std::forward<Args>(args)...);
            } catch (const std::bad_alloc&) {
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        /// 可取消 + 优先级 @requires priority 与 cancellable 标签
        template <typename F, typename... Args>
            requires(PRIORITY && CANCELLABLE_TAG && detail::takes_token_v<F, Args...> &&
                     std::is_nothrow_invocable_v<F, std::stop_token, Args...>)
        [[nodiscard]]
        std::expected<std::stop_source, submit_error> execute(task_priority prio, F&& f,
                                                              Args&&... args) {
            try {
                return execute_cancellable_impl(prio, std::forward<F>(f),
                                                std::forward<Args>(args)...);
            } catch (const std::bad_alloc&) {
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        /// 阻塞直至全部任务完成(排队 + 运行中). 虚假唤醒安全
        void wait() const noexcept {
            while (pending_.load(std::memory_order_acquire) != 0) {
                std::uint64_t g = idle_gen_.load(std::memory_order_acquire);
                if (pending_.load(std::memory_order_acquire) == 0) {
                    return;
                }
                idle_gen_.wait(g); // futex 睡眠; 推送方 bump 代际唤醒
            }
        }

        /// @return true=全部完成; false=超时(轮询精度约 100µs)
        template <typename Rep, typename Period>
        [[nodiscard]]
        bool wait_for(const std::chrono::duration<Rep, Period>& d) const noexcept {
            return wait_until(std::chrono::steady_clock::now() + d);
        }

        /// @return true=全部完成; false=到点未完(轮询精度约 100µs)
        template <typename Clock, typename Dur>
        [[nodiscard]]
        bool wait_until(const std::chrono::time_point<Clock, Dur>& tp) const noexcept {
            while (pending_.load(std::memory_order_acquire) != 0) {
                if (Clock::now() >= tp) {
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            return true;
        }

        /// 关闭. drain: 排空后退出(默认); discard: 丢弃未开始的任务立即退出
        void shutdown(shutdown_policy policy = shutdown_policy::drain) noexcept {
            const bool first = !stopping_.exchange(true, std::memory_order_acq_rel);
            if (first && policy == shutdown_policy::drain) {
                wait();
            }

            drop_all_queued(); // discard 的主体; drain 时仅收敛在途提交的残留
            wake_gen_.fetch_add(1, std::memory_order_release);
            wake_gen_.notify_all();
            workers_.clear(); // jthread 析构 join
            flush_freelists();
        }

        [[nodiscard]]
        bool running() const noexcept {
            return !stopping_.load(std::memory_order_acquire);
        }

        [[nodiscard]]
        std::size_t thread_count() const noexcept {
            return n_threads_;
        }

    private:
        template <typename R, typename F, typename... Args>
        std::expected<task<R>, submit_error> submit_impl(task_priority prio, F&& f,
                                                         Args&&... args) {
            auto st = detail::make_state<R>();
            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }

            if constexpr (TRACE) {
                st->id = next_id();
            }

            auto* self = this;
            node->body = [st, self, prio, f = std::forward<F>(f),
                          ... a = std::forward<Args>(args)]() mutable noexcept {
                self->run_task_body(*st, prio, [&]() -> R {
                    if constexpr (detail::takes_token_v<F, Args...>) {
                        return std::invoke(std::move(f), st->source.get_token(), std::move(a)...);
                    } else {
                        return std::invoke(std::move(f), std::move(a)...);
                    }
                });
            };

            if (auto err = route(static_cast<int>(level_of(prio)), node)) {
                return std::unexpected(*err);
            }

            trace_enqueue(st->id, prio);
            return task<R>{std::move(st)};
        }

        template <typename F, typename... Args>
        std::expected<void, submit_error> execute_impl(task_priority prio, F&& f, Args&&... args) {
            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }

            auto* self = this;
            std::uint64_t id = next_id();
            node->body = [self, id, prio, f = std::forward<F>(f),
                          ... a = std::forward<Args>(args)]() mutable noexcept {
                self->trace_begin(id, prio);
                std::invoke(std::move(f), std::move(a)...); // noexcept 由 concepts 强制
                self->trace_end(id, prio, outcome_kind::completed);

                if (self->pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    self->idle_gen_.fetch_add(1, std::memory_order_release);
                    self->idle_gen_.notify_all();
                }
            };

            if (auto err = route(static_cast<int>(level_of(prio)), node)) {
                return std::unexpected(*err);
            }

            trace_enqueue(id, prio);
            return {};
        }

        template <typename F, typename... Args>
        std::expected<std::stop_source, submit_error>
        execute_cancellable_impl(task_priority prio, F&& f, Args&&... args) {
            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }

            // 取消源生命周期随闭包: 调用方句柄失效后任务仍可安全查询
            auto source = std::make_shared<std::stop_source>();
            auto* self = this;
            std::uint64_t id = next_id();
            node->body = [self, id, prio, source, f = std::forward<F>(f),
                          ... a = std::forward<Args>(args)]() mutable noexcept {
                self->trace_begin(id, prio);
                if (!source->stop_requested()) {
                    std::invoke(std::move(f), source->get_token(), std::move(a)...);
                    self->trace_end(id, prio, outcome_kind::completed);
                } else {
                    self->trace_end(id, prio, outcome_kind::cancelled);
                }
                if (self->pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    self->idle_gen_.fetch_add(1, std::memory_order_release);
                    self->idle_gen_.notify_all();
                }
            };

            if (auto err = route(static_cast<int>(level_of(prio)), node)) {
                return std::unexpected(*err);
            }

            trace_enqueue(id, prio);
            return *source; // 拷贝句柄与闭包共享同一停止状态; 不可 move(move
                            // 会掏空闭包持有的源)
        }

        /// 有状态任务的外壳: 取消检查 -> 异常捕获 -> 结果发布 -> 续延内联 -> 计数收尾
        template <typename State, typename Invoker>
        void run_task_body(State& st, task_priority prio, Invoker&& invoke) noexcept {
            outcome_kind o = outcome_kind::completed;

            trace_begin(st.id, prio);
            if (st.source.stop_requested()) {
                st.set_cancelled();
                o = outcome_kind::cancelled;
            } else {
                try {
                    if constexpr (std::is_void_v<typename State::value_type>) {
                        invoke();
                    } else {
                        st.emplace_value(invoke());
                    }
                } catch (...) {
                    st.set_exception(std::current_exception());
                    o = outcome_kind::failed;
                }
            }
            trace_end(st.id, prio, o);
            st.finish(); // 先发布完成再跑续延(续延可能回查本状态)

            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                idle_gen_.fetch_add(1, std::memory_order_release);
                idle_gen_.notify_all();
            }
        }

        /// @return 空 = 成功; 非空 = submit_error
        std::optional<submit_error> route(int level, node_t* node) noexcept {
            pending_.fetch_add(1, std::memory_order_acq_rel);
            if (stopping_.load(std::memory_order_acquire)) [[unlikely]] {
                destroy_node(node);
                pending_.fetch_sub(1, std::memory_order_acq_rel);
                return submit_error::stopped;
            }

            // worker 内嵌套提交: 优先本地 deque(LIFO 缓存热度)
            if (detail::tls_pool == this && ctxs_[detail::tls_worker].local[level].push(node))
                [[likely]] {
                return notify_wake();
            }
            // 外部提交或本地溢出: 全局队列(环满自动落入溢出链, 永不失败, 永不阻塞)
            globals_[level].push(node);
            return notify_wake();
        }

        /// 执行一个节点并回收. body 在执行后立即析构 - 否则其捕获的实参与
        /// 共享状态引用会一直存活到该节点被复用/销毁, 形成可观的内存滞留
        void execute_node(node_t* n, std::size_t worker) noexcept {
            n->body();
            n->body.reset();
            detail::freelist_push(ctxs_[worker].free_head, n);
        }

        std::optional<submit_error> notify_wake() noexcept {
            wake_gen_.fetch_add(1, std::memory_order_release);
            wake_gen_.notify_one(); // 单任务只唤醒一个 worker, 避免 notify_all 惊群
            return std::nullopt;
        }

        static constexpr int level_of(task_priority p) noexcept {
            return PRIORITY ? (p == task_priority::high     ? 0
                               : p == task_priority::normal ? 1
                                                            : 2)
                            : 0;
        }

        node_t* acquire_node() noexcept {
            if (detail::tls_pool == static_cast<const void*>(this)) {
                if (auto* n = detail::freelist_pop(ctxs_[detail::tls_worker].free_head)) [[likely]]
                {
                    return n;
                }
            }
            return new (std::nothrow) node_t{};
        }

        /// 提交失败路径的销毁: 节点尚未入队, 直接归还给分配器
        void destroy_node(node_t* n) noexcept {
            n->body.reset();
            delete n;
        }

        void spawn_workers() {
            workers_.reserve(n_threads_);
            for (std::size_t i = 0; i < n_threads_; ++i) {
                workers_.emplace_back(
                    [this, i](std::stop_token st) { worker_main(i, std::move(st)); });
            }
        }

        /// 构造中途失败的收尾: 先让已启动的 worker 看到停止信号并唤醒它们,
        /// 再 join, 最后清空节点缓存. 之后异常方可继续传播
        void abort_partial_construction() noexcept {
            stopping_.store(true, std::memory_order_release);
            wake_gen_.fetch_add(1, std::memory_order_release);
            wake_gen_.notify_all();
            workers_.clear();
            flush_freelists();
        }

        void worker_main(std::size_t idx, std::stop_token stop) {
            detail::tls_pool = static_cast<const void*>(this);
            detail::tls_worker = idx;
            std::uint32_t seed = static_cast<std::uint32_t>(idx) * 0x9E3779B9u + 1u;

            while (true) {
                node_t* n = try_acquire(seed);
                if (!n) {
                    // 有界自旋: 每批让核后重新探测全队列; 预算耗尽才进入 futex 睡眠
                    const auto deadline = std::chrono::steady_clock::now() + SPIN_BUDGET;
                    while (!n) {
                        for (int s = 0; s < PAUSE_BATCH && !n; ++s) {
                            detail::cpu_relax();
                            n = try_acquire(seed);
                        }
                        if (n || std::chrono::steady_clock::now() >= deadline) {
                            break;
                        }
                    }
                }
                if (n) {
                    execute_node(n, idx);
                    continue;
                }
                // stop_token 是兜底退出条件: jthread 析构会 request_stop 后 join,
                // 即便某条路径漏了 stopping_ 也不会挂死在 join 上
                if (stopping_.load(std::memory_order_acquire) || stop.stop_requested())
                    [[unlikely]] {
                    break;
                }
                std::uint64_t g = wake_gen_.load(std::memory_order_acquire);
                if (any_work_hint()) {
                    continue;
                }
                wake_gen_.wait(g); // 全空则睡; 推送方 bump 代际唤醒
            }

            detail::tls_pool = nullptr;
        }

        /// 取一个任务: 自有本地(高->低) -> 全局(高->低) -> 窃取他人本地(FIFO)
        [[nodiscard]]
        node_t* try_acquire(std::uint32_t& seed) noexcept {
            auto& self = ctxs_[detail::tls_worker];
            for (int lv = 0; lv < LEVELS; ++lv) {
                if (auto* n = self.local[lv].pop()) [[likely]]
                {
                    return n;
                }
            }
            for (int lv = 0; lv < LEVELS; ++lv) {
                if (auto* n = globals_[lv].pop()) {
                    return n;
                }
            }
            const std::size_t start = xorshift(seed) % n_threads_;
            for (std::size_t k = 1; k <= n_threads_; ++k) {
                const std::size_t vi = (start + k) % n_threads_;
                if (vi == detail::tls_worker) {
                    continue;
                }
                auto& victim = ctxs_[vi];
                for (int lv = 0; lv < LEVELS; ++lv) {
                    if (auto* n = victim.local[lv].steal()) {
                        return n;
                    }
                }
            }
            return nullptr;
        }

        /// 廉价预检: 任一队列疑似非空则不睡眠(近似值, 宁误醒勿漏活)
        [[nodiscard]]
        bool any_work_hint() const noexcept {
            for (int lv = 0; lv < LEVELS; ++lv) {
                if (globals_[lv].size_approx()) {
                    return true;
                }
            }
            for (std::size_t i = 0; i < n_threads_; ++i) {
                for (int lv = 0; lv < LEVELS; ++lv) {
                    if (ctxs_[i].local[lv].size_approx()) {
                        return true;
                    }
                }
            }
            return false;
        }

        static std::uint32_t xorshift(std::uint32_t& s) noexcept {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return s;
        }

        /// 排空全部未开始的任务, 并收敛与并发提交的竞争残留
        ///
        /// stopping_ 置位后新提交必然被拒, 但"已越过拒绝检查"的在途提交仍可能
        /// 稍后才落队. 若只扫一轮, 这类节点会滞留队列: pending 永不归零,
        /// 之后一切 wait 都将悬挂. 故循环排空直至"队列空且计数归零"连续成立
        /// 两轮 - 提交路径为此零开销, 关闭路径多绕几轮即可收敛
        void drop_all_queued() noexcept {
            // 计数必须随销毁同步扣减: 若留到循环之后, "等待计数归零"与
            // "扣减被销毁节点的计数"互为因果, 将永久自锁
            auto account_drop = [this](node_t* n) noexcept {
                destroy_node(n);
                if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    idle_gen_.fetch_add(1, std::memory_order_release);
                    idle_gen_.notify_all();
                }
            };

            int quiet_rounds = 0;
            while (quiet_rounds < 2) {
                std::size_t got = 0;
                for (int lv = 0; lv < LEVELS; ++lv) {
                    while (auto* n = globals_[lv].pop()) {
                        account_drop(n);
                        ++got;
                    }
                }
                // 他人的 CL deque 只能经由 steal 清空
                for (std::size_t i = 0; i < n_threads_; ++i) {
                    for (int lv = 0; lv < LEVELS; ++lv) {
                        while (auto* n = ctxs_[i].local[lv].steal()) {
                            account_drop(n);
                            ++got;
                        }
                    }
                }
                if (got == 0 && pending_.load(std::memory_order_acquire) == 0) {
                    ++quiet_rounds;
                } else {
                    quiet_rounds = 0;
                    if (got == 0) {
                        std::this_thread::yield(); // 计数由在途任务持有: 让出 CPU 等其收尾
                    }
                }
            }
        }

        void flush_freelists() noexcept {
            for (std::size_t i = 0; i < n_threads_; ++i) {
                while (auto* n = detail::freelist_pop(ctxs_[i].free_head)) {
                    destroy_node(n);
                }
            }
        }

        [[nodiscard]]
        std::uint64_t next_id() noexcept {
            return id_seq_.fetch_add(1, std::memory_order_relaxed) + 1;
        }

        void trace_enqueue(std::uint64_t id, task_priority p) noexcept {
            if constexpr (TRACE) {
                if (hooks_.on_enqueue) {
                    hooks_.on_enqueue({id, phase_kind::enqueue, outcome_kind::completed,
                                       effective_priority(p), static_cast<std::size_t>(-1)});
                }
            }
        }
        void trace_begin(std::uint64_t id, task_priority p) noexcept {
            if constexpr (TRACE) {
                if (hooks_.on_begin) {
                    hooks_.on_begin({id, phase_kind::begin, outcome_kind::completed,
                                     effective_priority(p), detail::tls_worker});
                }
            }
        }
        void trace_end(std::uint64_t id, task_priority p, outcome_kind o) noexcept {
            if constexpr (TRACE) {
                if (hooks_.on_end) {
                    hooks_.on_end(
                        {id, phase_kind::end, o, effective_priority(p), detail::tls_worker});
                }
            }
        }

        [[nodiscard]]
        static task_priority effective_priority(task_priority p) noexcept {
            return PRIORITY ? p : task_priority::normal;
        }

        alignas(64) mutable std::atomic<std::int64_t> pending_{0};
        alignas(64) std::atomic<std::uint64_t> wake_gen_{0};
        alignas(64) mutable std::atomic<std::uint64_t> idle_gen_{0};
        std::atomic<bool> stopping_{false};
        std::atomic<std::uint64_t> id_seq_{0};

        std::unique_ptr<worker_ctx_t[]> ctxs_;
        std::conditional_t<WORKER_CAP != 0, std::inplace_vector<std::jthread, WORKER_CAP>,
                           std::vector<std::jthread>>
            workers_;
        std::array<gq_t, LEVELS> globals_{};
        trace_hooks hooks_;
        std::size_t n_threads_ = 0;
    };

    /// 默认别名: 无特性的基础形态
    using pool = basic_pool<>;

} // namespace concurrent
