#pragma once
#include <cstdint>
#include <functional>

namespace thread_pool {
/**
 * @brief 任务优先级等级
 */
enum class priority_t : std::uint8_t {
    low = 0,
    normal = 1,
    high = 2
};

/**
 * @brief 优先级任务包装器
 * @details 将任务与优先级绑定，支持优先级队列排序
 */
struct priority_task_t {
    priority_t priority;
    std::move_only_function<void()> task;

    explicit priority_task_t(priority_t priority_, std::move_only_function<void()> &&task_) noexcept(std::is_nothrow_move_constructible_v<std::move_only_function<void()>>)
    : priority(priority_),
      task(std::move(task_)) {}

    [[nodiscard]]
    friend bool operator<(const priority_task_t &lhs, const priority_task_t &rhs) noexcept {
        return lhs.priority < rhs.priority;
    }
};
} // namespace thread_pool
