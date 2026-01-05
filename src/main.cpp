#include <huxint/thread_pool.hpp>
#include <print>
#include <chrono>

using namespace thread_pool;

int main() {
    ThreadPool<> pool(4);

    pool.execute([] {
        std::println("execute: 后台任务执行 不关心返回值");
    });

    auto f1 = pool.submit([] {
        return true;
    });

    auto f2 = pool.submit([](int a = 10, int b = 20) -> int {
        return a + b;
    });

    thread_pool::wait(f1, f2);
    auto [r1, r2] = thread_pool::collect(f1, f2);
    std::println("结果: {}, {}", r1, r2);

    ThreadPool<op::cancellable> log(2);
    auto log_task = log.execute(
        [](token_ref token, const std::string &msg) {
            for (int i = 0; i < 5; ++i) {
                if (token.cancelled()) {
                    std::println("日志任务已取消: {}", msg);
                    return;
                }
                std::println("日志: {} ({}/{})", msg, i + 1, 5);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        },
        "重要日志");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    log_task.cancel(); // 取消日志任务
    return 0;
}