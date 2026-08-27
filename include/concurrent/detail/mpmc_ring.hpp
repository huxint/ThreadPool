#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace concurrent::detail {

    /// Vyukov 有界 MPMC 环形队列. 槽类型为平凡可拷贝指针
    /// 快路径纯无锁; 满/空语义由调用方处理(try_push 返回 false, try_pop 返回 nullptr)
    template <typename T, std::size_t Capacity>
        requires(std::has_single_bit(Capacity) && std::is_trivially_copyable_v<T>)
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

        [[nodiscard]]
        std::size_t size_approx() const noexcept {
            auto h = head_.load(std::memory_order_acquire);
            auto t = tail_.load(std::memory_order_acquire);
            return t > h ? t - h : 0;
        }

    private:
        struct alignas(64) cell {
            std::atomic<std::intptr_t> seq;
            T value{};
        };

        alignas(64) std::atomic<std::size_t> head_{0};
        alignas(64) std::atomic<std::size_t> tail_{0};
        std::array<cell, Capacity> cells_;
    };

} // namespace concurrent::detail
