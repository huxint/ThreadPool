#pragma once
#include <concepts>
#include <cstddef>

namespace concurrent::detail {

    /**
     * @brief 有界侵入式空闲节点栈
     *
     * 调用方保证串行访问: worker 缓存由所属线程独占, 生产者分片缓存由锁保护.
     * 节点按执行者回收, 而提交者可能在另一线程. 容量上限使满缓存的节点
     * 能流回共享池或分配器, 避免空闲节点随累计任务数无限滞留.
     *
     * @tparam Node     须提供 `Node* next` 成员
     * @tparam Capacity 缓存上限(节点数)
     */
    template <typename Node, std::size_t Capacity>
        requires requires(Node* n) {
            { n->next } -> std::convertible_to<Node*>;
        }
    class node_cache {
    public:
        /// 归还一个节点
        /// @return true = 已收入缓存; false = 已满, 节点仍归调用方处理
        [[nodiscard]]
        bool push(Node* n) noexcept {
            if (size_ >= Capacity) {
                return false;
            }
            n->next = head_;
            head_ = n;
            ++size_;
            return true;
        }

        /// 取一个节点; 空则 nullptr
        [[nodiscard]]
        Node* pop() noexcept {
            Node* h = head_;
            if (h) {
                head_ = h->next;
                --size_;
            }
            return h;
        }

        [[nodiscard]]
        std::size_t size() const noexcept {
            return size_;
        }

    private:
        Node* head_ = nullptr;
        std::size_t size_ = 0;
    };

} // namespace concurrent::detail
