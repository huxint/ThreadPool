#include <concurrent/concurrent.hpp>

// 第三方基线头未必按本仓库的警告集编写, 仅对引入行关闭诊断
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "third_party/BS_thread_pool.hpp"
#include "third_party/concurrentqueue.h"
#include <taskflow/taskflow.hpp>
#ifdef CONCURRENT_BENCH_TBB
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#endif
#pragma GCC diagnostic pop

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <print>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// 同机对比: concurrent::pool vs Taskflow vs BS::thread_pool,
// 扩展基线: oneTBB(系统包)与 moodycamel 队列自建池
//
// 公平性约定:
// - 同一负载, 同一线程数, 每项取多轮最优
// - 各池使用其惯用 API: 无结果通道优先(execute / silent_async / detach_task),
//   带结果统一走各自的 async / submit
// - 递归派生统一 work-first 形态(一支内联, 一支入队; TBB 行与本库行同款),
//   以深度优先的缓存局部性代表各自的最佳实践
// - 完成判定统一采用任务内计数器 + 主线程自旋等待, 不依赖任何一方的等待原语
namespace {

    using clk = std::chrono::steady_clock;
    constexpr int NAME_W = 28;
    constexpr int COL_W = 14;

    // 计时: 返回多轮中的最短耗时
    template <typename Fn>
    double best_seconds(std::size_t reps, Fn&& fn) {
        double best = 1e18;
        for (std::size_t i = 0; i < reps; ++i) {
            const auto t0 = clk::now();
            fn();
            const double s = std::chrono::duration<double>(clk::now() - t0).count();
            if (s < best) {
                best = s;
            }
        }
        return best;
    }

    // 统一完成判据: 自旋直至计数器达到期望值
    void wait_count(const std::atomic<std::size_t>& c, std::size_t expect) {
        while (c.load(std::memory_order_acquire) < expect) {
            std::this_thread::yield();
        }
    }

    // 吞吐: 每秒百万任务
    double mops(std::size_t count, double secs) {
        return static_cast<double>(count) / secs / 1e6;
    }

    // fire-and-forget 吞吐的统一骨架: 建池后 producers 个生产者线程各灌入
    // per_producer 个任务, join 生产者, 自旋等待全部完成; 取多轮最优秒数
    template <typename MakePool, typename Produce>
    double fire_secs(std::size_t reps, std::size_t producers, std::size_t per_producer,
                     MakePool make_pool, Produce produce) {
        return best_seconds(reps, [&] {
            auto p = make_pool();
            std::atomic<std::size_t> n{0};
            {
                std::vector<std::jthread> ts;
                ts.reserve(producers);
                for (std::size_t t = 0; t < producers; ++t) {
                    ts.emplace_back([&] { produce(p, n, per_producer); });
                }
            } // join 生产者
            wait_count(n, producers * per_producer);
        });
    }

    void section(std::string_view title) {
        // 长标题时取下限避免负填充
        const int pad = std::max(4, NAME_W + 40 - static_cast<int>(title.size()));
        std::println("\n== {} {:=>{}}", title, "", pad);
    }

    // Taskflow / BS / 本库三列对比表的表头, 吞吐表与耗时表共用
    void comparison_header() {
        std::println("{:<{}} {:>{}} {:>{}} {:>{}} {:>9}", "case", NAME_W, "Taskflow", COL_W, "BS",
                     COL_W, "ours", COL_W, "vs best");
    }

    void throughput_row(std::string_view name, double tf_mops, double bs_mops, double our_mops) {
        const double base = std::max(tf_mops, bs_mops);
        std::println("{:<{}} {:>{}.2f} {:>{}.2f} {:>{}.2f} {:>8.2f}x", name, NAME_W, tf_mops, COL_W,
                     bs_mops, COL_W, our_mops, COL_W, base > 0 ? our_mops / base : 0.0);
    }

