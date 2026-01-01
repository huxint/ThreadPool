#include <print>
#include <huxint/thread_pool>

using namespace huxint;

// 测试基础线程池
void test_basic_pool() {
    ThreadPool<> pool(4);

    // 测试1: 简单任务
    auto f1 = pool.submit([] {
        return 42;
    });
    std::println("{}", f1.get());

    // 测试2: 带参数的任务
    auto f2 = pool.submit(
        [](int a, int b) {
            return a + b;
        },
        10,
        20);
    std::println("{}", f2.get());

    // 测试3: 多个并发任务
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.submit([i] {
            return i * i;
        }));
    }
    int sum = 0;
    for (auto &f : futures) {
        sum += f.get();
    }
    std::println("{}", sum);

    // 测试4: void 返回类型
    std::atomic<int> counter{0};
    std::vector<std::future<void>> void_futures;
    for (int i = 0; i < 5; ++i) {
        void_futures.push_back(pool.submit([&counter] {
            ++counter;
        }));
    }
    for (auto &f : void_futures) {
        f.get();
    }
    std::println("{}", counter.load());

    // 测试5: 异常传播
    auto f_ex = pool.submit([]() -> int {
        throw std::runtime_error("test error");
    });
    try {
        f_ex.get();
        std::println("no exception");
    } catch (const std::runtime_error &e) {
        std::println("{}", e.what());
    }
}

// 测试优先级线程池
void test_priority_pool() {
    ThreadPool<op::priority> pool(1); // 单线程以确保顺序

    std::vector<int> execution_order;
    std::mutex order_mutex;

    auto low = pool.submit(priority_t::low, [&] {
        std::scoped_lock lock(order_mutex);
        execution_order.push_back(1);
    });
    auto normal = pool.submit(priority_t::normal, [&] {
        std::scoped_lock lock(order_mutex);
        execution_order.push_back(2);
    });
    auto high = pool.submit(priority_t::high, [&] {
        std::scoped_lock lock(order_mutex);
        execution_order.push_back(3);
    });

    pool.wait();

    std::println("{}", execution_order);
}

// 测试可取消线程池
void test_cancellable_pool() {
    ThreadPool<op::cancellable> pool(4);

    // 测试1: 正常完成
    auto task1 = pool.submit_cancellable([](const cancellation_token &token) {
        int sum = 0;
        for (int i = 0; i < 100 && !token.is_cancelled(); ++i) {
            sum += i;
        }
        return sum;
    });
    std::println("{}", task1.get());

    // 测试2: 取消正在运行的任务
    auto task2 = pool.submit_cancellable([](const cancellation_token &token) {
        int iterations = 0;
        while (!token.is_cancelled()) {
            ++iterations;
        }
        return iterations;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    task2.cancel();
    std::println("{} {}", task2.get(), task2.is_cancelled());

    // 测试3: 带参数的可取消任务
    auto task3 = pool.submit_cancellable(
        [](const cancellation_token &token, int start, int end) {
            std::int64_t sum = 0;
            for (int i = start; i < end && !token.is_cancelled(); ++i) {
                sum += i;
            }
            return sum;
        },
        1,
        100000000);

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    task3.cancel();
    std::println("sum is {}", task3.get());
}

int main() {
    test_basic_pool();
    test_priority_pool();
    test_cancellable_pool();
    std::print("All tests completed!\n");
    return 0;
}