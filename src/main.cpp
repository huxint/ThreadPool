#include <huxint/thread_pool.hpp>
#include <print>
#include <chrono>

using namespace thread_pool;

int main() {
    ThreadPool<> pool(4);

    pool.execute([] {
        std::println("execute: background task, return value ignored");
    });

    auto f1 = pool.submit([] {
        return true;
    });

    auto f2 = pool.submit([](int a = 10, int b = 20) -> int {
        return a + b;
    });

    thread_pool::wait(f1, f2);
    auto [r1, r2] = thread_pool::collect(f1, f2);
    std::println("result: {}, {}", r1, r2);

    ThreadPool<op::cancellable> log(2);
    auto log_task = log.execute(
        [](token_ref token, const std::string &msg) {
            for (int i = 0; i < 5; ++i) {
                if (token.cancelled()) {
                    std::println("log task cancelled: {}", msg);
                    return;
                }
                std::println("log: {} ({}/{})", msg, i + 1, 5);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        },
        "important log");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    log_task.cancel(); // 取消日志任务
    return 0;
}