    void time_row(std::string_view name, double tf_ms, double bs_ms, double our_ms) {
        const double base = std::max(1e-12, std::min(tf_ms, bs_ms));
        std::println("{:<{}} {:>{}.2f} {:>{}.2f} {:>{}.2f} {:>8.2f}x", name, NAME_W, tf_ms, COL_W,
                     bs_ms, COL_W, our_ms, COL_W, our_ms > 0 ? base / our_ms : 0.0);
    }

    void latency_row(std::string_view tag, double tf_us, double bs_us, double our_us) {
        std::println("{:<{}} {:>{}.2f} {:>{}.2f} {:>{}.2f}", tag, NAME_W, tf_us, COL_W, bs_us,
                     COL_W, our_us, COL_W);
    }

    // 负载: 累加调和级数, 结果经 volatile 消费防止被优化掉
    double spin_work(std::uint64_t iters) noexcept {
        double acc = 1.0;
        for (std::uint64_t i = 1; i <= iters; ++i) {
            acc += 1.0 / static_cast<double>(i);
        }
        return acc;
    }

    constexpr std::uint64_t LONG_ITERS = 20'000; // 长任务实算量(约数十微秒)
    constexpr std::uint64_t SHORT_ITERS = 100;   // 短任务实算量(亚微秒级)

    // 递归 fork-join 的规模: 满二叉树深度(--quick 缩减)
    constexpr std::size_t fork_depth(bool quick) noexcept { return quick ? 14 : 18; }

    // 各池的二叉递归派生: 每层两支, 叶子计数
    struct tf_fork {
        static void go(tf::Executor& ex, std::atomic<std::size_t>& leaves, std::size_t d) {
            if (d == 0) {
                leaves.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (int i = 0; i < 2; ++i) {
                ex.silent_async([&ex, &leaves, d] { go(ex, leaves, d - 1); });
            }
        }
    };
    struct bs_fork {
        using pool_t = BS::thread_pool<>;
        static void go(pool_t& p, std::atomic<std::size_t>& leaves, std::size_t d) {
            if (d == 0) {
                leaves.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (int i = 0; i < 2; ++i) {
                p.detach_task([&p, &leaves, d] { go(p, leaves, d - 1); });
            }
        }
    };
    struct cf_fork {
        using pool_t = concurrent::pool;
        static void go(pool_t& p, std::atomic<std::size_t>& leaves, std::size_t d) noexcept {
            if (d == 0) {
                leaves.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            // work-first: 一支入队供窃取, 一支内联沿深度优先(与扩展
            // 基线中 TBB 行的派生形态同款)
            static_cast<void>(p.fork_join([&p, &leaves, d]() noexcept { go(p, leaves, d - 1); },
                                          [&p, &leaves, d]() noexcept { go(p, leaves, d - 1); }));
        }
    };

    // moodycamel 队列 + 标准 CV 唤醒的自建池: 代表"最佳通用无锁队列"这一档.
    // std::function 与 BS::thread_pool 同档(通用池的惯用路径), 队列才是变量
    class mcq_pool {
    public:
        explicit mcq_pool(std::size_t n) {
            workers_.reserve(n);
            for (std::size_t i = 0; i < n; ++i) {
                workers_.emplace_back([this](std::stop_token st) { run(st); });
            }
        }
        ~mcq_pool() {
            stop_.store(true, std::memory_order_release);
            cv_.notify_all();
        }
        void enqueue(std::function<void()> f) {
            q_.enqueue(std::move(f));
            cv_.notify_one();
        }

    private:
        void run(std::stop_token st) {
            std::function<void()> f;
            while (!stop_.load(std::memory_order_acquire) && !st.stop_requested()) {
                if (q_.try_dequeue(f)) {
                    f();
                    continue;
                }
                std::unique_lock lk{mtx_};
                cv_.wait_for(lk, std::chrono::microseconds(200), [&] {
                    return stop_.load(std::memory_order_acquire) || st.stop_requested();
                });
            }
        }
        moodycamel::ConcurrentQueue<std::function<void()>> q_{};
        std::mutex mtx_;
        std::condition_variable cv_;
        std::atomic<bool> stop_{false};
        std::vector<std::jthread> workers_;
    };

