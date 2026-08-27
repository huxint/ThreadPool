#pragma once
#include "concurrent/detail/cpu_relax.hpp"
#include <atomic>

namespace concurrent::detail {

    /// 极短临界区专用自旋锁(续延链表头操作, 溢出链表接驳)
    /// 争用时以 cpu_relax 让核, 避免在超线程上把兄弟逻辑核的流水线一起拖住
    /// 不可重入, 不公平 - 只用于"持锁时间以纳秒计"的场合.
    /// 满足 BasicLockable, 临界区直接用 std::scoped_lock
    class spinlock {
    public:
        void lock() noexcept {
            while (flag_.test_and_set(std::memory_order_acquire)) {
                while (
                    flag_.test(std::memory_order_relaxed)) { // 先只读探测, 避免独占缓存行的写风暴
                    cpu_relax();
                }
            }
        }

        void unlock() noexcept { flag_.clear(std::memory_order_release); }

        [[nodiscard]]
        bool try_lock() noexcept {
            return !flag_.test_and_set(std::memory_order_acquire);
        }

    private:
        std::atomic_flag flag_{};
    };

} // namespace concurrent::detail
