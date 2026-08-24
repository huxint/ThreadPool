#pragma once
#include <atomic>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace concurrent::detail {

/// 固定容量 Chase-Lev 工作窃取双端队列（环形缓冲 + 单调递增索引）。
/// 槽类型必须平凡可拷贝（池内为 task_node*），以恢复原论文的读-再-CAS 安全语义：
/// 竞争失败者丢弃其读到的指针值，赢家持有的指针必然有效。
/// bottom 端仅所有者操作（LIFO），top 端被窃取者竞争（FIFO）。
template <typename T, std::size_t Capacity>
    requires((Capacity & (Capacity - 1)) == 0 && Capacity >= 2 && std::is_trivially_copyable_v<T>)
class chase_lev_deque {
    static constexpr std::size_t mask = Capacity - 1;

public:
    /// 仅所有者线程调用。满则返回 false（不阻塞、不覆盖）。
    bool push(T value) noexcept {
        std::size_t b = bottom_.load(std::memory_order_relaxed);
        if (b - top_.load(std::memory_order_acquire) >= Capacity)
            return false;
        slots_[b & mask] = value;
        std::atomic_thread_fence(std::memory_order_release); // 槽写入先于 bottom 发布
        bottom_.store(b + 1, std::memory_order_relaxed);
        return true;
    }

    /// 仅所有者线程调用。LIFO 弹出；空返回 nullptr。
    [[nodiscard]]
    T pop() noexcept {
        std::size_t b = bottom_.load(std::memory_order_relaxed);
        if (b == top_.load(std::memory_order_acquire))
            return nullptr;
        b -= 1;
        bottom_.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst); // 与 steal 的 CAS 竞争
        std::size_t t = top_.load(std::memory_order_relaxed);
        if (t <= b) {
            T v = slots_[b & mask];
            if (t == b) { // 最后一个元素：与窃取者 CAS 决出归属
                if (!top_.compare_exchange_strong(t, t + 1,
                                                   std::memory_order_seq_cst,
                                                   std::memory_order_relaxed))
                    v = nullptr; // 输给窃取者（赢家已认领该元素）
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            return v;
        }
        bottom_.store(b + 1, std::memory_order_relaxed); // 队列已被偷空，回滚
        return nullptr;
    }

    /// 任意线程调用。FIFO 偷取队头；空或竞争失败返回 nullptr。
    [[nodiscard]]
    T steal() noexcept {
        std::size_t t = top_.load(std::memory_order_acquire);
        if (t >= bottom_.load(std::memory_order_acquire))
            return nullptr;
        if (!top_.compare_exchange_strong(t, t + 1,
                                           std::memory_order_seq_cst,
                                           std::memory_order_relaxed))
            return nullptr;
        // 赢家唯一，此后对该槽独占访问
        return std::exchange(slots_[t & mask], nullptr);
    }

    /// 近似尺寸（仅调度启发与观测用，不作同步依据）
    [[nodiscard]]
    std::size_t size_approx() const noexcept {
        auto b = bottom_.load(std::memory_order_relaxed);
        auto t = top_.load(std::memory_order_relaxed);
        return b > t ? b - t : 0;
    }

private:
    alignas(64) std::atomic<std::size_t> bottom_{0}; ///< 仅所有者写
    alignas(64) std::atomic<std::size_t> top_{0};    ///< 所有者与窃取者竞争
    T slots_[Capacity]{};
};

} // namespace concurrent::detail
