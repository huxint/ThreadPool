#include <atomic>
#include <chrono>
#include <concurrent/concurrent.hpp>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace concurrent;

int main() {
    // 基础用法: submit 返回 expected<task<T>, submit_error>
    pool p({.threads = 4});

    // 即发即忘(callable 需 noexcept)
    std::atomic<int> fire{0};
    static_cast<void>(p.execute([&fire]() noexcept { fire.fetch_add(1, std::memory_order_relaxed); }));
    p.wait();
    std::println("fire-and-forget count: {}", fire.load());

    // 有返回值
    auto f1 = p.submit([] { return true; });
    auto f2 = p.submit([](int a, int b) { return a + b; }, 10, 20);

    if (f1 && f2) {
        std::println("result: {}, {}", *f1->get(), *f2->get());
    }

    // 函数式组合子: when_all + map
    auto a = p.submit([] { return 100; });
    auto b = p.submit([] { return 200; });
    if (a && b) {
        auto sum = when_all(std::move(*a), std::move(*b)).map([](auto&& tup) {
            return std::get<0>(tup) + std::get<1>(tup);
        });
        if (auto r = sum.get()) {
            std::println("when_all sum: {}", *r);
        }
    }

    // 惰性并行批量: 构造不提交, begin()/run() 时整批入队, 按输入顺序取回
    std::vector<int> data{1, 2, 3, 4, 5, 6, 7, 8};
    auto squares = parallel_map(p, data, [](int x) noexcept { return x * x; });
    for (auto&& r : squares) {
        if (r) {
            std::println("square: {}", *r);
        }
    }

    // 协作取消: callable 接收 std::stop_token
    pool log_pool({.threads = 2});
    auto log_task = log_pool.submit(
        [](std::stop_token tok, const std::string& msg) {
            for (int i = 0; i < 5; ++i) {
                if (tok.stop_requested()) {
                    std::println("log task cancelled: {}", msg);
                    return;
                }
                std::println("log: {} ({}/{})", msg, i + 1, 5);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        },
        "important log");

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (log_task) {
        log_task->request_stop(); // 请求取消日志任务
    }

    // 带优先级(callable 需 noexcept)
    basic_pool<decltype(priority)> prio_pool({.threads = 2});
    static_cast<void>(prio_pool.execute(task_priority::low, []() noexcept { std::println("low-priority task"); }));
    static_cast<void>(prio_pool.execute(task_priority::high, []() noexcept { std::println("high-priority task"); }));
    prio_pool.wait();

    // trace 钩子: 三阶段事件流(enqueue / begin / end)
    basic_pool<decltype(trace)> traced(
        {.threads = 2,
         .hooks = {.on_enqueue = {},
                   .on_begin = {},
                   .on_end =
                       [](trace_event e) noexcept {
                           std::println("[trace] task {} finished, outcome {}", e.id,
                                        static_cast<int>(e.outcome));
                       }}});
    static_cast<void>(traced.execute([]() noexcept {}));
    traced.wait();

    return 0;
}