    struct mcq_fork {
        static void go(mcq_pool& p, std::atomic<std::size_t>& leaves, std::size_t d) {
            if (d == 0) {
                leaves.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            for (int i = 0; i < 2; ++i) {
                p.enqueue([&p, &leaves, d] { go(p, leaves, d - 1); });
            }
        }
    };

    // 特性组合吞吐: 以 execute 单生产者为统一负载, 度量各标签组合的调度开销
    template <typename Pool>
    double fire_rate_mops(std::size_t threads, std::size_t count,
                          concurrent::trace_hooks hooks = {}) {
        typename Pool::options o{};
        o.threads = threads;
        o.hooks = std::move(hooks);
        Pool p(std::move(o));

        std::atomic<std::size_t> n{0};
        const auto t0 = clk::now();
        for (std::size_t i = 0; i < count; ++i) {
            static_cast<void>(
                p.execute([&n]() noexcept { n.fetch_add(1, std::memory_order_relaxed); }));
        }
        wait_count(n, count);

        const double secs = std::chrono::duration<double>(clk::now() - t0).count();
        return secs > 0 ? mops(count, secs) : 0.0;
    }

} // namespace

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string_view(argv[1]) == "--quick";
    const std::size_t hw = std::max<std::size_t>(std::jthread::hardware_concurrency(), 1);
    const std::size_t threads = std::min<std::size_t>(hw, 8);
    const std::size_t reps = quick ? 1 : 3;
    const std::size_t scale = quick ? 10 : 1;

    using concurrent::pool;

    // 各池的构造与 fire-and-forget 生产, 多个场景共用
    const auto make_tf = [&] { return tf::Executor(threads); };
    const auto make_bs = [&] { return BS::thread_pool(threads); };
    const auto make_ours = [&] { return pool({.threads = threads}); };
    const auto produce_tf = [](auto& ex, auto& n, std::size_t cnt) {
        for (std::size_t i = 0; i < cnt; ++i) {
            ex.silent_async([&n] { n.fetch_add(1, std::memory_order_relaxed); });
        }
    };
    const auto produce_bs = [](auto& p, auto& n, std::size_t cnt) {
        for (std::size_t i = 0; i < cnt; ++i) {
            p.detach_task([&n] { n.fetch_add(1, std::memory_order_relaxed); });
        }
    };
    const auto produce_ours = [](auto& p, auto& n, std::size_t cnt) {
        for (std::size_t i = 0; i < cnt; ++i) {
            static_cast<void>(
                p.execute([&n]() noexcept { n.fetch_add(1, std::memory_order_relaxed); }));
        }
    };

    std::println("ThreadPool benchmark: concurrent::pool vs Taskflow vs BS::thread_pool");
    std::println("hardware concurrency {}, benchmark threads {}, best of {} reps{}", hw, threads, reps,
                 quick ? " (--quick reduced scale)" : "");

    // 吞吐: fire-and-forget, 度量纯调度开销

    section("throughput: fire-and-forget single producer (M tasks/s)");
    comparison_header();
    {
        const std::size_t count = 500'000 / scale;
        const double tf = mops(count, fire_secs(reps, 1, count, make_tf, produce_tf));
        const double bs = mops(count, fire_secs(reps, 1, count, make_bs, produce_bs));
        const double ours = mops(count, fire_secs(reps, 1, count, make_ours, produce_ours));
        throughput_row("single producer", tf, bs, ours);
    }

    // 吞吐: submit 后立即取回结果, 度量结果通道成本

