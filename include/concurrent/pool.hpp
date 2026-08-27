#pragma once
#include "concurrent/detail/chase_lev.hpp"
#include "concurrent/detail/cpu_relax.hpp"
#include "concurrent/detail/global_queue.hpp"
#include "concurrent/detail/node_cache.hpp"
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
#include <mutex>
#include <new>
#include <ranges>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace concurrent {

    /// 关闭策略
    enum class shutdown_policy : std::uint8_t {
        drain,   ///< 排空全部排队任务后退出(析构默认)
        discard, ///< 丢弃排队任务并以取消语义终结其结果通道; 运行中任务两种
                 ///  策略下都会等待完成(jthread join 的固有语义)
    };

    namespace detail {

        /// worker 线程上下文: 身份 + 空闲节点缓存 + 分层本地 deque
        template <std::size_t Levels, std::size_t LocalCap, std::size_t CacheCap>
        struct alignas(64) worker_ctx {
            node_cache<task_node, CacheCap> cache; ///< MPSC 入 / 单消费者出, 带上限
            std::size_t index = 0;
            std::array<chase_lev_deque<task_node*, LocalCap>, Levels> local{};
        };

        /// 线程内当前池与 worker 身份(嵌套提交路由用). constinit: 常量
        /// 初始化, 免去 TLS 动态初始化守卫检查
        inline constinit thread_local const void* tls_pool = nullptr;
        inline constinit thread_local std::size_t tls_worker = 0;

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
     * @tparam Flags 特性标签: priority / cancellable / trace / worker_cap<N> /
     *                queue_cap<Global, Local>
     */
    template <typename... Flags>
    class basic_pool {
        static_assert((std::size_t{0} + ... + detail::is_worker_cap_flag_v<Flags>) <= 1,
                      "worker_cap<N> may appear at most once");
        static_assert((std::size_t{0} + ... + detail::is_queue_cap_flag_v<Flags>) <= 1,
                      "queue_cap<Global, Local> may appear at most once");

        static constexpr bool PRIORITY = detail::has_priority_v<Flags...>;
        static constexpr bool TRACE = detail::has_trace_v<Flags...>;
        /// cancellable 标签约束"返回取消源的 execute 重载"的可见性;
        /// submit 的 token 感知不受此限制(状态无条件携带取消源)
        static constexpr bool CANCELLABLE_TAG = detail::has_cancellable_v<Flags...>;
        static constexpr int LEVELS = PRIORITY ? 3 : 1;
        static constexpr std::size_t WORKER_CAP = detail::worker_capacity_v<Flags...>;
        /// 本地 deque / 全局环每层容量, queue_cap<Global, Local> 标签可配(2 的幂).
        /// 环满自动落入保序溢出链(不拒绝不阻塞), 故容量只影响内存占用与
        /// 无锁快路径占比, 不影响正确性; 缺省 256 / 1024
        static constexpr std::size_t LOCAL_CAP = detail::queue_local_cap_v<Flags...>;
        static constexpr std::size_t GLOBAL_CAP = detail::queue_global_cap_v<Flags...>;
        static_assert(GLOBAL_CAP != 0 && (GLOBAL_CAP & (GLOBAL_CAP - 1)) == 0,
                      "queue_cap<Global, Local>: Global must be a nonzero power of two");
        static_assert(LOCAL_CAP >= 2 && (LOCAL_CAP & (LOCAL_CAP - 1)) == 0,
                      "queue_cap<Global, Local>: Local must be a power of two >= 2");
        /// 每 worker 空闲节点缓存上限. 无上限时外部线程持续提交会让缓存长度
        /// 随累计任务数单调增长(节点归还进执行者的缓存, 外部生产者永远不来取)
        static constexpr std::size_t NODE_CACHE_CAP = 1024;
        /// stopping_ 置位后 worker 嵌套提交的放行预算: 防"自适应派生"型任务
        /// (派生速率不衰减)在关闭窗口内无限繁殖令 shutdown 永不返回.
        /// 覆盖约 depth-21 满二叉树的派生量, 耗尽退回拒绝(stopped);
        /// 正常运行零触碰
        static constexpr std::int64_t DRAIN_NESTED_BUDGET = 4'000'000;
        static constexpr int PAUSE_BATCH = 16; ///< 每轮探测间的让核步长

        using node_t = detail::task_node;
        using gq_t = detail::global_queue<node_t, GLOBAL_CAP>;
        using worker_ctx_t = detail::worker_ctx<LEVELS, LOCAL_CAP, NODE_CACHE_CAP>;
        using outcome_kind = task_outcome;
        using phase_kind = task_phase;

    public:
        struct options {
            std::size_t threads = 0; ///< 0 -> hardware_concurrency()
            /// 睡眠前忙等时间预算. 任务常成簇到达(嵌套提交, 往返模式), 一小段
            /// 自旋即可吸收"生产者已在路上"的窗口, 免去每次 futex 睡/醒的内核
            /// 往返 - 实测将空池往返 P50 从 ~55µs 降至亚微秒级. 以时间而非
            /// 次数计, 跨主频可移植; 到期仍无任务才真正睡眠. 0 = 不自旋直接
            /// 睡. 另一面: 任务到达间隔稳定小于该值时 N 个 worker 会全程占核,
            /// 稀疏流量的服务型池宜调小
            std::chrono::microseconds spin_budget{64};
            trace_hooks hooks{}; ///< 仅 trace 标签下生效
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
            : hooks_(std::move(opts.hooks)), spin_budget_(opts.spin_budget) {
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
                globals_ = std::make_unique<std::array<gq_t, LEVELS>>();
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
            requires(!std::same_as<std::remove_cvref_t<F>, task_priority>) &&
                    (std::invocable<F, Args...> ||
                     std::invocable<F, std::stop_token, Args...>) // 不可调用对象在重载处即报, 不再爆在深处
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
            requires(PRIORITY) && (std::invocable<F, Args...> ||
                                   std::invocable<F, std::stop_token, Args...>)
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
         * @brief 批量提交: 对区间每个元素提交 f(e), 全部落队后仅唤醒一次
         *
         * 与逐元素 submit 的差异仅在通知摊薄与两阶段提交:
         *  - 阶段一构建全部节点(不入队), 构建期失败整体回滚, 无半成品;
         *  - 阶段二依序入队, 仅 stopping 可败 - 此时已入队者照常运行
         *    (句柄随错误返回丢弃), 未入队者以取消语义终结
         *
         * 元素按值拷贝进各闭包(任务并发执行, 不得共享可变状态); 零拷贝
         * 需求请用 parallel_map/parallel_for. callable 同样支持 stop_token
         * 首参; 以 normal 优先级入队
         */
        template <typename Rng, typename F>
            requires std::ranges::input_range<Rng> &&
                     (std::invocable<F&, std::ranges::range_value_t<Rng>> ||
                      std::invocable<F&, std::stop_token, std::ranges::range_value_t<Rng>>)
        [[nodiscard]]
        auto submit_each(Rng&& rng, F f)
            -> std::expected<std::vector<task<detail::submit_result_t<
                                 F, std::ranges::range_value_t<Rng>>>>,
                             submit_error> {
            using elem_t = std::ranges::range_value_t<Rng>;
            using R = detail::submit_result_t<F, elem_t>;

            std::vector<task<R>> out;
            std::vector<node_t*> staged;

            // 阶段一: 整体构建. 任一失败回滚全部半成品(终结状态防悬挂)
            try {
                if constexpr (std::ranges::sized_range<Rng>) {
                    const auto n = static_cast<std::size_t>(std::ranges::size(rng));
                    out.reserve(n);
                    staged.reserve(n);
                }
                for (auto&& e : rng) {
                    std::shared_ptr<detail::shared_state<R>> st;
                    node_t* node = build_submit_node(task_priority::normal, st, f,
                                                     elem_t(std::forward<decltype(e)>(e)));
                    if (!node) [[unlikely]] {
                        for (auto* n2 : staged) {
                            abandon(n2);
                        }
                        return std::unexpected(submit_error::out_of_memory);
                    }
                    staged.push_back(node);
                    out.emplace_back(std::move(st));
                }
            } catch (...) { // F/元素拷贝的用户异常原样透传, 半成品照旧回收
                for (auto* n2 : staged) {
                    abandon(n2);
                }
                throw;
            }

            // 阶段二: 依序入队, 单次唤醒
            for (std::size_t i = 0; i < staged.size(); ++i) {
                auto ok = enqueue(static_cast<int>(level_of(task_priority::normal)),
                                  staged[i]);
                if (!ok) {
                    for (++i; i < staged.size(); ++i) {
                        abandon(staged[i]); // 从未入队: 终结后销毁
                    }
                    return std::unexpected(ok.error());
                }
                if constexpr (TRACE) { // id 就在手边: out[i] 持有同一份状态
                    trace_enqueue(out[i].st_->id, task_priority::normal);
                }
            }
            if (!staged.empty()) {
                notify_wake();
            }
            return out;
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
        /// 互斥约束: 无 token 也可调用的 callable 一律归普通重载 - 泛型
        /// callable(如 [](auto&&...))对两条路皆可行, 不加排斥则二义
        template <typename F, typename... Args>
            requires(CANCELLABLE_TAG && detail::takes_token_v<F, Args...> &&
                     std::is_nothrow_invocable_v<F, std::stop_token, Args...> &&
                     !std::is_nothrow_invocable_v<F, Args...>)
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
                     std::is_nothrow_invocable_v<F, std::stop_token, Args...> &&
                     !std::is_nothrow_invocable_v<F, Args...>)
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
                const auto now = Clock::now();
                if (now >= tp) {
                    return false;
                }
                // 单轮睡眠不超过剩余时限: 到点即刻复检, 不因固定步长过冲
                std::this_thread::sleep_for(
                    std::min(std::chrono::duration_cast<std::chrono::nanoseconds>(tp - now),
                             std::chrono::nanoseconds(100'000)));
            }
            return true;
        }

        /// 关闭. drain: 排空后退出(默认); discard: 丢弃未开始的任务立即退出
        /// 运行中任务两种策略下都会等待完成(jthread join 的固有语义), discard
        /// 仅跳过排队任务的执行. drain 期间正在运行任务在 worker 内的嵌套
        /// 派生被放行(有限预算, 见 DRAIN_NESTED_BUDGET), 在途计算树得以
        /// 整体完成而非中途断链. 可并发调用: 首个调用者的 policy 生效, 其余
        /// 调用者阻塞至拆除完成后做一次空收敛
        void shutdown(shutdown_policy policy = shutdown_policy::drain) noexcept {
            // seq_cst: 与 enqueue 侧的 Dekker 配对另一半, 保证"在途登记先于
            // 置位"时收敛轮次必能读到 submitting_ > 0
            const bool first = !stopping_.exchange(true, std::memory_order_seq_cst);
            if (first && policy == shutdown_policy::drain) {
                wait();
            }

            // drop_all 与 flush_freelists 的空闲链消费依赖"单消费者"前提,
            // 并发拆除会破坏之; workers_.clear() 更是裸数据竞争 -> 串行化
            std::lock_guard lock{shutdown_mtx_};
            drop_all_queued(); // discard 的主体; drain 时仅收敛在途提交的残留
            // 只有排空收敛之后才允许 worker 退出: 在此之前它们是队列的唯一
            // 消费者, 提前离场会让"已越过拒绝检查的在途提交"无人认领
            quitting_.store(true, std::memory_order_release);
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
        /// trace 专用捕获(任务编号 + 优先级). 标签关闭时折叠为 monostate,
        /// 任务闭包除 self/实参之外零固定开销, SBO 预算全部留给用户捕获
        struct trace_env {
            std::uint64_t id = 0;
            task_priority prio = task_priority::normal;
        };
        using trace_env_t = std::conditional_t<TRACE, trace_env, std::monostate>;

        [[nodiscard]]
        trace_env_t make_trace_env([[maybe_unused]] std::uint64_t id,
                                   [[maybe_unused]] task_priority prio) noexcept {
            if constexpr (TRACE) {
                return trace_env{.id = id, .prio = prio};
            } else {
                return {};
            }
        }

        /// 构建 submit 型节点(状态 + 闭包 + 丢弃终结钩子), 未入队
        /// @return 空 = 仅因 bad_alloc; 其余异常(F/实参拷贝)原样传播
        template <typename R, typename F, typename... Args>
        node_t* build_submit_node(task_priority prio,
                                  std::shared_ptr<detail::shared_state<R>>& st, F&& f,
                                  Args&&... args) {
            try {
                st = detail::make_state<R>();
            } catch (const std::bad_alloc&) {
                return nullptr;
            }
            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return nullptr; // st 随作用域释放
            }

            if constexpr (TRACE) {
                st->id = next_id();
            }

            auto* self = this;
            const trace_env_t env = make_trace_env(st->id, prio);
            node->body = [st, self, env, f = std::forward<F>(f),
                          ... a = std::forward<Args>(args)]() mutable noexcept {
                self->run_task_body(*st, env, [&]() -> R {
                    if constexpr (detail::takes_token_v<F, Args...>) {
                        return std::invoke(std::move(f), st->source.get_token(), std::move(a)...);
                    } else {
                        return std::invoke(std::move(f), std::move(a)...);
                    }
                });
            };

            // 关闭丢弃路径的终结钩子: 以取消语义收尾共享状态并发布完成,
            // 使持有 task 句柄的一方经错误通道观测到 operation_cancelled
            node->discard_ctx = st.get();
            node->discard = [](void* p) noexcept {
                static_cast<detail::shared_state<R>*>(p)->set_cancelled();
                static_cast<detail::shared_state<R>*>(p)->finish();
            };
            return node;
        }

        template <typename R, typename F, typename... Args>
        std::expected<task<R>, submit_error> submit_impl(task_priority prio, F&& f,
                                                         Args&&... args) {
            std::shared_ptr<detail::shared_state<R>> st;
            node_t* node =
                build_submit_node(prio, st, std::forward<F>(f), std::forward<Args>(args)...);
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }

            if (auto ok = route(static_cast<int>(level_of(prio)), node); !ok) {
                return std::unexpected(ok.error());
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
            std::uint64_t id = 0;
            if constexpr (TRACE) {
                id = next_id(); // trace 关闭时零开销: 不触碰 id_seq_
            }
            const trace_env_t env = make_trace_env(id, prio);
            node->body = [self, env, f = std::forward<F>(f),
                          ... a = std::forward<Args>(args)]() mutable noexcept {
                if constexpr (TRACE) {
                    self->trace_begin(env.id, env.prio);
                }
                std::invoke(std::move(f), std::move(a)...); // noexcept 由 concepts 强制
                if constexpr (TRACE) {
                    self->trace_end(env.id, env.prio, outcome_kind::completed);
                }
                self->complete_one();
            };

            if (auto ok = route(static_cast<int>(level_of(prio)), node); !ok) {
                return std::unexpected(ok.error());
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
            std::uint64_t id = 0;
            if constexpr (TRACE) {
                id = next_id(); // trace 关闭时零开销: 不触碰 id_seq_
            }
            const trace_env_t env = make_trace_env(id, prio);
            node->body = [self, env, source, f = std::forward<F>(f),
                          ... a = std::forward<Args>(args)]() mutable noexcept {
                if constexpr (TRACE) {
                    self->trace_begin(env.id, env.prio);
                }
                if (!source->stop_requested()) {
                    std::invoke(std::move(f), source->get_token(), std::move(a)...);
                    if constexpr (TRACE) {
                        self->trace_end(env.id, env.prio, outcome_kind::completed);
                    }
                } else {
                    if constexpr (TRACE) {
                        self->trace_end(env.id, env.prio, outcome_kind::cancelled);
                    }
                }
                self->complete_one();
            };

            if (auto ok = route(static_cast<int>(level_of(prio)), node); !ok) {
                return std::unexpected(ok.error());
            }

            trace_enqueue(id, prio);
            return *source; // 拷贝句柄与闭包共享同一停止状态; 不可 move(move
                            // 会掏空闭包持有的源)
        }

        /// 有状态任务的外壳: 取消检查 -> 异常捕获 -> 结果发布 -> 续延内联 -> 计数收尾
        template <typename State, typename Invoker>
        void run_task_body(State& st, const trace_env_t& env, Invoker&& invoke) noexcept {
            outcome_kind o = outcome_kind::completed;

            if constexpr (TRACE) {
                trace_begin(env.id, env.prio);
            }
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
            if constexpr (TRACE) {
                trace_end(env.id, env.prio, o);
            }
            st.finish(); // 先发布完成再跑续延(续延可能回查本状态)
            complete_one();
        }

        /// 入队, 不含唤醒(批量提交方据此摊薄通知成本)
        /// @return 空 = 成功; 非空 = submit_error
        std::expected<void, submit_error> enqueue(int level, node_t* node) noexcept {
            // 乐观预检: 已停则直接拒绝, 不触碰任何计数. 若被拒提交也抬高
            // pending_, 持续重试的生产者会把 drop_all_queued 的收敛条件
            // 无限重置 - 生产者等 shutdown 返回, shutdown 等生产者停手
            //
            // 唯一例外: 本池 worker 的嵌套提交放行(有限预算). 正在运行的
            // fork-join 型任务在 shutdown 期间派生子任务, 语义上属于
            // "停机前已接受工作"的延续, 拒绝它会让 drain 变相丢弃在途
            // 计算树; 放行不登记 submitting_: 嵌套提交的父任务持有
            // pending_ 计数直至派生返回, wait/drop 的收敛判据不可能
            // 越过它静默(见 nested_submit_permitted)
            bool registered = false;
            if (stopping_.load(std::memory_order_acquire)) [[unlikely]] {
                if (!nested_submit_permitted()) [[unlikely]] {
                    // 统一出口 abandon: submit 路径节点入队前已挂 discard 钩子,
                    // 须以取消语义终结其共享状态; execute 路径节点无钩子, 等价销毁
                    abandon(node);
                    return std::unexpected(submit_error::stopped);
                }
            } else {
                // Dekker 配对(两侧皆 seq_cst): 在途登记先于复查. 若复查仍未
                // 见置位, 依全序"登记 < 复查 < 置位 < 收敛轮读", 轮次必能读到
                // submitting_ > 0 而继续等待, "越过检查却在途"的提交不会被漏
                // 计; 反之复查见置位则撤销登记, 走拒绝或嵌套放行, 不留痕迹
                submitting_.fetch_add(1, std::memory_order_seq_cst);
                if (stopping_.load(std::memory_order_seq_cst)) [[unlikely]] {
                    submitting_.fetch_sub(1, std::memory_order_seq_cst);
                    if (!nested_submit_permitted()) [[unlikely]] {
                        abandon(node);
                        return std::unexpected(submit_error::stopped);
                    }
                } else {
                    registered = true;
                }
            }
            // pending_ 只计已入队/运行中的工作: wait() 与 drain 的计数语义
            // 自此与"正在尝试提交"解耦
            pending_.fetch_add(1, std::memory_order_acq_rel);

            // worker 内嵌套提交: 优先本地 deque(LIFO 缓存热度)
            if (detail::tls_pool == this && ctxs_[detail::tls_worker].local[level].push(node))
                [[likely]] {
                if (registered) {
                    submitting_.fetch_sub(1, std::memory_order_seq_cst);
                }
                return {};
            }
            // 外部提交或本地溢出: 全局队列(环满自动落入溢出链, 永不失败, 永不阻塞)
            (*globals_)[level].push(node);
            if (registered) {
                submitting_.fetch_sub(1, std::memory_order_seq_cst);
            }
            return {};
        }

        /// stopping_ 置位后本条提交是否放行: 仅本池 worker 的嵌套提交,
        /// 消耗有限预算(DRAIN_NESTED_BUDGET), 其余一律拒绝.
        /// 放行依赖的不变式: 调用方(worker)必在执行某个任务体, 而任务体
        /// 在完成前持有 pending_ 计数 -> 收敛判据(pending 归零)不可能
        /// 在派生窗口内静默成立, 在途子节点必被计入或被后续轮次消费
        [[nodiscard]]
        bool nested_submit_permitted() noexcept {
            if (detail::tls_pool != static_cast<const void*>(this)) {
                return false;
            }
            return drain_nested_budget_.fetch_sub(1, std::memory_order_relaxed) > 0;
        }

        /// 入队并唤醒一个 worker
        /// @return 空 = 成功; 非空 = submit_error
        std::expected<void, submit_error> route(int level, node_t* node) noexcept {
            if (auto ok = enqueue(level, node); !ok) {
                return ok;
            }
            notify_wake();
            return {};
        }

        /// 执行一个节点并回收. body 在执行后立即析构 - 否则其捕获的实参与
        /// 共享状态引用会一直存活到该节点被复用/销毁, 形成可观的内存滞留
        void execute_node(node_t* n, std::size_t worker) noexcept {
            n->body();
            n->body.reset();
            // 丢弃钩子只服务于"从未执行"的节点. 一旦执行完毕, 它指向的共享
            // 状态随时可能被释放(body 析构即放掉最后一份引用), 故必须与 body
            // 一同清除: execute 路径复用节点时不覆写这两个字段, 陈旧钩子会被
            // 带进队列, 关闭丢弃时 abandon 便在已释放的状态上写入
            n->discard = nullptr;
            n->discard_ctx = nullptr;
            // 缓存已满则直接归还分配器: 节点此刻已是干净空壳
            if (!ctxs_[worker].cache.push(n)) {
                delete n;
            }
        }

        void notify_wake() noexcept {
            // 稳态下(自旋预算内)没有 worker 在睡, 这次所有生产者共享一行
            // 的 fetch_add + futex 纯属浪费 - 多生产者 x8 的主要拖累之一.
            // 不丢唤醒协议(两侧 seq_cst 栅栏, Eigen/taskflow notifier 路数):
            // 生产者 push 后过栅栏再读 sleepers_; worker 先登记再过栅栏
            // 复查. 若复查错过 push, 依全序登记必先于本读 - 看到登记便
            // 照常唤醒, 丢失唤醒不可能
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (sleepers_.load(std::memory_order_acquire) != 0) {
                wake_gen_.fetch_add(1, std::memory_order_release);
                wake_gen_.notify_one(); // 单任务只唤醒一个 worker, 避免 notify_all 惊群
            }
        }

        /// 单个任务完成的统一收尾: 计数归零者负责推进空闲代际并唤醒等待者.
        /// wait()/shutdown(drain) 挂在 idle_gen_ 上, 任何一条完成路径漏掉
        /// 这一步都会让它们永久沉睡 - 故所有收尾必须收口于此
        void complete_one() noexcept {
            if (pending_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                idle_gen_.fetch_add(1, std::memory_order_release);
                idle_gen_.notify_all();
            }
        }

        static constexpr int level_of(task_priority p) noexcept {
            // 枚举 low..high 升序而层索引反向(高优先级层号小): 一行完成映射
            return PRIORITY ? LEVELS - 1 - static_cast<int>(std::to_underlying(p)) : 0;
        }

        node_t* acquire_node() noexcept {
            if (detail::tls_pool == static_cast<const void*>(this)) {
                if (auto* n = ctxs_[detail::tls_worker].cache.pop()) [[likely]] {
                    return n;
                }
            }
            return new (std::nothrow) node_t{};
        }

        /// abandon 的实现细节: 抹掉 body 后归还分配器. 不允许被直接调用 -
        /// 节点可能挂着 discard 钩子, 绕过 abandon 即共享状态永不终结
        void destroy_node(node_t* n) noexcept {
            n->body.reset();
            delete n;
        }

        /// 节点回收的统一出口: 未执行过的节点挂着 discard 钩子, 先以取消
        /// 语义终结其共享状态(使等待方经错误通道观测 operation_cancelled
        /// 而非永久等待)再销毁; 已执行的节点钩子已被清除, 等价于直接销毁
        void abandon(node_t* n) noexcept {
            if (n->discard) {
                n->discard(n->discard_ctx);
            }
            destroy_node(n);
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
            stopping_.store(true, std::memory_order_seq_cst);
            quitting_.store(true, std::memory_order_release);
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
                node_t* n = try_acquire(idx, seed);
                if (!n && spin_budget_ > std::chrono::microseconds{0}) {
                    // 有界自旋: 每批让核后重新探测全队列; 预算耗尽(或配置为 0)
                    // 才进入 futex 睡眠协议
                    const auto deadline = std::chrono::steady_clock::now() + spin_budget_;
                    while (!n) {
                        for (int s = 0; s < PAUSE_BATCH && !n; ++s) {
                            detail::cpu_relax();
                            n = try_acquire(idx, seed);
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
                // 免竞态睡眠协议: 代际必须先于"退出条件"与"有无工作"读取.
                // 二者的置位方都在改动后递增 wake_gen_, 故任何发生在本次读取
                // 之后的置位都会让下面的 wait(g) 立即返回; 反之若代际未变, 则
                // 置位方的写入必已先于本次 acquire 读可见, 下面两项检查看得到
                const std::uint64_t g = wake_gen_.load(std::memory_order_acquire);

                // 判据是 quitting_ 而非 stopping_: 后者仅表示"拒绝新提交",
                // 此时 drain 还要靠 worker 把队列消费干净. 若以 stopping_ 退出,
                // worker 会在 shutdown(drain) 的 wait() 期间集体离场, 而"已越过
                // 拒绝检查"的在途提交随后才落队 -> pending 永不归零, wait() 悬挂
                //
                // stop_token 是兜底: jthread 析构会 request_stop 后 join,
                // 即便某条路径漏了 quitting_ 也不会挂死在 join 上
                if (quitting_.load(std::memory_order_acquire) || stop.stop_requested())
                    [[unlikely]] {
                    break;
                }
                // 睡前最后一搏用 try_acquire 而非只读的 any_work_hint:
                // 扫的还是同一批队列, 但有活直接拿走执行而非"看到却空手
                // 回去再来一轮", 全空才睡 - 免竞态协议不变(代际已先读).
                //
                // 睡眠登记与复查之间必须隔一道 seq_cst 栅栏(与 notify_wake
                // 的生产者侧配对): 若复查错过刚 push 的活, 依全序本次登记
                // 必先于生产者读 sleepers_ - 后者看到登记便照常唤醒; 若登记
                // 晚于该读, 则 push 必已对复查可见, 复查不会错过. 两侧必居
                // 其一, 丢失唤醒不可能
                sleepers_.fetch_add(1, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (node_t* m = try_acquire(idx, seed)) {
                    sleepers_.fetch_sub(1, std::memory_order_relaxed);
                    execute_node(m, idx);
                    continue;
                }
                wake_gen_.wait(g); // 全空则睡; 推送方 bump 代际唤醒
                sleepers_.fetch_sub(1, std::memory_order_relaxed);
            }

            detail::tls_pool = nullptr;
        }

        /// 取一个任务: 自有本地(高->低) -> 全局(高->低) -> 窃取他人本地(FIFO)
        /// worker 身份由调用方传入: worker_main 手上就有 idx, 免去 TLS 读
        [[nodiscard]]
        node_t* try_acquire(std::size_t idx, std::uint32_t& seed) noexcept {
            auto& self = ctxs_[idx];
            for (int lv = 0; lv < LEVELS; ++lv) {
                if (auto* n = self.local[lv].pop()) [[likely]]
                {
                    return n;
                }
            }
            for (int lv = 0; lv < LEVELS; ++lv) {
                if (auto* n = (*globals_)[lv].pop()) {
                    return n;
                }
            }
            const std::size_t start = xorshift(seed) % n_threads_;
            std::size_t vi = start;
            for (std::size_t k = 0; k < n_threads_; ++k) {
                if (++vi == n_threads_) {
                    vi = 0;
                }
                if (vi == idx) {
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
                abandon(n);
                complete_one();
            };

            int quiet_rounds = 0;
            while (quiet_rounds < 2) {
                std::size_t got = 0;
                for (int lv = 0; lv < LEVELS; ++lv) {
                    while (auto* n = (*globals_)[lv].pop()) {
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
                // submitting_ 的读亦为 Dekker 配对的一环(seq_cst): 只要存在
                // "越过检查但尚未入队"的生产者, 本轮即不静默 - 置位后不再有
                // 新的进入者, 该计数单调收敛, 两轮静默从而是可达成的
                if (got == 0 && pending_.load(std::memory_order_acquire) == 0 &&
                    submitting_.load(std::memory_order_seq_cst) == 0) {
                    ++quiet_rounds;
                } else {
                    quiet_rounds = 0;
                    if (got == 0) {
                        std::this_thread::yield(); // 计数由在途任务/提交持有: 让出 CPU 等其收尾
                    }
                }
            }
        }

        void flush_freelists() noexcept {
            for (std::size_t i = 0; i < n_threads_; ++i) {
                while (auto* n = ctxs_[i].cache.pop()) {
                    abandon(n); // 缓存节点钩子已清, 经统一出口仅为结构一致
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
        /// 睡眠中(或正在入睡)的 worker 数. notify_wake 据此在稳态下免去
        /// 全局代际 RMW; 与 worker 侧的"登记 -> 栅栏 -> 复查"构成不丢唤醒
        /// 协议, 详见 notify_wake 与 worker_main 的注释
        alignas(64) std::atomic<std::int64_t> sleepers_{0};
        /// 提交在途计数: "乐观预检通过"到"入队完成"之间的生产者个数.
        /// 与 pending_ 分工 - 它只兜住竞态窗口(置位瞬间仍有生产者越过
        /// 检查), 不计已入队的工作; drain 收敛据此等完最后的在途提交
        alignas(64) std::atomic<std::int64_t> submitting_{0};
        /// route 的每次提交都 acquire 读 stopping_; 与高频递增的 id_seq_
        /// 各占一行, 避免读写互弹缓存行
        alignas(64) std::atomic<bool> stopping_{false};
        /// worker 的退出判据. 与 stopping_ 分离: 前者一置位即拒绝新提交, 而
        /// worker 必须继续消费到排空收敛之后才可离场
        alignas(64) std::atomic<bool> quitting_{false};
        alignas(64) std::atomic<std::uint64_t> id_seq_{0};

        /// 仅拆除路径使用; 与热路径原子量无共享行
        std::mutex shutdown_mtx_;
        /// drain 期嵌套提交放行余额(见 DRAIN_NESTED_BUDGET), 仅 stopping_
        /// 置位后触碰
        std::atomic<std::int64_t> drain_nested_budget_{DRAIN_NESTED_BUDGET};

        std::unique_ptr<worker_ctx_t[]> ctxs_;
        std::conditional_t<WORKER_CAP != 0, std::inplace_vector<std::jthread, WORKER_CAP>,
                           std::vector<std::jthread>>
            workers_;
        /// 全局环体积随容量线性增长(实测 1024 槽 ≈ 64KiB/层, priority 下三层),
        /// 堆分配以保持池对象本身可安全栈上构造; 热路径仅多一次指针解引用
        std::unique_ptr<std::array<gq_t, LEVELS>> globals_;
        trace_hooks hooks_;
        std::size_t n_threads_ = 0;
        /// 睡前忙等预算(options::spin_budget, 构造后只读)
        std::chrono::microseconds spin_budget_{64};
    };

    /// 默认别名: 无特性的基础形态
    using pool = basic_pool<>;

} // namespace concurrent
