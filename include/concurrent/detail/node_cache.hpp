#pragma once
#include <atomic>
#include <concepts>
#include <cstddef>

namespace concurrent::detail {

    /**
     * @brief 每 worker 的任务节点缓存: MPSC Treiber 栈 + 容量上限
     *
     * 压栈来自任意线程(谁执行完谁归还), 弹出仅限所有者 -> 单一消费者, 无 ABA
     *
     * **为什么必须设上限**
     * 外部线程提交时节点只能由 new 取得 - 它不属于任何 worker, 没有本地缓存.
     * 但节点执行完毕后是归还进**执行它的那个 worker** 的缓存, 而生产者永远
     * 不会来取. 于是无上限时缓存长度随**累计**任务数单调增长: 实测 4 worker
     * 单外部生产者, 每百万任务滞留约 128 MiB, 且 wait() 之后一分不还.
     * 峰值在途任务数有界, 保留内存却无界 - 对长期运行的池等同于泄漏
     *
     * **计数是近似值**
     * 并发压栈可能同时越过上限判定, 超额至多为并发压栈方个数. 判定偏保守
     * 无害: 宁可多还给分配器, 不可滞留. 弹出侧单线程, 故计数不会下溢
     *
     * @tparam Node     须提供 `Node* next_free` 成员
     * @tparam Capacity 缓存上限(节点数)
     */
    template <typename Node, std::size_t Capacity>
        requires requires(Node* n) {
            { n->next_free } -> std::convertible_to<Node*>;
        }
    class node_cache {
    public:
        static constexpr std::size_t capacity = Capacity;

        /// 归还一个节点(任意线程)
        /// @return true = 已收入缓存; false = 已满, 调用方须自行销毁该节点
        [[nodiscard]]
        bool push(Node* n) noexcept {
            if (size_.load(std::memory_order_relaxed) >= Capacity) {
                return false;
            }
            size_.fetch_add(1, std::memory_order_relaxed);
            Node* h = head_.load(std::memory_order_relaxed);
            do {
                n->next_free = h;
            } while (!head_.compare_exchange_weak(h, n, std::memory_order_release,
                                                  std::memory_order_relaxed));
            return true;
        }

        /// 取一个节点; 空则 nullptr. 仅所有者线程调用
        [[nodiscard]]
        Node* pop() noexcept {
            Node* h = head_.load(std::memory_order_acquire);
            while (h) {
                if (head_.compare_exchange_weak(h, h->next_free, std::memory_order_acquire,
                                                std::memory_order_acquire)) {
                    size_.fetch_sub(1, std::memory_order_relaxed);
                    return h;
                }
            }
            return nullptr;
        }

        /// 近似长度(观测与测试用, 不作同步依据)
        [[nodiscard]]
        std::size_t size_approx() const noexcept {
            return size_.load(std::memory_order_relaxed);
        }

    private:
        // 二者被同一次 push/pop 一并触碰, 故刻意同居一行
        std::atomic<Node*> head_{nullptr};
        std::atomic<std::size_t> size_{0};
    };

} // namespace concurrent::detail
