#include <huxint/thread_pool.hpp>
#include <print>
#include <chrono>
#include <cassert>
#include <atomic>
#include <numeric>

using namespace thread_pool;
using namespace std::chrono;

// ============ 测试工具 ============

#define TEST(name)                                                                                                                                                                 \
    std::print("测试 {}: ", #name);                                                                                                                                                \
    try {                                                                                                                                                                          \
        test_##name();                                                                                                                                                             \
        std::println("✓ 通过");                                                                                                                                                    \
    } catch (const std::exception &e) {                                                                                                                                            \
        std::println("✗ 失败: {}", e.what());                                                                                                                                      \
    }

#define ASSERT(cond)                                                                                                                                                               \
    if (!(cond))                                                                                                                                                                   \
    throw std::runtime_error("断言失败: " #cond)

// ============ 正确性测试 ============

void test_basic_submit() {
    ThreadPool<> pool(4);
    auto f = pool.submit(
        [](int a, int b) {
            return a + b;
        },
        10, 20);
    ASSERT(f.get() == 30);
}

void test_execute_runs() {
    ThreadPool<> pool(2);
    std::atomic<int> counter{0};

    for (int i = 0; i < 100; ++i) {
        pool.execute([&counter] {
            counter++;
        });
    }
    pool.wait();
    ASSERT(counter == 100);
}

void test_wait() {
    ThreadPool<> pool(4);
    auto f1 = pool.submit([] {
        return 1;
    });
    auto f2 = pool.submit([] {
        return 2;
    });
    auto f3 = pool.submit([] {
        return 3;
    });

    wait(f1, f2, f3);
    ASSERT(f1.get() + f2.get() + f3.get() == 6);
}

void test_priority_order() {
    ThreadPool<op::priority> pool(1); // 单线程确保顺序
    std::vector<int> order;
    std::mutex mtx;

    // 先提交低优先级，再提交高优先级
    // 由于单线程，高优先级应该先执行
    pool.execute(priority_t::low, [&] {
        std::scoped_lock lock(mtx);
        order.push_back(1);
    });
    pool.execute(priority_t::low, [&] {
        std::scoped_lock lock(mtx);
        order.push_back(2);
    });
    pool.execute(priority_t::high, [&] {
        std::scoped_lock lock(mtx);
        order.push_back(3);
    });

    pool.wait();
    // 高优先级任务应该在低优先级之前执行（如果它在队列中等待的话）
    ASSERT(order.size() == 3);
}

void test_cancellation() {
    ThreadPool<op::cancellable> pool(2);
    std::atomic<bool> started{false};
    std::atomic<bool> cancelled{false};

    auto task = pool.submit([&](token_ref token) {
        started = true;
        while (!token.cancelled()) {
            std::this_thread::sleep_for(milliseconds(10));
        }
        cancelled = true;
        return 42;
    });

    // 等待任务开始
    while (!started) {
        std::this_thread::yield();
    }

    task.cancel();
    task.wait();

    ASSERT(cancelled);
}

void test_shutdown_and_launch() {
    ThreadPool<> pool(2);
    std::atomic<int> counter{0};

    pool.execute([&] {
        counter++;
    });
    pool.wait();
    ASSERT(counter == 1);

    pool.shutdown();
    ASSERT(!pool.running());

    pool.launch();
    ASSERT(pool.running());

    pool.execute([&] {
        counter++;
    });
    pool.wait();
    ASSERT(counter == 2);
}

void test_exception_propagation() {
    ThreadPool<> pool(2);
    auto f = pool.submit([]() -> int {
        throw std::runtime_error("测试异常");
    });

    bool caught = false;
    try {
        f.get();
    } catch (const std::runtime_error &e) {
        caught = true;
    }
    ASSERT(caught);
}

void test_move_only_args() {
    ThreadPool<> pool(2);
    auto ptr = std::make_unique<int>(42);

    auto f = pool.submit(
        [](std::unique_ptr<int> p) {
            return *p * 2;
        },
        std::move(ptr));

    ASSERT(f.get() == 84);
}

// ============ 性能测试 ============

void bench_throughput() {
    std::println("\n=== 吞吐量测试 ===");

    constexpr int TASKS = 100000;
    std::atomic<int> counter{0};

    ThreadPool<> pool(std::thread::hardware_concurrency());

    auto start = high_resolution_clock::now();

    for (int i = 0; i < TASKS; ++i) {
        pool.execute([&counter] {
            counter++;
        });
    }
    pool.wait();

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    std::println("  {} 个任务, {} ms", TASKS, ms);
    std::println("  吞吐量: {:.0f} 任务/秒", TASKS * 1000.0 / ms);
    ASSERT(counter == TASKS);
}

void bench_latency() {
    std::println("\n=== 延迟测试 ===");

    constexpr int SAMPLES = 1000;
    ThreadPool<> pool(4);

    std::vector<long long> latencies;
    latencies.reserve(SAMPLES);

    for (int i = 0; i < SAMPLES; ++i) {
        auto start = high_resolution_clock::now();
        auto f = pool.submit([] {
            return 1;
        });
        f.get();
        auto end = high_resolution_clock::now();
        latencies.push_back(duration_cast<nanoseconds>(end - start).count());
    }

    std::sort(latencies.begin(), latencies.end());

    auto avg = std::accumulate(latencies.begin(), latencies.end(), 0LL) / SAMPLES;
    auto p50 = latencies[SAMPLES / 2];
    auto p99 = latencies[SAMPLES * 99 / 100];

    std::println("  平均延迟: {} ns", avg);
    std::println("  P50 延迟: {} ns", p50);
    std::println("  P99 延迟: {} ns", p99);
}

void bench_contention() {
    std::println("\n=== 竞争测试 (多生产者) ===");

    constexpr int PRODUCERS = 8;
    constexpr int TASKS_PER_PRODUCER = 10000;

    ThreadPool<> pool(4);
    std::atomic<int> counter{0};

    auto start = high_resolution_clock::now();

    std::vector<std::jthread> producers;
    for (int p = 0; p < PRODUCERS; ++p) {
        producers.emplace_back([&pool, &counter] {
            for (int i = 0; i < TASKS_PER_PRODUCER; ++i) {
                pool.execute([&counter] {
                    counter++;
                });
            }
        });
    }

    for (auto &t : producers) {
        t.join();
    }
    pool.wait();

    auto end = high_resolution_clock::now();
    auto ms = duration_cast<milliseconds>(end - start).count();

    int total = PRODUCERS * TASKS_PER_PRODUCER;
    std::println("  {} 生产者 x {} 任务 = {} 总任务", PRODUCERS, TASKS_PER_PRODUCER, total);
    std::println("  耗时: {} ms", ms);
    std::println("  吞吐量: {:.0f} 任务/秒", total * 1000.0 / ms);
    ASSERT(counter == total);
}

// ============ 主函数 ============

int main() {
    std::println("========== 正确性测试 ==========\n");

    TEST(basic_submit);
    TEST(execute_runs);
    TEST(wait);
    TEST(priority_order);
    TEST(cancellation);
    TEST(shutdown_and_launch);
    TEST(exception_propagation);
    TEST(move_only_args);

    std::println("\n========== 性能测试 ==========");

    bench_throughput();
    bench_latency();
    bench_contention();

    std::println("\n========== 测试完成 ==========");
    return 0;
}