    section("throughput: submit + fetch result (M tasks/s)");
    comparison_header();
    {
        const std::size_t count = 200'000 / scale;

        const double tf = mops(count, best_seconds(reps, [&] {
            tf::Executor ex(threads);
            long sum = 0;
            for (std::size_t i = 0; i < count; ++i) {
                sum += ex.async([] { return 1; }).get();
            }
            if (sum != static_cast<long>(count)) {
                std::println("!! Taskflow result verification failed");
            }
        }));
        const double bs = mops(count, best_seconds(reps, [&] {
            BS::thread_pool p(threads);
            long sum = 0;
            for (std::size_t i = 0; i < count; ++i) {
                sum += p.submit_task([] { return 1; }).get();
            }
            if (sum != static_cast<long>(count)) {
                std::println("!! BS result verification failed");
            }
        }));
        const double ours = mops(count, best_seconds(reps, [&] {
            pool p({.threads = threads});
            long sum = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (auto t = p.submit([] { return 1; })) {
                    sum += t->get().value_or(0);
                }
            }
            if (sum != static_cast<long>(count)) {
                std::println("!! concurrent result verification failed");
            }
        }));
        throughput_row("submit/fetch one by one", tf, bs, ours);
    }

    // 吞吐: 递归 fork-join, 工作窃取调度的主场

    section("throughput: recursive fork-join (M leaves/s)");
    comparison_header();
    {
        const std::size_t depth = fork_depth(quick);
        const std::size_t leaves_expect = std::size_t{1} << depth;

        const double tf = mops(leaves_expect, best_seconds(reps, [&] {
            tf::Executor ex(threads);
            std::atomic<std::size_t> leaves{0};
            tf_fork::go(ex, leaves, depth);
            wait_count(leaves, leaves_expect);
        }));
        const double bs = mops(leaves_expect, best_seconds(reps, [&] {
            bs_fork::pool_t p(threads);
            std::atomic<std::size_t> leaves{0};
            bs_fork::go(p, leaves, depth);
            wait_count(leaves, leaves_expect);
        }));
        const double ours = mops(leaves_expect, best_seconds(reps, [&] {
            cf_fork::pool_t p({.threads = threads});
            std::atomic<std::size_t> leaves{0};
            cf_fork::go(p, leaves, depth);
            wait_count(leaves, leaves_expect);
        }));
        throughput_row(std::format("binary split depth {}", depth), tf, bs, ours);
    }

    // 吞吐: 多生产者竞争提交

    section("throughput: multi-producer concurrent submit (M tasks/s)");
    comparison_header();
    for (const std::size_t producers : {std::size_t{2}, std::size_t{4}, std::size_t{8}}) {
        const std::size_t each = 100'000 / scale;
        const std::size_t total = producers * each;
        const double tf = mops(total, fire_secs(reps, producers, each, make_tf, produce_tf));
        const double bs = mops(total, fire_secs(reps, producers, each, make_bs, produce_bs));
        const double ours = mops(total, fire_secs(reps, producers, each, make_ours, produce_ours));
        throughput_row(std::format("{} producers", producers), tf, bs, ours);
    }

    // 延迟: 空池 提交 -> 取回 的往返分位

