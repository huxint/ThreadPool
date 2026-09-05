#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace concurrent::detail {

    /// Vyukov 有界 MPMC 环形队列. 槽类型为指针
    /// 快路径纯无锁; 满/空语义由调用方处理(try_push 返回 false, try_pop 返回 nullptr)
    template <typename T, std::size_t Capacity>
        requires(Capacity >= 2 && std::has_single_bit(Capacity) && std::is_pointer_v<T>)
    class mpmc_ring {
        static constexpr std::size_t mask = Capacity - 1;

    public:
        mpmc_ring() noexcept {
            for (std::size_t i = 0; i < Capacity; ++i) {
                cells_[i].seq.store(i, std::memory_order_relaxed);
            }
        }

        /// 入队一个元素; 满则返回 false
        bool try_push(T value) noexcept {
            std::size_t pos = tail_.load(std::memory_order_relaxed);
            for (;;) {
                cell& c = cells_[pos & mask];
                std::intptr_t seq = c.seq.load(std::memory_order_acquire);
                std::intptr_t dif = seq - static_cast<std::intptr_t>(pos);
                if (dif == 0) {
                    if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                        // seq == pos 即该槽对本轮位置为空; CAS 成功后本线程独占
                        // 该槽, 直至把 seq 发布为 pos+1
                        c.value = value;
                        c.seq.store(static_cast<std::intptr_t>(pos) + 1, std::memory_order_release);
                        return true;
                    }
                } else if (dif < 0) {
                    return false; // 队列已满
                } else {
                    pos = tail_.load(std::memory_order_relaxed); // tail 落后, 重读
                }
            }
        }

        /// 出队一个元素; 空则返回 nullptr
        [[nodiscard]]
        T try_pop() noexcept {
            std::size_t pos = head_.load(std::memory_order_relaxed);
            for (;;) {
                cell& c = cells_[pos & mask];
                std::intptr_t seq = c.seq.load(std::memory_order_acquire);
                std::intptr_t dif = seq - (static_cast<std::intptr_t>(pos) + 1);
                if (dif == 0) {
                    if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                        // seq == pos+1 即该槽已写入待弹出; CAS 成功后本线程独占
                        // 该槽, 直至把 seq 发布为 pos+Capacity(下一轮的空)
                        T v = c.value;
                        c.seq.store(static_cast<std::intptr_t>(pos) + Capacity,
                                    std::memory_order_release);
                        return v;
                    }
                } else if (dif < 0) {
                    return nullptr; // 队列为空
                } else {
                    pos = head_.load(std::memory_order_relaxed); // head 落后, 重读
                }
            }
        }

        /// 一次 CAS 领取连续已发布的槽; seq 在读取值后才释放, 生产者不会提前复用.
        std::size_t try_pop_batch(std::span<T> out) noexcept {
            const std::size_t limit = out.size() < Capacity ? out.size() : Capacity;
            if (limit == 0) {
                return 0;
            }
            std::size_t pos = head_.load(std::memory_order_relaxed);
            for (;;) {
                std::size_t count = 0;
                std::intptr_t dif = 0;
                for (; count < limit; ++count) {
                    const auto slot = pos + count;
                    const auto seq = cells_[slot & mask].seq.load(std::memory_order_acquire);
                    dif = seq - (static_cast<std::intptr_t>(slot) + 1);
                    if (dif != 0) {
                        break;
                    }
                }
                if (count == 0) {
                    if (dif < 0) {
                        return 0;
                    }
                    pos = head_.load(std::memory_order_relaxed);
                    continue;
                }
                if (!head_.compare_exchange_weak(pos, pos + count, std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    continue;
                }
                for (std::size_t i = 0; i < count; ++i) {
                    cell& c = cells_[(pos + i) & mask];
                    out[i] = c.value;
                    c.seq.store(static_cast<std::intptr_t>(pos + i) + Capacity,
                                std::memory_order_release);
                }
                return count;
            }
        }

        [[nodiscard]]
        std::size_t size_approx() const noexcept {
            auto h = head_.load(std::memory_order_relaxed);
            auto t = tail_.load(std::memory_order_relaxed);
            return t > h ? t - h : 0;
        }

    private:
        /// 槽按缓存行填充: 相邻槽由不同生产者同时写入(tail_ 递增即分发相邻
        /// 位置), 密排时一行内四个槽的写入互相失效. 实测 8 生产者吞吐差约
        /// 8%. 代价是环体积四倍, 池的常驻内存因此由容量直接决定
        /// (见 queue_cap 缺省值的取舍)
        struct alignas(64) cell {
            std::atomic<std::intptr_t> seq;
            T value{};
        };

        alignas(64) std::atomic<std::size_t> head_{0};
        alignas(64) std::atomic<std::size_t> tail_{0};
        std::array<cell, Capacity> cells_;
    };

} // namespace concurrent::detail
