#pragma once
// 测试公共工具: 与框架无关的异步观测辅助, 断言一律使用 doctest 宏
#include <atomic>
#include <chrono>
#include <thread>

namespace tu {

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

} // namespace tu