    section("latency: empty-pool roundtrip submit->fetch (us, lower is better)");
    {
        const std::size_t samples = 30'000 / scale;
        struct latency {
            double p50 = 0, p90 = 0, p99 = 0, max = 0;
        };

        const auto percentile = [](std::vector<double>& v, double q) {
            const auto idx = static_cast<std::size_t>(q * static_cast<double>(v.size() - 1));
            std::ranges::nth_element(v, v.begin() + static_cast<std::ptrdiff_t>(idx));
            return v[idx];
        };
        const auto summarize = [&](std::vector<double> us) {
            latency l;
            l.max = us.empty() ? 0 : *std::ranges::max_element(us);
            l.p50 = percentile(us, 0.50);
            l.p90 = percentile(us, 0.90);
            l.p99 = percentile(us, 0.99);
            return l;
        };
        const auto roundtrip = [&](auto make_pool, auto once) -> latency {
            auto p = make_pool();
            for (std::size_t i = 0; i < 200; ++i) {
                once(p); // 预热路径、节点缓存与分支预测
            }
            std::vector<double> us;
            us.reserve(samples);
            for (std::size_t i = 0; i < samples; ++i) {
                const auto t0 = clk::now();
                once(p);
                us.push_back(std::chrono::duration<double, std::micro>(clk::now() - t0).count());
            }
            return summarize(std::move(us));
        };

        const latency tf = roundtrip(
            make_tf, [](auto& ex) { static_cast<void>(ex.async([] { return 0; }).get()); });
        const latency bs = roundtrip(
            make_bs, [](auto& p) { static_cast<void>(p.submit_task([] { return 0; }).get()); });
        const latency ours = roundtrip(make_ours, [](auto& p) {
            if (auto t = p.submit([] { return 0; })) {
                static_cast<void>(t->get());
            }
        });

        std::println("{:<{}} {:>{}} {:>{}} {:>{}}", "quantile", NAME_W, "Taskflow", COL_W, "BS",
                     COL_W, "ours", COL_W);
        latency_row("P50", tf.p50, bs.p50, ours.p50);
        latency_row("P90", tf.p90, bs.p90, ours.p90);
        latency_row("P99", tf.p99, bs.p99, ours.p99);
        latency_row("max", tf.max, bs.max, ours.max);
    }

    // 混合负载: 短任务流中掺入长任务, 观察队头阻塞表现

    section("mixed load: 10% long tasks in a short-task stream (ms, lower is better)");
    comparison_header();
    {
        const std::size_t total = 20'000 / scale;

        // 三方共用同一随机序列: 约 90% 短任务 + 10% 长任务
        std::vector<unsigned char> plan(total);
        {
            std::mt19937 rng(42);
            for (auto& v : plan) {
                v = static_cast<unsigned char>(rng() % 10 == 0 ? 1 : 0);
            }
        }
        const auto body = [](unsigned char kind, auto& done) {
            volatile double sink = spin_work(kind == 0 ? SHORT_ITERS : LONG_ITERS);
            static_cast<void>(sink);
            done.fetch_add(1, std::memory_order_relaxed);
        };

        const double tf =
            best_seconds(reps,
                         [&] {
                             tf::Executor ex(threads);
                             std::atomic<std::size_t> n{0};
                             for (auto kind : plan) {
                                 ex.silent_async([&n, kind, &body] { body(kind, n); });
                             }
                             wait_count(n, total);
                         }) *
            1e3;
        const double bs = best_seconds(reps,
                                       [&] {
                                           BS::thread_pool p(threads);
                                           std::atomic<std::size_t> n{0};
                                           for (auto kind : plan) {
                                               p.detach_task([&n, kind, &body] { body(kind, n); });
                                           }
                                           wait_count(n, total);
                                       }) *
                          1e3;
        const double ours =
            best_seconds(reps,
                         [&] {
                             pool p({.threads = threads});
                             std::atomic<std::size_t> n{0};
                             for (auto kind : plan) {
                                 static_cast<void>(
                                     p.execute([&n, kind, &body]() noexcept { body(kind, n); }));
                             }
                             wait_count(n, total);
                         }) *
            1e3;
        time_row("90% short + 10% long", tf, bs, ours);
    }

    // 扩展性: 固定实算负载随线程数的伸缩

