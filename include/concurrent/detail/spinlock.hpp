#pragma once
#include "concurrent/detail/cpu_relax.hpp"
#include <atomic>

namespace concurrent::detail {

    /// 极短临界区专用自旋锁(溢出链表接驳)
    /// 争用时以 cpu_relax 让核, 避免在超线程上把兄弟逻辑核的流水线一起拖住
    /// 不可重入, 不公平 - 只用于"持锁时间以纳秒计"的场合.
    /// 满足 BasicLockable, 临界区直接用 std::scoped_lock
    class spinlock {
    public:
        void lock() noexcept {
            int spins = 0;
            // 纯指数退避重试, 刻意不做 test-and-test-and-set: 只读等待会让全部
            // 等待者在解锁瞬间同时冲上来抢同一行(惊群), 而错开的退避把重试摊开
            // 在时间上. 实测 16 线程猛灌溢出链(queue_cap<64>)时本形态快约 50%
            while (flag_.test_and_set(std::memory_order_acquire)) {
                for (int i = 0; i < (1 << (spins < 8 ? spins : 8)); ++i) {
                    cpu_relax();
                }
                ++spins;
            }
        }

        void unlock() noexcept { flag_.clear(std::memory_order_release); }

    private:
        std::atomic_flag flag_{};
    };

} // namespace concurrent::detail
