#pragma once
// 测试公共工具: 与框架无关的异步观测辅助, 断言一律使用 doctest 宏
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace tu {

    // 挂死看门狗. 死锁类回归测试一旦失败, 表现是整个测试进程卡住, CI 只能等到
    // 超时且不知卡在哪. 以此把"挂死"转成一次带明确诊断的立即中止
    class deadlock_watchdog {
    public:
        deadlock_watchdog(std::chrono::seconds limit, const char* what)
            : t_([limit, what](std::stop_token st) {
                  const auto deadline = std::chrono::steady_clock::now() + limit;
                  while (!st.stop_requested()) {
                      if (std::chrono::steady_clock::now() >= deadline) {
                          std::fprintf(stderr, "\n[deadlock_watchdog] \"%s\" not finished within %lld s, deadlock assumed\n",
                                       what, static_cast<long long>(limit.count()));
                          std::fflush(stderr);
                          std::abort();
                      }
                      std::this_thread::sleep_for(std::chrono::milliseconds(50));
                  }
              }) {}

        deadlock_watchdog(const deadlock_watchdog&) = delete;
        deadlock_watchdog& operator=(const deadlock_watchdog&) = delete;

    private:
        std::jthread t_;
    };

    // 用若干自旋占位任务卡住全部 worker, 使随后提交的任务必然处于排队状态
    struct gate {
        std::atomic<bool> open{false};

        template <typename Pool>
        void block_all(Pool& p, std::size_t workers) {
            for (std::size_t i = 0; i < workers; ++i) {
                static_cast<void>(p.execute([this]() noexcept {
                    while (!open.load(std::memory_order_acquire)) {
                        std::this_thread::yield();
                    }
                }));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5)); // 确保已进入阻塞体
        }

        void release() { open.store(true, std::memory_order_release); }
    };

    // 存活实例计数. 用于验证"结果值即使从未被消费也会被析构" - 共享状态里的
    // value_ 是裸字节缓冲, 漏掉析构不会有任何症状, 只能靠计数观测
    struct tracked {
        static inline std::atomic<int> live{0};
        int v = 0;

        explicit tracked(int x = 0) noexcept : v(x) { live.fetch_add(1, std::memory_order_relaxed); }
        tracked(const tracked& o) noexcept : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
        tracked(tracked&& o) noexcept : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
        tracked& operator=(const tracked&) = default;
        tracked& operator=(tracked&&) = default;
        ~tracked() { live.fetch_sub(1, std::memory_order_relaxed); }
    };

    // 只可拷贝, 不可移动(声明了拷贝构造 -> 隐式移动构造被抑制, 右值走拷贝).
    // 取值路径若只把源对象"移动"走却不析构它, 这种类型会整个泄漏
    struct copy_only {
        static inline std::atomic<int> live{0};
        int v = 0;

        explicit copy_only(int x = 0) noexcept : v(x) { live.fetch_add(1, std::memory_order_relaxed); }
        copy_only(const copy_only& o) noexcept : v(o.v) { live.fetch_add(1, std::memory_order_relaxed); }
        copy_only& operator=(const copy_only&) = default;
        ~copy_only() { live.fetch_sub(1, std::memory_order_relaxed); }
    };

} // namespace tu
