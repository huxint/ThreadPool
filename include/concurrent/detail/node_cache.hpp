#pragma once
#include <concepts>
#include <cstddef>

namespace concurrent::detail {

    /**
     * @brief 每 worker 的任务节点缓存: 侵入式空闲栈 + 容量上限
     *
     * **单线程访问**
     * 归还与取用都发生在缓存所属的 worker 自己的线程上 - 节点由谁执行就
     * 回谁的缓存, 提交路径亦只碰调用线程自己那份. 拆除期的排空则排在
     * join 之后. 故本结构无并发访问者, 不需要原子量: 每个任务因此省下
     * 一次入栈 CAS, 一次出栈 CAS 与两次计数 RMW
     *
     * **为什么必须设上限**
     * 外部线程提交时节点只能由 new 取得 - 它不属于任何 worker, 没有本地缓存.
     * 但节点执行完毕后是归还进**执行它的那个 worker** 的缓存, 而生产者永远
     * 不会来取. 于是无上限时缓存长度随**累计**任务数单调增长: 实测 4 worker
     * 单外部生产者, 每百万任务滞留约 128 MiB, 且 wait() 之后一分不还.
     * 峰值在途任务数有界, 保留内存却无界 - 对长期运行的池等同于泄漏
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
        /// @return true = 已收入缓存; false = 已满, 调用方须自行销毁该节点
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
