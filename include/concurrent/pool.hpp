#pragma once
#include "concurrent/detail/chase_lev.hpp"
#include "concurrent/detail/contract_assert.hpp"
#include "concurrent/detail/cpu_relax.hpp"
#include "concurrent/detail/global_queue.hpp"
#include "concurrent/detail/mpmc_ring.hpp"
#include "concurrent/detail/node_cache.hpp"
#include "concurrent/detail/spinlock.hpp"
#include "concurrent/tags.hpp"
#include "concurrent/task.hpp"
#include "concurrent/trace.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <inplace_vector>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace concurrent {

    /// 关闭策略
    enum class shutdown_policy : std::uint8_t {
        drain, ///< 排空全部排队任务后退出(析构默认)
        /// 丢弃排队任务并以取消语义终结其结果通道; 运行中任务两种策略下
        /// 都会等待完成(jthread join 的固有语义). 时序上的精确保证: 被
        /// drop_all_queued 收敛的任务必不执行, 且结果通道必以
        /// operation_cancelled 终结; 但停止位与排空之间存在竞争窗口, worker
        /// 已从队列取出而尚未开跑的那部分可能照常执行(任务体不在该窗口内
        /// 被取消). 故"丢弃"应读作"尽力丢弃", 不可依赖"排队即绝不执行"
        discard,
    };

    namespace detail {

        /// worker 线程上下文: 空闲节点缓存 + 分层本地 deque
        template <std::size_t Levels, std::size_t LocalCap, std::size_t CacheCap>
        struct alignas(64) worker_ctx {
            node_cache<task_node, CacheCap> cache; ///< 仅所有者线程访问, 带上限
            std::array<chase_lev_deque<task_node*, LocalCap>, Levels> local{};
        };

        /// 线程内当前池与 worker 身份(嵌套提交路由用). constinit: 常量
        /// 初始化, 免去 TLS 动态初始化守卫检查
        inline constinit thread_local const void* tls_pool = nullptr;
        inline constinit thread_local std::size_t tls_worker = 0;
        /// 非 worker 提交者(外部生产者 / 排空线程)的记账分片槽号: 首次
        /// 提交时自全局序号取模惰性分配, SIZE_MAX 为未分配哨兵
        inline constinit thread_local std::size_t tls_external_cell = SIZE_MAX;
        inline constinit std::atomic<std::size_t> g_cell_seq{0};

        /// 可提交的 callable: 直接可调用, 或以 std::stop_token 为首参可调用
        template <typename F, typename... Args>
        concept submittable =
            std::invocable<F, Args...> || std::invocable<F, std::stop_token, Args...>;

        template <typename F, typename... Args>
        inline constexpr bool takes_token_v = std::invocable<F, std::stop_token, Args...>;

        /// submit 的结果类型: token 感知调用优先匹配
        template <typename F, typename... Args>
        struct submit_result {
            using type = std::invoke_result_t<F, Args...>;
        };
        template <typename F, typename... Args>
            requires takes_token_v<F, Args...>
        struct submit_result<F, Args...> {
            using type = std::invoke_result_t<F, std::stop_token, Args...>;
        };
        template <typename F, typename... Args>
        using submit_result_t = typename submit_result<F, Args...>::type;

    } // namespace detail

    /**
     * @brief 固定容量工作窃取线程池
     *
     * 架构: 每线程分层本地 deque(LIFO)+ Chase-Lev 窃取(FIFO, 随机 victim 起点)
     * + 分层全局可扩容队列兜底(Vyukov 环 + 溢出链). 外部提交进全局,
     * worker 内嵌套提交进本地, 本地溢出落全局; 全局环满则转入同序溢出链,
     * 提交不等待队列腾空, 不因容量耗尽被拒绝
     *
     * 计数: 提交与完成分别累加到调用线程的分片, worker 独占各自的缓存行.
     * 空闲判定在两次稳定的提交快照之间核对完成总数, 同时取得任务写入的
     * 可见性. 关闭闸门与提交登记以 seq_cst 定序(见 enqueue)
     *
     * 零 throw: 库自身的一切失败经 std::expected 报告; 任务体异常被捕获并透传至
     * 结果通道; execute 要求 callable 为 noexcept - 从类型系统保证遗忘型任务零
     * 异常逃逸(连带排斥了拷贝构造会抛的实参). 唯一的例外是 submit / submit_each
     * 在提交期运行的**用户代码** - callable 与实参的拷贝/移动构造: 其中的
     * bad_alloc 折成 submit_error::out_of_memory, 其余异常原样透传给调用方
     * (误标为 OOM 反而丢失了真实成因)
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
        /// 本地 deque / 全局环每层容量, 由 queue_cap<Global, Local> 标签配置(取舍与缺省见 tags.hpp)
        static constexpr std::size_t LOCAL_CAP = detail::queue_local_cap_v<Flags...>;
        static constexpr std::size_t GLOBAL_CAP = detail::queue_global_cap_v<Flags...>;
        static_assert(GLOBAL_CAP >= 2 && std::has_single_bit(GLOBAL_CAP),
                      "queue_cap<Global, Local>: Global must be a power of two >= 2");
        static_assert(LOCAL_CAP >= 2 && std::has_single_bit(LOCAL_CAP),
                      "queue_cap<Global, Local>: Local must be a power of two >= 2");
        /// 每 worker 空闲节点缓存上限. 无上限时外部线程持续提交会让缓存长度
        /// 随累计任务数单调增长(节点归还进执行者的缓存, 外部生产者永远不来取)
        static constexpr std::size_t NODE_CACHE_CAP = 1024;
        /// 全局空闲节点池上限: worker 本地缓存溢出时归还于此, 供外部生产者
        /// 跨线程复用. 无此环节则每任务一次跨线程 free, 实测 fire-and-forget
        /// 吞吐掉约 4 倍
        static constexpr std::size_t NODE_POOL_CAP = 4096;
        static constexpr std::size_t NODE_BATCH = 8;
        static_assert(NODE_CACHE_CAP >= NODE_BATCH);
        static constexpr std::size_t GLOBAL_BATCH = std::min<std::size_t>(LOCAL_CAP, 8);
        /// stopping 置位后 worker 嵌套提交的放行预算: 防"自适应派生"型任务
        /// (派生速率不衰减)在关闭窗口内无限繁殖令 shutdown 永不返回.
        /// 覆盖约 depth-21 满二叉树的派生量, 耗尽退回拒绝(stopped);
        /// 正常运行零触碰
        static constexpr std::int64_t DRAIN_NESTED_BUDGET = 4'000'000;
        static constexpr int PAUSE_BATCH = 16; ///< 每轮探测间的让核步长
        static constexpr std::size_t MAX_THREADS = 65536;
        /// idle_state_ 的两个计数单位(高 32 位自旋者, 低 32 位睡眠者);
        /// worker 数不超过 MAX_THREADS, 两侧计数均不可能溢出各自的 32 位
        static constexpr std::uint64_t SPINNER_UNIT = std::uint64_t{1} << 32;
        static constexpr std::uint64_t SLEEPER_UNIT = 1;
        /// 外部线程共享固定数量的分片; worker 的分片单独分配, 不与生产者碰撞.
        static constexpr std::size_t EXTERNAL_CELLS = 256;

        [[nodiscard]]
        static constexpr std::uint64_t spinner_count(std::uint64_t s) noexcept {
            return s >> 32;
        }
        [[nodiscard]]
        static constexpr std::uint64_t sleeper_count(std::uint64_t s) noexcept {
            return s & 0xFFFF'FFFFull;
        }

        [[nodiscard]]
        std::uint64_t submitted_count() const noexcept {
            std::uint64_t sum = 0;
            for (std::size_t i = 0; i < n_threads_ + EXTERNAL_CELLS; ++i) {
                sum += cells_[i].submitted.load(std::memory_order_seq_cst);
            }
            return sum;
        }

        /// 所有计数单调递增. 两次提交总数相等时, 中间的完成扫描期间没有
        /// 提交变化; 完成数也相等才可判空. seq_cst 将这些观察置于同一全序,
        /// 完成计数的 acquire 语义同时发布任务体与捕获析构的写入.
        /// 计数绕回的前提与队列索引相同: 一次观察期间不能发生 2^64 次操作.
        [[nodiscard]]
        bool is_idle() const noexcept {
            const std::uint64_t submitted = submitted_count();
            std::uint64_t completed = 0;
            for (std::size_t i = 0; i < n_threads_ + EXTERNAL_CELLS; ++i) {
                completed += cells_[i].completed.load(std::memory_order_seq_cst);
            }
            return submitted == completed && submitted == submitted_count();
        }

        using node_t = detail::task_node;
        using gq_t = detail::global_queue<node_t, GLOBAL_CAP>;
        using worker_ctx_t = detail::worker_ctx<LEVELS, LOCAL_CAP, NODE_CACHE_CAP>;
        using node_pool_t = detail::mpmc_ring<node_t*, NODE_POOL_CAP / NODE_BATCH>;
        struct alignas(64) accounting_cell {
            std::atomic<std::uint64_t> submitted{0};
            std::atomic<std::uint64_t> completed{0};
        };
        struct alignas(64) producer_cache {
            detail::node_cache<node_t, NODE_BATCH> nodes;
            detail::spinlock lock;
        };

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
         * @pre opts.threads ≤ MAX_THREADS
         * @pre 带 worker_cap<N> 标签时 opts.threads ≤ N
         */
        explicit basic_pool(options opts = {}) pre(opts.threads <= MAX_THREADS)
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

            // 队列先于 worker 就位, 且置于 try 之外: 收尾路径(flush_freelists)
            // 要解引用 node_pool_, 只有 worker 已启动的失败才需要那条路径
            ctxs_ = std::make_unique<worker_ctx_t[]>(n_threads_);
            node_pool_ = std::make_unique<node_pool_t>();
            producer_caches_ = std::make_unique<producer_cache[]>(EXTERNAL_CELLS);
            globals_ = std::make_unique<std::array<gq_t, LEVELS>>();
            cells_ = std::make_unique<accounting_cell[]>(n_threads_ + EXTERNAL_CELLS);
            try {
                spawn_workers();
            } catch (...) {
                // 已就位的 worker 只在看到退出判据后才离场: 先置位并唤醒, 再
                // join, 最后清空节点缓存, 之后异常方可继续传播; 若跳过这步,
                // 成员析构中 jthread 的 join 会永久阻塞
                retire_workers();
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
         *
         * @throws 提交期由**用户代码**(callable 与实参的拷贝/移动构造)抛出的
         *         非 bad_alloc 异常原样透传; bad_alloc 折成 out_of_memory
         */
        template <typename F, typename... Args>
            requires(!std::same_as<std::remove_cvref_t<F>, task_priority>) &&
                    detail::submittable<F, Args...> // 不可调用对象在重载处即报, 而非深入实现后才报
        [[nodiscard]]
        auto submit(F&& f, Args&&... args)
            -> std::expected<task<detail::submit_result_t<F, Args...>>, submit_error> {
            using R = detail::submit_result_t<F, Args...>;
            return guard_oom([&] {
                return submit_impl<R>(task_priority::normal, std::forward<F>(f),
                                      std::forward<Args>(args)...);
            });
        }

        /// 提交带优先级的任务 @requires priority 标签
        template <typename F, typename... Args>
            requires(PRIORITY) && detail::submittable<F, Args...>
        [[nodiscard]]
        auto submit(task_priority prio, F&& f, Args&&... args)
            -> std::expected<task<detail::submit_result_t<F, Args...>>, submit_error> {
            using R = detail::submit_result_t<F, Args...>;
            return guard_oom([&] {
                return submit_impl<R>(prio, std::forward<F>(f), std::forward<Args>(args)...);
            });
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
                     detail::submittable<F&, std::ranges::range_value_t<Rng>>
        [[nodiscard]]
        auto submit_each(Rng&& rng, F f)
            -> std::expected<std::vector<task<detail::submit_result_t<
                                 F, std::ranges::range_value_t<Rng>>>>,
                             submit_error> {
            using elem_t = std::ranges::range_value_t<Rng>;
            using R = detail::submit_result_t<F, elem_t>;

            std::vector<task<R>> out;
            std::vector<std::pair<node_t*, trace_env_t>> staged;
            // 半成品的回收: 终结其状态防悬挂. 已交给队列的槽位已置空, 不在范围内
            auto rollback = [&]() noexcept {
                for (node_t* n : staged | std::views::keys) {
                    if (n) {
                        abandon(n);
                    }
                }
            };

            // 阶段一: 整体构建. 先占位再建节点, 容器扩容抛出时没有游离的半成品
            try {
                if constexpr (std::ranges::sized_range<Rng>) {
                    const auto n = static_cast<std::size_t>(std::ranges::size(rng));
                    out.reserve(n);
                    staged.reserve(n);
                }
                for (auto&& e : rng) {
                    auto& slot = staged.emplace_back();
                    std::shared_ptr<detail::shared_state<R>> st;
                    slot = build_submit_node(task_priority::normal, st, f,
                                             elem_t(std::forward<decltype(e)>(e)));
                    if (!slot.first) [[unlikely]] {
                        rollback();
                        return std::unexpected(submit_error::out_of_memory);
                    }
                    out.emplace_back(std::move(st));
                }
            } catch (const std::bad_alloc&) { // 容器扩容或闭包堆存储
                rollback();
                return std::unexpected(submit_error::out_of_memory);
            } catch (...) { // F/元素拷贝的用户异常原样透传
                rollback();
                throw;
            }

            // 阶段二: 依序入队, 单次唤醒. 节点一经交给 enqueue 即归队列所有
            // (被拒时由 enqueue 终结), 从回滚范围移除
            for (auto& [node, env] : staged) {
                node_t* n = std::exchange(node, nullptr);
                if (auto ok = enqueue(level_of(task_priority::normal), n); !ok) {
                    rollback();
                    return std::unexpected(ok.error());
                }
                trace_enqueue(env);
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
            return guard_oom([&] {
                return execute_impl(task_priority::normal, std::forward<F>(f),
                                    std::forward<Args>(args)...);
            });
        }

        /// 即发即忘 + 优先级 @requires priority 标签
        template <typename F, typename... Args>
            requires(PRIORITY && std::is_nothrow_invocable_v<F, Args...>)
        [[nodiscard]]
        std::expected<void, submit_error> execute(task_priority prio, F&& f, Args&&... args) {
            return guard_oom(
                [&] { return execute_impl(prio, std::forward<F>(f), std::forward<Args>(args)...); });
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
            return guard_oom([&] {
                return execute_cancellable_impl(task_priority::normal, std::forward<F>(f),
                                                std::forward<Args>(args)...);
            });
        }

        /// 可取消 + 优先级 @requires priority 与 cancellable 标签
        template <typename F, typename... Args>
            requires(PRIORITY && CANCELLABLE_TAG && detail::takes_token_v<F, Args...> &&
                     std::is_nothrow_invocable_v<F, std::stop_token, Args...> &&
                     !std::is_nothrow_invocable_v<F, Args...>)
        [[nodiscard]]
        std::expected<std::stop_source, submit_error> execute(task_priority prio, F&& f,
                                                              Args&&... args) {
            return guard_oom([&] {
                return execute_cancellable_impl(prio, std::forward<F>(f),
                                                std::forward<Args>(args)...);
            });
        }

        /**
         * @brief 结构化 work-first 派生: f 提交入池, g 由调用线程内联执行
         *
         * 递归分治的惯用形态: 提交分支先入队供任何 worker 窃取, 内联分支
         * 沿调用栈深度优先执行, 保有缓存局部性; 每层只付一次提交通道,
         * 较双提交省一半入队与记账开销. 返回后两分支各自在途, 以池级
         * wait()/shutdown 汇合 - 自身不阻塞, 故在 worker 内调用安全(与
         * wait/shutdown 相反). f 须 noexcept(与 execute 同); g 直接内联,
         * 其异常按直接调用语义传播(worker 内由任务体的异常通道接住).
         * 提交失败(已停/OOM)则 g 不执行, 以免半棵计算树落空
         */
        template <typename F1, typename F2>
            requires std::is_nothrow_invocable_v<F1&>
        [[nodiscard]]
        std::expected<void, submit_error> fork_join(F1&& f, F2&& g) {
            // 先入队的 f 先可窃取, 再跑内联分支(work-first). g 置于 OOM 守卫之外:
            // 其异常(含 bad_alloc)属直接调用语义, 不得被折成提交失败
            auto ok = guard_oom(
                [&] { return execute_impl(task_priority::normal, std::forward<F1>(f)); });
            if (ok) {
                std::invoke(std::forward<F2>(g));
            }
            return ok;
        }

        /// 阻塞直至全部任务完成(排队 + 运行中). 虚假唤醒安全.
        /// 唤醒常态即时(完成路径的捷径), 最坏多等一个自旋预算(见 complete_one)
        /// @pre 调用者不得是本池的 worker - 其正在执行的任务自身就持有
        ///      pending 计数, 归零永不发生, 必死锁(Debug 构建下契约断言终止)
        void wait() const noexcept {
            CONCURRENT_CONTRACT_ASSERT(!in_own_worker());
            while (!is_idle()) {
                std::uint32_t g = idle_gen_.load(std::memory_order_acquire);
                if (is_idle()) {
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
            while (!is_idle()) {
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
        /// @pre 调用者不得是本池的 worker - join 自身 + 等自身完成的 pending
        ///      归零, 必死锁(Debug 构建下契约断言终止)
        void shutdown(shutdown_policy policy = shutdown_policy::drain) noexcept {
            CONCURRENT_CONTRACT_ASSERT(!in_own_worker());
            // 策略选择、排空和拆除由同一调用者持有: 后来的调用必须等待其完成.
            std::lock_guard lock{shutdown_mtx_};
            // 与 enqueue 的登记及复查同处 seq_cst 全序, 关闭后的空闲检查
            // 必计入所有越过闸门但尚未发布到队列的提交.
            const bool first = !stopping_.exchange(true, std::memory_order_seq_cst);
            if (first && policy == shutdown_policy::drain) {
                wait();
            }

            drop_all_queued(); // discard 的主体; drain 时仅收敛在途提交的残留
            // 只有排空收敛之后才允许 worker 退出: 在此之前它们是队列的唯一
            // 消费者, 提前离场会让"已越过拒绝检查的在途提交"无人认领
            retire_workers();
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
        /// submit / execute 的公共外壳: 提交路径唯一允许逃逸的异常是分配
        /// 失败, 就地转入 expected 的错误通道(库表面零 throw). 其余异常
        /// (F 与实参的拷贝/移动)属用户代码, 原样透传
        template <typename Impl>
        [[nodiscard]]
        static auto guard_oom(Impl&& impl) -> decltype(impl()) {
            try {
                return impl();
            } catch (const std::bad_alloc&) {
                return std::unexpected(submit_error::out_of_memory);
            }
        }

        /// trace 专用捕获(任务编号 + 优先级). 标签关闭时折叠为 monostate,
        /// 任务闭包除 self/实参之外零固定开销, SBO 预算全部留给用户捕获
        struct trace_env {
            std::uint64_t id = 0;
            task_priority prio = task_priority::normal;
        };
        using trace_env_t = std::conditional_t<TRACE, trace_env, std::monostate>;

        /// 领取任务编号并打包 trace 环境; trace 关闭时零开销, 不触碰 id_seq_
        [[nodiscard]]
        trace_env_t make_trace_env([[maybe_unused]] task_priority prio) noexcept {
            if constexpr (TRACE) {
                return trace_env{.id = id_seq_.fetch_add(1, std::memory_order_relaxed) + 1,
                                 .prio = prio};
            } else {
                return {};
            }
        }

        /// 取一个节点并把任务闭包就地构造在它的槽里: 免去"先建临时再移动
        /// 进来"的一次移动构造 + 析构. 构建(F/实参拷贝, sbo_function 堆模式
        /// 分配)是三条提交路径唯一可能抛的用户代码段, 抛出时节点仍是干净
        /// 空壳, 原样归还
        /// @return 空 = 仅因 bad_alloc(节点耗尽); 其余异常原样传播
        template <typename Factory>
        node_t* make_node(Factory&& make) {
            node_t* node = acquire_node();
            if (!node) [[unlikely]] {
                return nullptr;
            }
            try {
                node->body.emplace_with(std::forward<Factory>(make));
            } catch (...) {
                release_node(node);
                throw;
            }
            return node;
        }

        /// 提交路径的收尾: 入队 + 唤醒一个 worker + trace
        std::expected<void, submit_error> emit(node_t* node, task_priority prio,
                                               const trace_env_t& env) noexcept {
            if (auto ok = enqueue(level_of(prio), node); !ok) {
                return ok;
            }
            notify_wake();
            trace_enqueue(env);
            return {};
        }

        /// 构建 submit 型节点(状态 + 闭包 + 丢弃终结钩子), 未入队
        /// @return 节点为空 = 仅因 bad_alloc; 其余异常(F/实参拷贝)原样传播
        template <typename R, typename F, typename... Args>
        std::pair<node_t*, trace_env_t>
        build_submit_node(task_priority prio, std::shared_ptr<detail::shared_state<R>>& st,
                          F&& f, Args&&... args) {
            try {
                st = detail::make_state<R>();
                if constexpr (detail::takes_token_v<F, Args...>) {
                    // 只有轮询 token 的任务需要真正的 stop_source(一次堆分配);
                    // 其余任务的取消由状态内的原子标志承载. 此刻状态尚未交给
                    // 任何其他线程, 实体化不与并发的 request_stop 竞争
                    st->enable_stop();
                }
            } catch (const std::bad_alloc&) {
                return {nullptr, {}};
            }

            auto* self = this;
            const trace_env_t env = make_trace_env(prio);
            node_t* node = make_node([&] {
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
            if (!node) [[unlikely]] {
                return {nullptr, env}; // st 随作用域释放
            }

            // 关闭丢弃路径的终结钩子: 以取消语义收尾共享状态并发布完成,
            // 使持有 task 句柄的一方经错误通道观测到 operation_cancelled
            node->discard_ctx = st.get();
            node->discard = [](void* p) noexcept {
                auto* s = static_cast<detail::shared_state<R>*>(p);
                s->set_cancelled();
                s->finish();
            };
            return {node, env};
        }

        template <typename R, typename F, typename... Args>
        std::expected<task<R>, submit_error> submit_impl(task_priority prio, F&& f,
                                                         Args&&... args) {
            std::shared_ptr<detail::shared_state<R>> st;
            auto [node, env] =
                build_submit_node(prio, st, std::forward<F>(f), std::forward<Args>(args)...);
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }
            if (auto ok = emit(node, prio, env); !ok) {
                return std::unexpected(ok.error());
            }
            return task<R>{std::move(st)};
        }

        template <typename F, typename... Args>
        std::expected<void, submit_error> execute_impl(task_priority prio, F&& f, Args&&... args) {
            const trace_env_t env = make_trace_env(prio);
            node_t* node = make_node([&] {
                if constexpr (TRACE) {
                    return [this, env, f = std::forward<F>(f),
                            ... a = std::forward<Args>(args)]() mutable noexcept {
                        trace_begin(env);
                        std::invoke(std::move(f), std::move(a)...);
                        trace_end(env, task_outcome::completed);
                    };
                } else {
                    return [f = std::forward<F>(f),
                            ... a = std::forward<Args>(args)]() mutable noexcept {
                        std::invoke(std::move(f), std::move(a)...);
                    };
                }
            });
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }
            return emit(node, prio, env);
        }

        template <typename F, typename... Args>
        std::expected<std::stop_source, submit_error>
        execute_cancellable_impl(task_priority prio, F&& f, Args&&... args) {
            // stop_source 的拷贝共享同一停止状态: 闭包持一份, 调用方得一份,
            // 调用方句柄失效后任务仍可安全查询, 无须再套一层 shared_ptr
            std::stop_source source;
            auto* self = this;
            const trace_env_t env = make_trace_env(prio);
            node_t* node = make_node([&] {
                return [self, env, source, f = std::forward<F>(f),
                        ... a = std::forward<Args>(args)]() mutable noexcept {
                    self->trace_begin(env);
                    if (!source.stop_requested()) {
                        std::invoke(std::move(f), source.get_token(), std::move(a)...);
                        self->trace_end(env, task_outcome::completed);
                    } else {
                        self->trace_end(env, task_outcome::cancelled);
                    }
                };
            });
            if (!node) [[unlikely]] {
                return std::unexpected(submit_error::out_of_memory);
            }
            if (auto ok = emit(node, prio, env); !ok) {
                return std::unexpected(ok.error());
            }
            return source;
        }

        /// 有状态任务的异常与取消进入结果通道, 完成后内联运行续延.
        template <typename State, typename Invoker>
        void run_task_body(State& st, const trace_env_t& env, Invoker&& invoke) noexcept {
            task_outcome o = task_outcome::completed;
            trace_begin(env);
            if (st.stop_requested()) {
                st.set_cancelled();
                o = task_outcome::cancelled;
            } else {
                try {
                    if constexpr (std::is_void_v<typename State::value_type>) {
                        invoke();
                    } else {
                        st.emplace_value(invoke());
                    }
                } catch (...) {
                    st.set_exception(std::current_exception());
                    o = task_outcome::failed;
                }
            }
            trace_end(env, o);
            st.finish(); // 先发布完成再跑续延(续延可能回查本状态)
        }

        /// 入队, 不含唤醒(批量提交方据此摊薄通知成本)
        /// @return 空 = 成功; 非空 = submit_error
        std::expected<void, submit_error> enqueue(int level, node_t* node) noexcept {
            // 登记、闸门复查与关闭置位同处 SC 全序: 获准的提交先于关闭,
            // 必被关闭方的提交快照看到. 预检拒绝稳定的停止状态, 以免外部
            // 重试不断改变提交数, 使关闭方无法取得稳定快照.
            const bool stopped_seen = stopping_.load(std::memory_order_acquire);
            if (stopped_seen && !nested_submit_permitted()) [[unlikely]] {
                // 统一出口 abandon: submit 路径节点入队前已挂 discard 钩子,
                // 须以取消语义终结其共享状态; execute 路径节点无钩子, 等价销毁
                abandon(node);
                return std::unexpected(submit_error::stopped);
            }
            const std::size_t cell = cell_of_caller();
            cells_[cell].submitted.fetch_add(1, std::memory_order_seq_cst);
            if (!stopped_seen) [[likely]] {
                // 预检时未停才需要复查; 已停分支(嵌套放行)复查无意义且其
                // 收敛由父任务不变式保证(见 nested_submit_permitted)
                if (stopping_.load(std::memory_order_seq_cst) &&
                    !nested_submit_permitted()) [[unlikely]] {
                    abandon(node);
                    cells_[cell].completed.fetch_add(1, std::memory_order_seq_cst);
                    maybe_bump_if_idle();
                    return std::unexpected(submit_error::stopped);
                }
            }

            // worker 内嵌套提交: 优先本地 deque(LIFO 缓存热度)
            if (in_own_worker() && ctxs_[detail::tls_worker].local[level].push(node))
                [[likely]] {
                return {};
            }
            // 外部提交或本地溢出进入全局队列, 环满时由溢出链承接.
            (*globals_)[level].push(node);
            return {};
        }

        /// 调用者是否本池的 worker. 供 wait / shutdown 的前置契约使用:
        /// 这两者由 worker 调用必死锁(见各自 @pre)
        [[nodiscard]]
        bool in_own_worker() const noexcept {
            return detail::tls_pool == static_cast<const void*>(this);
        }

        /// worker 独占前 n_threads_ 个单元; 外部线程映射到独立的固定分片区.
        [[nodiscard]]
        std::size_t cell_of_caller() const noexcept {
            if (in_own_worker()) {
                return detail::tls_worker;
            }
            if (detail::tls_external_cell == SIZE_MAX) [[unlikely]] {
                detail::tls_external_cell =
                    detail::g_cell_seq.fetch_add(1, std::memory_order_relaxed) % EXTERNAL_CELLS;
            }
            return n_threads_ + detail::tls_external_cell;
        }

        /// stopping 置位后本条提交是否放行: 仅本池 worker 的嵌套提交,
        /// 消耗有限预算(DRAIN_NESTED_BUDGET), 其余一律拒绝.
        /// 父任务直到 callable 析构结束才记为完成, 因此子任务发布期间池不能判空.
        [[nodiscard]]
        bool nested_submit_permitted() noexcept {
            if (!in_own_worker()) {
                return false;
            }
            return drain_nested_budget_.fetch_sub(1, std::memory_order_relaxed) > 0;
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
            complete_one(worker);
        }

        /// 干净空壳节点的归还: 本地缓存(嵌套提交复用) -> 全局空闲池(外部
        /// 生产者跨线程复用) -> 分配器
        void recycle_node(node_t* n, std::size_t worker) noexcept {
            if (!ctxs_[worker].cache.push(n)) {
                recycle_batch(n, worker);
            }
        }

        // 跨线程归还只在缓存满时发生; 独立调用使 worker 循环的常态路径保持紧凑.
        [[gnu::noinline]]
        void recycle_batch(node_t* n, std::size_t worker) noexcept {
            auto& cache = ctxs_[worker].cache;
            n->next = nullptr;
            for (std::size_t i = 1; i < NODE_BATCH; ++i) {
                node_t* spare = cache.pop();
                spare->next = n->next;
                n->next = spare;
            }
            if (!node_pool_->try_push(n)) {
                destroy_batch(n);
            }
        }

        /// 提交路径上的归还(闭包构建失败): 调用方未必是本池 worker
        void release_node(node_t* n) noexcept {
            if (in_own_worker()) {
                recycle_node(n, detail::tls_worker);
            } else {
                auto& cache = producer_caches_[cell_of_caller() - n_threads_];
                {
                    std::lock_guard lock{cache.lock};
                    if (cache.nodes.push(n)) {
                        return;
                    }
                }
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

        /// 空闲代际推进: 唤醒挂在 idle_gen_ 上的全部等待者(wait / 排空)
        void bump_idle() const noexcept {
            idle_gen_.fetch_add(1, std::memory_order_release);
            idle_gen_.notify_all();
        }

        void maybe_bump_if_idle() const noexcept {
            if (is_idle()) {
                bump_idle();
            }
        }

        /// 完成发布覆盖任务体、续延和 callable 析构, 计数只写本 worker 的单元.
        void complete_one(std::size_t worker) noexcept {
            cells_[worker].completed.fetch_add(1, std::memory_order_seq_cst);
            // 常态唤醒捷径: 其余 worker 全在睡且队列已空, 本次完成极可能
            // 就是最后一次 - 直接推代际, 等待者免等满本 worker 的自旋预算.
            // 启发式可能产生虚假唤醒; 等待者核对计数, 睡前检查补足遗漏的通知.
            const std::uint64_t idle = idle_state_.load(std::memory_order_relaxed);
            if (spinner_count(idle) == 0 && sleeper_count(idle) == n_threads_ - 1 &&
                !more_work_hint(worker)) {
                bump_idle();
            }
        }

        static constexpr int level_of(task_priority p) noexcept {
            // 枚举 low..high 升序而层索引反向(高优先级层号小): 一行完成映射
            return PRIORITY ? LEVELS - 1 - static_cast<int>(std::to_underlying(p)) : 0;
        }

        node_t* acquire_node() noexcept {
            if (in_own_worker()) {
                if (auto* n = ctxs_[detail::tls_worker].cache.pop()) [[likely]] {
                    return n;
                }
            }
            return acquire_shared_node();
        }

        // 本地节点命中时不进入跨线程缓存与分配器路径, 便于将递归派生的取节点内联.
        [[gnu::noinline]]
        node_t* acquire_shared_node() noexcept {
            if (in_own_worker()) {
                if (auto* n = refill_nodes(ctxs_[detail::tls_worker].cache)) {
                    return n;
                }
            } else {
                auto& cache = producer_caches_[cell_of_caller() - n_threads_];
                // 外部线程号可能映射到同一分片, shutdown 也会排空这些缓存.
                std::lock_guard lock{cache.lock};
                if (auto* n = cache.nodes.pop()) {
                    return n;
                }
                if (auto* n = refill_nodes(cache.nodes)) {
                    return n;
                }
            }
            return new (std::nothrow) node_t{};
        }

        template <std::size_t Capacity>
        node_t* refill_nodes(detail::node_cache<node_t, Capacity>& cache) noexcept {
            static_assert(Capacity >= NODE_BATCH);
            CONCURRENT_CONTRACT_ASSERT(cache.size() == 0);
            node_t* node = node_pool_->try_pop();
            if (!node) {
                return nullptr;
            }
            node_t* rest = std::exchange(node->next, nullptr);
            while (rest) {
                node_t* next = rest->next;
                const bool stored = cache.push(rest);
                CONCURRENT_CONTRACT_ASSERT(stored);
                static_cast<void>(stored);
                rest = next;
            }
            return node;
        }

        void destroy_batch(node_t* node) noexcept {
            while (node) {
                node_t* next = node->next;
                abandon(node);
                node = next;
            }
        }

        /// 节点销毁的统一出口: 未执行过的节点挂着 discard 钩子, 先以取消
        /// 语义终结其共享状态(使等待方经错误通道观测 operation_cancelled
        /// 而非永久等待)再销毁; 已执行的节点钩子已被清除, 等价于直接销毁
        void abandon(node_t* n) noexcept {
            if (n->discard) {
                n->discard(n->discard_ctx);
            }
            delete n;
        }

        void spawn_workers() {
            workers_.reserve(n_threads_);
            for (std::size_t i = 0; i < n_threads_; ++i) {
                workers_.emplace_back([this, i] { worker_main(i); });
            }
        }

        /// 放 worker 离场: 置退出判据 -> 推代际唤醒全部睡眠者 -> jthread 析构
        /// join -> 清空节点缓存(join 之后再无并发访问者)
        void retire_workers() noexcept {
            quitting_.store(true, std::memory_order_release);
            wake_gen_.fetch_add(1, std::memory_order_release);
            wake_gen_.notify_all();
            workers_.clear();
            flush_freelists();
        }

        void worker_main(std::size_t idx) {
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
                            idle_state_.fetch_sub(SPINNER_UNIT, std::memory_order_relaxed);
                        // 最后一个自旋者带着活离场: 在此期间生产者的推送都因
                        // "有自旋者"而免了唤醒, 若队列里还有剩余, 需由本线程
                        // 接力唤醒一个睡眠者, 否则它们要等到本任务跑完才被认领.
                        // 注销与复查之间隔一道 seq_cst 栅栏, 与 notify_wake 的
                        // "push -> 栅栏 -> 读 idle_state_" 配对(同睡眠登记的论证)
                        if (n && spinner_count(prev) == 1) {
                            std::atomic_thread_fence(std::memory_order_seq_cst);
                            wake_queued_workers();
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
                if (quitting_.load(std::memory_order_acquire)) [[unlikely]] {
                    break;
                }
                // 睡前最后一搏用 try_acquire 而非只读的 more_work_hint:
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
                // 睡前核对完成数, 补足最后一个任务未命中唤醒捷径的情况.
                maybe_bump_if_idle();
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
                if (auto* n = self.local[lv].pop()) [[likely]] {
                    return n;
                }
            }
            for (int lv = 0; lv < LEVELS; ++lv) {
                std::array<node_t*, GLOBAL_BATCH> batch;
                if (const auto count = (*globals_)[lv].pop_batch(batch); count != 0) {
                    // 单 worker 保持 FIFO; 多 worker 将较早的任务放在窃取端,
                    // 使分治任务的大分支能及时分配给其他线程.
                    for (std::size_t i = 1; i < count; ++i) {
                        const std::size_t next = n_threads_ == 1 ? count - i : i;
                        const bool stored = self.local[lv].push(batch[next]);
                        CONCURRENT_CONTRACT_ASSERT(stored);
                        static_cast<void>(stored);
                    }
                    if (count > 1) {
                        // 批次暂存期间其他 worker 可能已入睡, 本地发布后须重新通知.
                        // 当前 worker 可能仍登记为闲置, 因此通知预算包含它.
                        notify_wake_n(count);
                    }
                    return batch[0];
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

        // 最后的自旋者可能刚从别人的队列取走任务, 余项仍需睡眠中的 worker 接手.
        [[gnu::noinline]]
        void wake_queued_workers() noexcept {
            std::size_t queued = 0;
            for (int lv = 0; lv < LEVELS; ++lv) {
                queued += (*globals_)[lv].size_approx();
                for (std::size_t i = 0; i < n_threads_; ++i) {
                    queued += ctxs_[i].local[lv].size_approx();
                }
            }
            if (queued != 0) {
                notify_wake_n(queued);
            }
        }

        /// 其余 worker 已睡眠时, 完成路径只检查全局与本地队列以避免全量扫描.
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
            // 每个被丢弃的任务在结果通道终结、捕获析构后发布完成.
            const std::size_t cell = cell_of_caller();
            auto account_drop = [this, cell](node_t* n) noexcept {
                abandon(n);
                cells_[cell].completed.fetch_add(1, std::memory_order_seq_cst);
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
                // 提交登记覆盖发布到队列之前的窗口; 嵌套派生由仍在途的父任务保护.
                if (got == 0 && is_idle()) {
                    bump_idle(); // 排空归零: 唤醒挂在 idle_gen_ 上的其他等待者
                    ++quiet_rounds;
                } else {
                    quiet_rounds = 0;
                    if (got == 0) {
                        // 计数由在途工作持有: 挂在空闲代际上而非 yield 空转 -
                        // 后者在 discard 期长任务跑着时会烧满整个任务时长.
                        // 复检后仍非零才睡(complete_one 把"归零"与 bump/notify
                        // 原子配对, 不会睡过终点); 虚假唤醒由外层轮次结构容忍
                        const std::uint32_t g = idle_gen_.load(std::memory_order_acquire);
                        if (!is_idle()) {
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
            for (std::size_t i = 0; i < EXTERNAL_CELLS; ++i) {
                auto& cache = producer_caches_[i];
                std::lock_guard lock{cache.lock};
                while (auto* n = cache.nodes.pop()) {
                    abandon(n);
                }
            }
            while (auto* batch = node_pool_->try_pop()) {
                destroy_batch(batch);
            }
        }

        void trace_enqueue([[maybe_unused]] const trace_env_t& env) noexcept {
            if constexpr (TRACE) {
                if (hooks_.on_enqueue) {
                    hooks_.on_enqueue({env.id, task_phase::enqueue, task_outcome::completed,
                                       env.prio, no_worker});
                }
            }
        }
        void trace_begin([[maybe_unused]] const trace_env_t& env) noexcept {
            if constexpr (TRACE) {
                if (hooks_.on_begin) {
                    hooks_.on_begin({env.id, task_phase::begin, task_outcome::completed, env.prio,
                                     detail::tls_worker});
                }
            }
        }
        void trace_end([[maybe_unused]] const trace_env_t& env,
                       [[maybe_unused]] task_outcome o) noexcept {
            if constexpr (TRACE) {
                if (hooks_.on_end) {
                    hooks_.on_end({env.id, task_phase::end, o, env.prio, detail::tls_worker});
                }
            }
        }

        /// 提交闸门: 置位即拒绝新提交(worker 嵌套提交除外, 见 enqueue).
        /// 与提交登记和空闲快照同处 seq_cst 全序, 保证在途提交被关闭方计入.
        alignas(64) std::atomic<bool> stopping_{false};
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
        /// worker 的退出判据. 与 stopping_ 分离: 后者一置位即
        /// 拒绝新提交, 而 worker 必须继续消费到排空收敛之后才可离场
        alignas(64) std::atomic<bool> quitting_{false};
        alignas(64) std::atomic<std::uint64_t> id_seq_{0};

        /// 仅拆除路径使用; 与热路径原子量无共享行
        std::mutex shutdown_mtx_;
        /// drain 期嵌套提交放行余额(见 DRAIN_NESTED_BUDGET), 仅 stopping
        /// 置位后触碰
        std::atomic<std::int64_t> drain_nested_budget_{DRAIN_NESTED_BUDGET};

        std::unique_ptr<worker_ctx_t[]> ctxs_;
        /// worker 各占一个缓存行, 外部提交者另有固定分片区. 提交与完成
        /// 分别单调累加, 空闲快照无需向这些热行写入或要求 worker 停顿.
        std::unique_ptr<accounting_cell[]> cells_;
        /// 空闲节点按 NODE_BATCH 个一组跨线程流转; 共享环容量按总节点数限定.
        std::unique_ptr<node_pool_t> node_pool_;
        std::unique_ptr<producer_cache[]> producer_caches_;
        std::conditional_t<WORKER_CAP != 0, std::inplace_vector<std::jthread, WORKER_CAP>,
                           std::vector<std::jthread>>
            workers_;
        /// 全局环体积随容量线性增长, 堆分配以保持池对象本身可安全栈上构造;
        /// 热路径仅多一次指针解引用
        std::unique_ptr<std::array<gq_t, LEVELS>> globals_;
        /// !TRACE 时零尺寸(monostate + [[no_unique_address]]), 三个
        /// move_only_function 槽位不占池对象空间
        [[no_unique_address]] std::conditional_t<TRACE, trace_hooks, std::monostate> hooks_;
        std::size_t n_threads_ = 0;
        /// 睡前忙等预算(options::spin_budget, 构造后只读)
        std::chrono::microseconds spin_budget_;

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