    section("scalability: fixed real workload x threads (ms, lower is better)");
    comparison_header();
    {
        const std::size_t chunks = quick ? 64 : 512;
        const auto chunk_body = [](auto& done) {
            volatile double sink = spin_work(LONG_ITERS);
            static_cast<void>(sink);
            done.fetch_add(1, std::memory_order_relaxed);
        };
        const auto load = [&](std::size_t t, auto make_pool, auto submit_chunk) {
            return best_seconds(reps,
                                [&] {
                                    auto p = make_pool(t);
                                    std::atomic<std::size_t> done{0};
                                    for (std::size_t i = 0; i < chunks; ++i) {
                                        submit_chunk(p, done);
                                    }
                                    wait_count(done, chunks);
                                }) *
                   1e3;
        };

        std::size_t shown = 0; // threads 与列表前几项重合时跳过重复行
        for (const std::size_t t : {std::size_t{1}, std::size_t{2}, std::size_t{4}, threads}) {
            if (t > hw || t == shown) {
                continue;
            }
            shown = t;
            const double tf = load(
                t, [](std::size_t n) { return tf::Executor(n); },
                [&](auto& ex, auto& done) {
                    ex.silent_async([&done, chunk_body] { chunk_body(done); });
                });
            const double bs = load(
                t, [](std::size_t n) { return BS::thread_pool(n); },
                [&](auto& p, auto& done) {
                    p.detach_task([&done, chunk_body] { chunk_body(done); });
                });
            const double ours = load(
                t, [](std::size_t n) { return pool({.threads = n}); },
                [&](auto& p, auto& done) {
                    static_cast<void>(
                        p.execute([&done, chunk_body]() noexcept { chunk_body(done); }));
                });
            time_row(std::format("{} threads", t), tf, bs, ours);
        }
    }

    // 特性组合吞吐: 本库特有, 度量特性标签组合对调度热路径的开销

    section("feature-combo throughput: execute single producer (M tasks/s)");
    {
        const std::size_t count = 500'000 / scale;

        using prio_pool = concurrent::basic_pool<decltype(concurrent::priority)>;
        using cancel_pool = concurrent::basic_pool<decltype(concurrent::cancellable)>;
        using trace_pool = concurrent::basic_pool<decltype(concurrent::trace)>;
        using capped_pool = concurrent::basic_pool<decltype(concurrent::worker_cap<8>)>;
        using all_pool =
            concurrent::basic_pool<decltype(concurrent::priority),
                                   decltype(concurrent::cancellable), decltype(concurrent::trace),
                                   decltype(concurrent::worker_cap<8>)>;

        // 各组合的测量闭包; 交错采样让它们在相近的系统状态下被测量
        const std::vector<std::pair<std::string_view, std::function<double(std::size_t)>>> combos =
            {{"no flags", [&](std::size_t c) { return fire_rate_mops<pool>(threads, c); }},
             {"+ priority", [&](std::size_t c) { return fire_rate_mops<prio_pool>(threads, c); }},
             {"+ cancellable",
              [&](std::size_t c) { return fire_rate_mops<cancel_pool>(threads, c); }},
             {"+ trace no hooks",
              [&](std::size_t c) { return fire_rate_mops<trace_pool>(threads, c); }},
             {"+ trace on_end empty hook",
              [&](std::size_t c) {
                  concurrent::trace_hooks h;
                  h.on_end = [](concurrent::trace_event) noexcept {};
                  return fire_rate_mops<trace_pool>(threads, c, std::move(h));
              }},
             {"+ worker_cap<8>",
              [&](std::size_t c) { return fire_rate_mops<capped_pool>(threads, c); }},
             {"all flags", [&](std::size_t c) { return fire_rate_mops<all_pool>(threads, c); }}};

        struct combo_row {
            std::string_view name;
            double mops = 0;
        };
        std::vector<combo_row> rows;
        for (const auto& [name, fn] : combos) {
            rows.push_back({name, 0.0});
        }
        for (std::size_t r = 0; r < reps; ++r) {
            for (std::size_t i = 0; i < combos.size(); ++i) {
                rows[i].mops = std::max(rows[i].mops, combos[i].second(count));
            }
        }

        std::println("{:<{}} {:>12} {:>12}", "flag combo", NAME_W, "M/s", "vs no flags");
        const double base = rows.front().mops;
        for (const auto& r : rows) {
            std::println("{:<{}} {:>12.2f} {:>11.2f}x", r.name, NAME_W, r.mops,
                         base > 0 ? r.mops / base : 0.0);
        }
        std::println("note: trace hooks are compile-time switches; with no hook set only a branch remains;"
                     " the on_end row includes one virtual dispatch");
    }

