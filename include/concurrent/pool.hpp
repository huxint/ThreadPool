#pragma once
#include "concurrent/detail/chase_lev.hpp"
#include "concurrent/detail/cpu_relax.hpp"
#include "concurrent/detail/global_queue.hpp"
#include "concurrent/detail/mpmc_ring.hpp"
#include "concurrent/detail/node_cache.hpp"
#include "concurrent/tags.hpp"
#include "concurrent/task.hpp"
#include "concurrent/trace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
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
     * 取消: 每个任务状态都带取消标志, 未开跑即取消的任务体被跳过并标记
     * operation_cancelled; 真正的 stop_source 仅在 callable 轮询 token 时实体化
     *
     * @tparam Flags 特性标签: priority / cancellable / trace / worker_cap<N> /
     *                queue_cap<Global, Local>
     */
    template <typename... Flags>
    class basic_pool {
        static_assert((0uz + ... + detail::is_worker_cap_flag_v<Flags>) <= 1,
                      "worker_cap<N> may appear at most once");
        static_assert((0uz + ... + detail::is_queue_cap_flag_v<Flags>) <= 1,
                      "queue_cap<Global, Local> may appear at most once");

        static constexpr bool PRIORITY = detail::has_priority_v<Flags...>;
        static constexpr bool TRACE = detail::has_trace_v<Flags...>;
        /// cancellable 标签约束"返回取消源的 execute 重载"的可见性;
        /// submit 的 token 感知不受此限制(状态自带取消标志)
        static constexpr bool CANCELLABLE_TAG = detail::has_cancellable_v<Flags...>;
        static constexpr int LEVELS = PRIORITY ? 3 : 1;
        static constexpr std::size_t WORKER_CAP = detail::worker_capacity_v<Flags...>;
        /// 本地 deque / 全局环每层容量, queue_cap<Global, Local> 标签可配(2 的幂).
        /// 环满自动落入保序溢出链(不拒绝不阻塞), 故容量只影响内存占用与
        /// 无锁快路径占比, 不影响正确性; 大环吸收多生产者积压, 避免溢出链
        /// 自旋锁争用. 缺省 256 / 65536
        static constexpr std::size_t LOCAL_CAP = detail::queue_local_cap_v<Flags...>;
        static constexpr std::size_t GLOBAL_CAP = detail::queue_global_cap_v<Flags...>;
        static_assert(std::has_single_bit(GLOBAL_CAP),
                      "queue_cap<Global, Local>: Global must be a nonzero power of two");
        static_assert(LOCAL_CAP >= 2 && std::has_single_bit(LOCAL_CAP),
                      "queue_cap<Global, Local>: Local must be a power of two >= 2");
        /// 每 worker 空闲节点缓存上限. 无上限时外部线程持续提交会让缓存长度
        /// 随累计任务数单调增长(节点归还进执行者的缓存, 外部生产者永远不来取)
        static constexpr std::size_t NODE_CACHE_CAP = 1024;
        /// 全局空闲节点池上限: worker 本地缓存溢出时归还于此, 供外部生产者
        /// 跨线程复用. 无此环节则每任务一次跨线程 free, 实测 fire-and-forget
        /// 吞吐掉约 4 倍
        static constexpr std::size_t NODE_POOL_CAP = 4096;
        /// stopping 置位后 worker 嵌套提交的放行预算: 防"自适应派生"型任务
        /// (派生速率不衰减)在关闭窗口内无限繁殖令 shutdown 永不返回.
        /// 覆盖约 depth-21 满二叉树的派生量, 耗尽退回拒绝(stopped);
        /// 正常运行零触碰
        static constexpr std::int64_t DRAIN_NESTED_BUDGET = 4'000'000;
        static constexpr int PAUSE_BATCH = 16; ///< 每轮探测间的让核步长
        /// idle_state_ 的两个计数单位(高 32 位自旋者, 低 32 位睡眠者);
        /// worker 数上限 65536, 两侧计数均不可能溢出各自的 32 位
        static constexpr std::uint64_t SPINNER_UNIT = std::uint64_t{1} << 32;
        static constexpr std::uint64_t SLEEPER_UNIT = 1;
        /// state_ 的位布局(见成员声明)
        static constexpr std::uint64_t STOPPING_BIT = 1;
        static constexpr std::uint64_t PENDING_UNIT = 2;

        [[nodiscard]]
        static constexpr std::uint64_t spinner_count(std::uint64_t s) noexcept {
            return s >> 32;
        }
        [[nodiscard]]
        static constexpr std::uint64_t sleeper_count(std::uint64_t s) noexcept {
            return s & 0xFFFF'FFFFull;
        }

        [[nodiscard]]
        std::uint64_t pending_count() const noexcept {
            return state_.load(std::memory_order_acquire) >> 1;
        }

        using node_t = detail::task_node;
        using gq_t = detail::global_queue<node_t, GLOBAL_CAP>;
        using worker_ctx_t = detail::worker_ctx<LEVELS, LOCAL_CAP, NODE_CACHE_CAP>;
        using node_pool_t = detail::mpmc_ring<node_t*, NODE_POOL_CAP>;
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
            : hooks_(take_hooks(opts)), spin_budget_(opts.spin_budget) {
            n_threads_ = opts.threads
                             ? opts.threads
                             : std::max<std::size_t>(std::jthread::hardware_concurrency(), 1uz);
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
                node_pool_ = std::make_unique<node_pool_t>();
                globals_ = std::make_unique<std::array<gq_t, LEVELS>>();
                spawn_workers();
            } catch (...) {
                // 已就位的 worker 只在看到停止信号后才退出循环; 若跳过这步,
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
                notify_wake_n(staged.size());
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
            while (pending_count() != 0) {
                std::uint32_t g = idle_gen_.load(std::memory_order_acquire);
                if (pending_count() == 0) {
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
            while (pending_count() != 0) {
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
            // 与 enqueue 的占坑 RMW 同处一个字: 修改序全序保证"置位之前占坑
            // 成功"的在途提交必被收敛轮次看到(见 enqueue)
            const bool first = (state_.fetch_or(STOPPING_BIT, std::memory_order_acq_rel) &
                                STOPPING_BIT) == 0;
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
            return (state_.load(std::memory_order_acquire) & STOPPING_BIT) == 0;
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
                if constexpr (detail::takes_token_v<F, Args...>) {
                    // 只有轮询 token 的任务需要真正的 stop_source(一次堆分配);
                    // 其余任务的取消由状态内的原子标志承载. 此刻状态尚未交给
                    // 任何其他线程, 实体化不与并发的 request_stop 竞争
                    st->enable_stop();
                }
            } catch (const std::bad_alloc&) {
                return nullptr;
            }

            if constexpr (TRACE) {
                st->id = next_id();
            }
            auto* self = this;
            const trace_env_t env = make_trace_env(st->id, prio);

            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return nullptr; // st 随作用域释放
            }
            // 闭包就地构造在节点的槽里: 免去一次移动构造 + 析构. 构建
            // (F/实参拷贝, sbo_function 堆模式分配)是本函数唯一可能抛的
            // 用户代码段, 抛出时节点仍是干净空壳, 原样归还即可
            try {
                node->body.emplace_with([&] {
                    return [st, self, env, f = std::forward<F>(f),
                            ... a = std::forward<Args>(args)]() mutable noexcept {
                        self->run_task_body(*st, env, [&]() -> R {
                            if constexpr (detail::takes_token_v<F, Args...>) {
                                return std::invoke(std::move(f), st->get_token(), std::move(a)...);
                            } else {
                                return std::invoke(std::move(f), std::move(a)...);
                            }
                        });
                    };
                });
            } catch (...) { // 构造期异常原样传播(见 build_submit_node @return)
                release_node(node);
                throw;
            }

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
            auto* self = this;
            std::uint64_t id = 0;
            if constexpr (TRACE) {
                id = next_id(); // trace 关闭时零开销: 不触碰 id_seq_
            }
            const trace_env_t env = make_trace_env(id, prio);

            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }
            // 同 build_submit_node: 闭包就地构造, 构建失败原样归还节点
            try {
                node->body.emplace_with([&] {
                    return [self, env, f = std::forward<F>(f),
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
                });
            } catch (...) {
                release_node(node);
                throw;
            }

            if (auto ok = route(static_cast<int>(level_of(prio)), node); !ok) {
                return std::unexpected(ok.error());
            }

            trace_enqueue(id, prio);
            return {};
        }

        template <typename F, typename... Args>
        std::expected<std::stop_source, submit_error>
        execute_cancellable_impl(task_priority prio, F&& f, Args&&... args) {
            // 取消源生命周期随闭包: 调用方句柄失效后任务仍可安全查询
            auto source = std::make_shared<std::stop_source>();
            auto* self = this;
            std::uint64_t id = 0;
            if constexpr (TRACE) {
                id = next_id(); // trace 关闭时零开销: 不触碰 id_seq_
            }
            const trace_env_t env = make_trace_env(id, prio);

            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }
            // 同上面两条提交路径: 闭包就地构造, 构建失败原样归还节点
            try {
                node->body.emplace_with([&] {
                    return [self, env, source, f = std::forward<F>(f),
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
                });
            } catch (...) {
                release_node(node);
                throw;
            }

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
            if (st.stop_requested()) {
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
            // 占坑与关闭判定合成一次 RMW: 两侧都是 state_ 上的读改写, 修改序
            // 天然全序 —— 读回的 stopping 位为 0 即本次占坑必排在 fetch_or
            // (置位)之前, 关闭方此后对 state_ 的任何读都会看到这份 pending,
            // 收敛轮次不会在"越过检查却尚未入队"的窗口里静默
            //
            // 前置的乐观预检只为"已停"这一稳定态服务: 被拒的提交不得抬高
            // pending, 否则持续重试的生产者会把 drop_all_queued 的收敛条件
            // 无限重置 - 生产者等 shutdown 返回, shutdown 等生产者停手
            std::uint64_t prev = state_.load(std::memory_order_acquire);
            if ((prev & STOPPING_BIT) == 0) [[likely]] {
                prev = state_.fetch_add(PENDING_UNIT, std::memory_order_acq_rel);
                if ((prev & STOPPING_BIT) != 0) [[unlikely]] {
                    complete_one(); // 置位赶在占坑之前: 撤销, 不留痕迹
                }
            }
            // 已停的唯一例外: 本池 worker 的嵌套提交放行(有限预算). 正在运行
            // 的 fork-join 型任务在 shutdown 期间派生子任务, 语义上属于"停机前
            // 已接受工作"的延续, 拒绝它会让 drain 变相丢弃在途计算树
            if ((prev & STOPPING_BIT) != 0) [[unlikely]] {
                if (!nested_submit_permitted()) [[unlikely]] {
                    // 统一出口 abandon: submit 路径节点入队前已挂 discard 钩子,
                    // 须以取消语义终结其共享状态; execute 路径节点无钩子, 等价销毁
                    abandon(node);
                    return std::unexpected(submit_error::stopped);
                }
                state_.fetch_add(PENDING_UNIT, std::memory_order_acq_rel);
            }

            // worker 内嵌套提交: 优先本地 deque(LIFO 缓存热度)
            if (detail::tls_pool == this && ctxs_[detail::tls_worker].local[level].push(node))
                [[likely]] {
                return {};
            }
            // 外部提交或本地溢出: 全局队列(环满自动落入溢出链, 永不失败, 永不阻塞)
            (*globals_)[level].push(node);
            return {};
        }

        /// stopping 置位后本条提交是否放行: 仅本池 worker 的嵌套提交,
        /// 消耗有限预算(DRAIN_NESTED_BUDGET), 其余一律拒绝.
        /// 放行依赖的不变式: 调用方(worker)必在执行某个任务体, 而任务体
        /// 在完成前持有 pending 计数 -> 收敛判据(pending 归零)不可能
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
            recycle_node(n, worker);
        }

        /// 干净空壳节点的归还: 本地缓存(嵌套提交复用) -> 全局空闲池(外部
        /// 生产者跨线程复用) -> 分配器
        void recycle_node(node_t* n, std::size_t worker) noexcept {
            if (!ctxs_[worker].cache.push(n)) {
                if (!node_pool_->try_push(n)) {
                    delete n;
                }
            }
        }

        /// 提交路径上的归还(闭包构建失败): 调用方未必是本池 worker
        void release_node(node_t* n) noexcept {
            if (detail::tls_pool == static_cast<const void*>(this)) {
                recycle_node(n, detail::tls_worker);
            } else if (!node_pool_->try_push(n)) {
                delete n;
            }
        }

        void notify_wake() noexcept {
            // 不丢唤醒协议(两侧 seq_cst 栅栏, Eigen/taskflow notifier 路数):
            // 生产者 push 后过栅栏再读闲置状态; worker 先登记(自旋者或睡眠者)
            // 再过栅栏复查队列. 两道 seq_cst 栅栏在全序 S 中必分先后:
            //  - 生产者栅栏在先 -> worker 的复查必看到本次 push;
            //  - worker 栅栏在先 -> 生产者的读必看到 worker 的登记变更.
            // 二者必居其一, 故"见到自旋者就跳过唤醒"不会丢失唤醒: 该自旋者
            // 要么在自旋中扫到这个任务, 要么在入睡前的复查里扫到
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const std::uint64_t s = idle_state_.load(std::memory_order_acquire);
            if (spinner_count(s) == 0 && sleeper_count(s) != 0) {
                wake_gen_.fetch_add(1, std::memory_order_release);
                wake_gen_.notify_one(); // 单任务只唤醒一个 worker, 避免 notify_all 惊群
            }
        }

        /// 批量入队后的唤醒: 一批 n 个任务至多需要 n 个 worker. 单次
        /// notify_one 只唤醒一个, 整批落队却只叫醒一人时其余 worker 会一直
        /// 睡到该 worker 逐个消费完; 自旋者已在扫队列, 只为其余的活叫人
        void notify_wake_n(std::size_t n) noexcept {
            if (n <= 1) [[unlikely]] {
                notify_wake();
                return;
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            const std::uint64_t s = idle_state_.load(std::memory_order_acquire);
            const std::uint64_t sleepers = sleeper_count(s);
            const std::uint64_t spinners = spinner_count(s);
            if (sleepers == 0 || n <= spinners) {
                return;
            }
            wake_gen_.fetch_add(1, std::memory_order_release);
            const std::uint64_t want = n - spinners;
            if (want >= sleepers) {
                wake_gen_.notify_all();
                return;
            }
            for (std::uint64_t i = 0; i < want; ++i) {
                wake_gen_.notify_one();
            }
        }

        /// 单个任务完成的统一收尾: 计数归零者负责推进空闲代际并唤醒等待者.
        /// wait()/shutdown(drain) 挂在 idle_gen_ 上, 任何一条完成路径漏掉
        /// 这一步都会让它们永久沉睡 - 故所有收尾必须收口于此
        void complete_one() noexcept {
            if ((state_.fetch_sub(PENDING_UNIT, std::memory_order_acq_rel) >> 1) == 1) {
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
            // 跨线程复用: worker 归还的空闲节点经全局池回流转给外部生产者,
            // 省去每任务一次 malloc + 一次跨线程 free(否则 fire-and-forget
            // 单生产者吞吐掉约 4 倍)
            if (auto* n = node_pool_->try_pop()) [[likely]] {
                return n;
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
            state_.fetch_or(STOPPING_BIT, std::memory_order_acq_rel);
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
                    bool spinning = false;
                    while (!n) {
                        for (int s = 0; s < PAUSE_BATCH && !n; ++s) {
                            detail::cpu_relax();
                            n = try_acquire(idx, seed);
                        }
                        if (n || std::chrono::steady_clock::now() >= deadline) {
                            break;
                        }
                        if (!spinning) {
                            // 首批探测就落空才向 idle_state_ 登记为自旋者:
                            // 生产者见到自旋者即可跳过 futex 唤醒(稀疏到达下
                            // 省掉的正是"每次提交一次系统调用", 见 notify_wake
                            // 的不丢唤醒论证). 满载时任务间的取活间隙极短,
                            // 那种一闪而过的空转不值得写这条共享缓存行
                            idle_state_.fetch_add(SPINNER_UNIT, std::memory_order_relaxed);
                            spinning = true;
                        }
                    }
                    if (spinning) {
                        const std::uint64_t prev =
                            idle_state_.fetch_sub(SPINNER_UNIT, std::memory_order_acq_rel);
                        // 最后一个自旋者带着活离场: 在此期间生产者的推送都因
                        // "有自旋者"而免了唤醒, 若队列里还有剩余, 需由本线程
                        // 接力唤醒一个睡眠者, 否则它们要等到本任务跑完才被认领
                        if (n && spinner_count(prev) == 1 && more_work_hint(idx)) {
                            notify_wake();
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
                const std::uint32_t g = wake_gen_.load(std::memory_order_acquire);

                // 判据是 quitting_ 而非 stopping 位: 后者仅表示"拒绝新提交",
                // 此时 drain 还要靠 worker 把队列消费干净. 若以它为退出判据,
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
                // 必先于生产者读 idle_state_ - 后者看到登记便照常唤醒; 若登记
                // 晚于该读, 则 push 必已对复查可见, 复查不会错过. 两侧必居
                // 其一, 丢失唤醒不可能. 自旋者的注销同样先于本栅栏, 故
                // "生产者见到自旋者便跳过唤醒"落在同一论证内
                idle_state_.fetch_add(SLEEPER_UNIT, std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (node_t* m = try_acquire(idx, seed)) {
                    idle_state_.fetch_sub(SLEEPER_UNIT, std::memory_order_relaxed);
                    execute_node(m, idx);
                    continue;
                }
                wake_gen_.wait(g); // 全空则睡; 推送方 bump 代际唤醒
                idle_state_.fetch_sub(SLEEPER_UNIT, std::memory_order_relaxed);
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
            std::size_t vi = bounded_rand(xorshift(seed), n_threads_);
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

        /// [0, n) 上的窃取起点(Lemire 乘移法). n_threads_ 是运行期值,
        /// 取模会编译成真正的 div - 而本函数落在每次探测的路径上
        [[nodiscard]]
        static constexpr std::size_t bounded_rand(std::uint32_t r, std::size_t n) noexcept {
            return static_cast<std::size_t>((static_cast<std::uint64_t>(r) * n) >> 32);
        }

        /// 队列里是否还有未认领的工作(近似, 仅作唤醒启发, 不作同步依据).
        /// 只看全局队列与自有本地 deque: 他人的本地 deque 非空即意味着其
        /// 所有者正在运行, 无须唤醒第三方
        [[nodiscard]]
        bool more_work_hint(std::size_t idx) const noexcept {
            for (int lv = 0; lv < LEVELS; ++lv) {
                if ((*globals_)[lv].size_approx() != 0 ||
                    ctxs_[idx].local[lv].size_approx() != 0) {
                    return true;
                }
            }
            return false;
        }

        /// 排空全部未开始的任务, 并收敛与并发提交的竞争残留
        ///
        /// stopping 置位后新提交必然被拒, 但"已越过拒绝检查"的在途提交仍可能
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
                // pending 同时覆盖"已入队/运行中"与"越过检查但尚未入队"两类
                // 在途工作(见 enqueue 的占坑 RMW): 置位后不再有新的进入者,
                // 该计数单调收敛, 两轮静默从而是可达成的
                if (got == 0 && pending_count() == 0) {
                    ++quiet_rounds;
                } else {
                    quiet_rounds = 0;
                    if (got == 0) {
                        // 计数由在途工作持有: 挂在空闲代际上而非 yield 空转 -
                        // 后者在 discard 期长任务跑着时会烧满整个任务时长.
                        // 复检后仍非零才睡(complete_one 把"归零"与 bump/notify
                        // 原子配对, 不会睡过终点); 虚假唤醒由外层轮次结构容忍
                        const std::uint32_t g = idle_gen_.load(std::memory_order_acquire);
                        if (pending_count() != 0) {
                            idle_gen_.wait(g);
                        }
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
            while (auto* n = node_pool_->try_pop()) {
                abandon(n);
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
                                       effective_priority(p), no_worker});
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

        /// 提交闸门与在途计数合成一个字: bit0 = stopping(拒绝新提交),
        /// 其余位 = pending(已入队/运行中的工作, 外加"越过检查但尚未入队"的
        /// 在途提交). 合成之后每次提交只付一次共享行上的 RMW, 关闭与计数的
        /// 定序也不再需要额外的 Dekker 配对(见 enqueue)
        alignas(64) mutable std::atomic<std::uint64_t> state_{0};
        /// 唤醒代际. 取 32 位而非 64: libstdc++ 只对 4 字节对象直接用 futex,
        /// 更宽的类型退化为哈希代理等待池 - 代理下 notify_one 只能保守地
        /// 唤醒全部等待者(惊群), 且不同对象会在池中相互干扰
        alignas(64) std::atomic<std::uint32_t> wake_gen_{0};
        alignas(64) mutable std::atomic<std::uint32_t> idle_gen_{0};
        /// 闲置 worker 状态: 高 32 位 = 自旋中的数目, 低 32 位 = 睡眠中
        /// (或正在入睡)的数目. 二者同处一个字, 使 notify_wake 一次载入即可
        /// 判定"是否需要进内核"; 与 worker 侧的"登记 -> 栅栏 -> 复查"构成
        /// 不丢唤醒协议, 详见 notify_wake 与 worker_main 的注释
        alignas(64) std::atomic<std::uint64_t> idle_state_{0};
        /// worker 的退出判据. 与 state_ 的 stopping 位分离: 后者一置位即
        /// 拒绝新提交, 而 worker 必须继续消费到排空收敛之后才可离场
        alignas(64) std::atomic<bool> quitting_{false};
        alignas(64) std::atomic<std::uint64_t> id_seq_{0};

        /// 仅拆除路径使用; 与热路径原子量无共享行
        std::mutex shutdown_mtx_;
        /// drain 期嵌套提交放行余额(见 DRAIN_NESTED_BUDGET), 仅 stopping
        /// 置位后触碰
        std::atomic<std::int64_t> drain_nested_budget_{DRAIN_NESTED_BUDGET};

        std::unique_ptr<worker_ctx_t[]> ctxs_;
        /// 全局空闲节点池(MPMC 有界环): 外部生产者与 worker 之间的节点流转
        /// 复用. 堆分配以保持池对象本身可安全栈上构造(同 globals_)
        std::unique_ptr<node_pool_t> node_pool_;
        std::conditional_t<WORKER_CAP != 0, std::inplace_vector<std::jthread, WORKER_CAP>,
                           std::vector<std::jthread>>
            workers_;
        /// 全局环体积随容量线性增长(65536 槽 ≈ 4MiB/层, priority 下三层),
        /// 堆分配以保持池对象本身可安全栈上构造; 热路径仅多一次指针解引用
        std::unique_ptr<std::array<gq_t, LEVELS>> globals_;
        /// !TRACE 时零尺寸(monostate + [[no_unique_address]]), 三个
        /// move_only_function 槽位不占池对象空间
        [[no_unique_address]] std::conditional_t<TRACE, trace_hooks, std::monostate> hooks_;
        std::size_t n_threads_ = 0;
        /// 睡前忙等预算(options::spin_budget, 构造后只读)
        std::chrono::microseconds spin_budget_{64};

        /// 取走 options 里的钩子; 非 trace 池原样丢弃(存储为零尺寸 monostate)
        [[nodiscard]]
        static std::conditional_t<TRACE, trace_hooks, std::monostate> take_hooks(
            options& opts) noexcept {
            if constexpr (TRACE) {
                return std::move(opts.hooks);
            } else {
                return std::monostate{};
            }
        }
    };

    /// 默认别名: 无特性的基础形态
    using pool = basic_pool<>;

} // namespace concurrent
