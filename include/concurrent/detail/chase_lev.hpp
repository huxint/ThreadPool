#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <type_traits>

namespace concurrent::detail {

    /// 固定容量 Chase-Lev 工作窃取双端队列(环形缓冲 + 单调递增索引)
    /// 槽类型为指针(池内为 task_node*)
    ///
    /// 槽以 relaxed 原子量访问, 数据可见性经 bottom 的 release/acquire 对
    /// 传递(见 push); 归属判定完全由 top/bottom 的索引协议完成
    /// ("先读槽, 后 CAS", 竞争失败者丢弃所读值)
    ///
    /// pop/steal 各保留一道 seq_cst fence: 二者构成 Dekker 式配对,
    /// 杜绝"最后一个元素归属判定"的双赢/双输
    ///
    /// bottom 端仅所有者操作(LIFO), top 端被窃取者竞争(FIFO)
    template <typename T, std::size_t Capacity>
        requires(std::has_single_bit(Capacity) && Capacity >= 2 && std::is_pointer_v<T>)
    class chase_lev_deque {
        static constexpr std::size_t mask = Capacity - 1;

    public:
        /// 仅所有者线程调用. 满则返回 false(不阻塞, 不覆盖)
        bool push(T value) noexcept {
            std::size_t b = bottom_.load(std::memory_order_relaxed);
            if (b - top_.load(std::memory_order_acquire) >= Capacity) {
                return false;
            }
            // 通过容量检查即槽 b&mask 归本线程独占: bottom 发布前窃取者读不到它
            slots_[b & mask].store(value, std::memory_order_relaxed);
            // release 发布: 窃取者的 bottom acquire 载入一旦观察到 b+1,
            // 即与本次槽写入建立 happens-before(论文用独立 fence 表达同一
            // 定序, 但 ThreadSanitizer 不建模裸 fence, 故改用序内建于原子量)
            bottom_.store(b + 1, std::memory_order_release);
            return true;
        }

        /// 仅所有者线程调用. LIFO 弹出; 空返回 nullptr
        [[nodiscard]]
        T pop() noexcept {
            std::size_t b = bottom_.load(std::memory_order_relaxed);
            if (b == top_.load(std::memory_order_acquire)) {
                return nullptr;
            }
            b -= 1;
            bottom_.store(b, std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_seq_cst); // 与 steal 的 CAS 竞争
            std::size_t t = top_.load(std::memory_order_relaxed);
            if (t <= b) {
                T v = slots_[b & mask].load(std::memory_order_relaxed);
                if (t == b) { // 最后一个元素: 与窃取者 CAS 决出归属
                    if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                                      std::memory_order_relaxed)) {
                        v = nullptr; // 输给窃取者(赢家已认领该元素)
                    }
                    bottom_.store(b + 1, std::memory_order_relaxed);
                }
                return v;
            }
            bottom_.store(b + 1, std::memory_order_relaxed); // 队列已被偷空, 回滚
            return nullptr;
        }

        /// 任意线程调用. FIFO 偷取队头; 空或竞争失败返回 nullptr
        [[nodiscard]]
        T steal() noexcept {
            // 无栅栏预检: 空队列直接走人, 不付 Dekker 栅栏的代价. 误判"空"
            // 完全良性(调用方本就容忍偷取失败), 漏判由下方完整路径兜住.
            // 空闲 worker 在自旋预算里反复扫 n_threads x LEVELS 个 victim,
            // 稳态下全部命中此快路径
            if (top_.load(std::memory_order_relaxed) >= bottom_.load(std::memory_order_relaxed)) {
                return nullptr;
            }
            std::size_t t = top_.load(std::memory_order_acquire);
            std::atomic_thread_fence(std::memory_order_seq_cst); // 与 pop 的 bottom 递减定序
            std::size_t b = bottom_.load(std::memory_order_acquire);
            if (t >= b) {
                return nullptr;
            }
            // 必须"先读槽, 后 CAS": CAS 一旦成功 top 即推进, 所有者的 push
            // 容量检查随之放行同一槽位并写入新值; 此时再读(或写回)该槽
            // 会取到被覆盖的值 / 清空刚入队的任务. 竞争失败者丢弃所读指针
            T v = slots_[t & mask].load(std::memory_order_relaxed);
            if (!top_.compare_exchange_strong(t, t + 1, std::memory_order_seq_cst,
                                              std::memory_order_relaxed)) {
                return nullptr;
            }
            return v;
        }

        /// 近似尺寸(仅调度启发与观测用, 不作同步依据)
        [[nodiscard]]
        std::size_t size_approx() const noexcept {
            auto b = bottom_.load(std::memory_order_relaxed);
            auto t = top_.load(std::memory_order_relaxed);
            return b > t ? b - t : 0;
        }

    private:
        alignas(64) std::atomic<std::size_t> bottom_{0}; ///< 仅所有者写
        alignas(64) std::atomic<std::size_t> top_{0};    ///< 所有者与窃取者竞争
        std::array<std::atomic<T>, Capacity> slots_{};
    };

} // namespace concurrent::detail