    // 本库独有设施展示: 惰性并行批量端到端

    section("lazy batch: parallel_map end-to-end (ms, lower is better)");
    {
        std::vector<std::uint64_t> data(quick ? 64 : 512, LONG_ITERS);
        const double ours =
            best_seconds(reps,
                         [&] {
                             pool p({.threads = threads});
                             auto v = concurrent::parallel_map(
                                 p, data, [](std::uint64_t k) { return spin_work(k); });
                             static_cast<void>(v.run());
                         }) *
            1e3;
        std::println("{:<{}} {:>14.2f}", "parallel_map full run", NAME_W, ours);
    }

    // 扩展基线: oneTBB(系统包)与 moodycamel 队列自建池, 同负载同线程数, 独立
    // 成表(主表固定为 Taskflow / BS 两基线). oneTBB 未安装时该列打 n/a

    section("extended baselines: oneTBB / moodycamel pool");
    {
        const auto make_mcq = [&] { return mcq_pool(threads); };
        const auto produce_mcq = [](auto& p, auto& n, std::size_t cnt) {
            for (std::size_t i = 0; i < cnt; ++i) {
                p.enqueue([&n] { n.fetch_add(1, std::memory_order_relaxed); });
            }
        };
#ifdef CONCURRENT_BENCH_TBB
        const auto make_tbb = [&] { return tbb::task_arena(static_cast<int>(threads)); };
        const auto produce_tbb = [](auto& arena, auto& n, std::size_t cnt) {
            for (std::size_t i = 0; i < cnt; ++i) {
                arena.enqueue([&n] { n.fetch_add(1, std::memory_order_relaxed); });
            }
        };
#else
        std::println("oneTBB not found - its column reads n/a (install onetbb to enable it)");
#endif
        const auto cell = [](std::optional<double> v) {
            return v ? std::format("{:.2f}", *v) : std::string("n/a");
        };
        std::println("{:<{}} {:>{}} {:>{}} {:>{}} {:>9}", "case", NAME_W, "oneTBB", COL_W,
                     "mcq pool", COL_W, "ours", COL_W, "vs best");
        const auto xrow = [&](std::string_view name, std::optional<double> tbb_m, double mcq_m,
                              double our_m) {
            const double base = std::max(tbb_m.value_or(0.0), mcq_m);
            std::println("{:<{}} {:>{}} {:>{}.2f} {:>{}.2f} {:>8.2f}x", name, NAME_W, cell(tbb_m),
                         COL_W, mcq_m, COL_W, our_m, COL_W, base > 0 ? our_m / base : 0.0);
        };

        // 单生产者 fire-and-forget
        {
            const std::size_t count = 500'000 / scale;
            std::optional<double> tbb;
#ifdef CONCURRENT_BENCH_TBB
            tbb = mops(count, fire_secs(reps, 1, count, make_tbb, produce_tbb));
#endif
            const double mcq = mops(count, fire_secs(reps, 1, count, make_mcq, produce_mcq));
            const double ours = mops(count, fire_secs(reps, 1, count, make_ours, produce_ours));
            xrow("single producer", tbb, mcq, ours);
        }

        // 8 生产者竞争提交
        {
            constexpr std::size_t producers = 8;
            const std::size_t each = 100'000 / scale;
            const std::size_t total = producers * each;
            std::optional<double> tbb;
#ifdef CONCURRENT_BENCH_TBB
            tbb = mops(total, fire_secs(reps, producers, each, make_tbb, produce_tbb));
#endif
            const double mcq = mops(total, fire_secs(reps, producers, each, make_mcq, produce_mcq));
            const double ours =
                mops(total, fire_secs(reps, producers, each, make_ours, produce_ours));
            xrow(std::format("{} producers", producers), tbb, mcq, ours);
        }

        // 递归 fork-join
        {
            const std::size_t depth = fork_depth(quick);
            const std::size_t leaves_expect = std::size_t{1} << depth;
            std::optional<double> tbb;
#ifdef CONCURRENT_BENCH_TBB
            tbb = mops(leaves_expect, best_seconds(reps, [&] {
                tbb::task_arena arena(static_cast<int>(threads));
                std::atomic<std::size_t> leaves{0};
                arena.execute([&] {
                    tbb::task_group tg;
                    auto go = [&](auto&& self, std::size_t d) -> void {
                        if (d == 0) {
                            leaves.fetch_add(1, std::memory_order_relaxed);
                            return;
                        }
                        tg.run([&, d] { self(self, d - 1); });
                        self(self, d - 1);
                    };
                    go(go, depth);
                    tg.wait();
                });
                wait_count(leaves, leaves_expect);
            }));
#endif
            const double mcq = mops(leaves_expect, best_seconds(reps, [&] {
                mcq_pool p(threads);
                std::atomic<std::size_t> leaves{0};
                mcq_fork::go(p, leaves, depth);
                wait_count(leaves, leaves_expect);
            }));
            const double ours = mops(leaves_expect, best_seconds(reps, [&] {
                pool p({.threads = threads});
                std::atomic<std::size_t> leaves{0};
                cf_fork::go(p, leaves, depth);
                wait_count(leaves, leaves_expect);
            }));
            xrow(std::format("binary split depth {}", depth), tbb, mcq, ours);
        }

        // 分块并行映射: TBB parallel_for 与本库 chunked 入口同 grain 同负载.
        // 每块结果经任务内 volatile 消费: 直接内联的循环会被 DCE 整段删除
        // (实测出现过 0.00ms 的假数), volatile 写入强制实算留存
        {
            std::vector<std::uint64_t> data(quick ? 64 : 512, LONG_ITERS);
            std::println("{:<{}} {:>{}} {:>{}} {:>9}", "case (ms)", NAME_W, "oneTBB", COL_W, "ours",
                         COL_W, "vs TBB");
            std::optional<double> tbb;
#ifdef CONCURRENT_BENCH_TBB
            tbb = best_seconds(reps, [&] {
                      tbb::task_arena arena(static_cast<int>(threads));
                      arena.execute([&] {
                          tbb::parallel_for(tbb::blocked_range<std::size_t>(0, data.size(), 64),
                                            [&](const tbb::blocked_range<std::size_t>& r) {
                                                double acc = 0;
                                                for (std::size_t i = r.begin(); i != r.end(); ++i) {
                                                    acc += spin_work(data[i]);
                                                }
                                                volatile double sink = acc;
                                                static_cast<void>(sink);
                                            });
                      });
                  }) *
                  1e3;
#endif
            const double ours = best_seconds(reps, [&] {
                                    pool p({.threads = threads});
                                    auto v = concurrent::parallel_map_chunked(
                                        p, data,
                                        [](auto&& chunk) {
                                            double acc = 0;
                                            for (auto k : chunk) {
                                                acc += spin_work(k);
                                            }
                                            volatile double sink = acc;
                                            static_cast<void>(sink);
                                        },
                                        64);
                                    static_cast<void>(v.run());
                                }) *
                                1e3;
            const std::string ratio =
                tbb && *tbb > 0 ? std::format("{:.2f}x", ours / *tbb) : std::string("n/a");
            std::println("{:<{}} {:>{}} {:>{}.2f} {:>9}", "parallel map x64 chunks", NAME_W,
                         cell(tbb), COL_W, ours, COL_W, ratio);
        }
    }

    std::println("\nnote: ratio is relative to \"best baseline\" - throughput = ours/best,"
                 " time = best/ours");
    return 0;
}
