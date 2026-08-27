#pragma once
#include "concurrent/detail/mpmc_ring.hpp"
#include "concurrent/detail/spinlock.hpp"
#include <atomic>
#include <concepts>
#include <cstddef>
#include <mutex>

namespace concurrent::detail {

    /**
     * @brief 可增长的全局任务队列: Vyukov 有界环(快路径)+ 侵入式溢出链(慢路径)
     *
     * **为什么需要"扩容"**
     * 单纯的有界环在满时只有两条出路: 拒绝, 或让生产者阻塞等空槽(背压).
     * 背压会把队列深度直接摊到提交延迟的尾部(P99 爆炸), 并且在 worker 内嵌套
     * 提交时可能自锁 - 所有 worker 都在等空槽, 就没人消费了. 故此处改为
     * **永不拒绝, 永不阻塞**: 环满即转入溢出链
     *
     * **为什么溢出链不分配内存**
     * 溢出链复用节点自身的 `next_q` 指针串成侵入式单链表. 一个节点在任一时刻
     * 只可能处于"环 / 溢出链 / 空闲链"三者之一, 指针字段互不冲突. 因此
     * 队列扩容的边际内存开销为零 - 节点本来就存在
     *
     * **保序**
     * 严格不变式: **环中元素恒早于溢出链中元素**
     *  - 生产: 溢出链非空时一律追加到溢出链尾, 绝不插队回环
     *  - 消费: 先排空环, 环空才动溢出链; 取一个的同时批量回填一批到环,
     *    使后续 pop 重回无锁快路径. 回填的是溢出链头部(最老的), 不变式得以保持
     *
     * 唯一的乱序窗口来自无锁预检的固有竞争: 生产者读到"溢出链空"的瞬间它刚被填上,
     * 于是该元素落进环, 排到了溢出链元素之前. 窗口仅存在于空与非空的切换点.
     * 本池的顺序本就是 best-effort(本地 deque 是 LIFO, 优先级是分层扫描),
     * 故此处的**强保证是"不丢, 不重"**, 顺序为最大努力
     *
     * **争用**
     * 稳态下溢出链恒空, push/pop 全部落在无锁环上, 自旋锁一次也不会被摸到.
     * 只有积压真的超过 RingCap 时才进入加锁路径, 且临界区被 REFILL_BATCH 限长.
     *
     * @tparam Node    任务节点类型, 须提供 `Node* next_q` 成员
     * @tparam RingCap 环容量(2 的幂)
     */
    template <typename Node, std::size_t RingCap>
        requires requires(Node* n) {
            { n->next_q } -> std::convertible_to<Node*>;
        }
    class global_queue {
        /// 单次回填上限: 把自旋锁的临界区钉死在常数时间内
        static constexpr std::size_t REFILL_BATCH = 64;

    public:
        /**
         * @brief 入队. 永不失败, 永不阻塞(不分配内存 -> 无 OOM 出口)
         * @param n 非空节点; 其 next_q 由本队列接管
         */
        void push(Node* n) noexcept {
            // 快路径: 溢出链空且环有空位
            if (overflow_size_.load(std::memory_order_acquire) == 0) [[likely]] {
                if (ring_.try_push(n)) [[likely]] {
                    return;
                }
            }
            push_overflow(n);
        }

        /// 出队; 空则返回 nullptr
        [[nodiscard]]
        Node* pop() noexcept {
            if (Node* n = ring_.try_pop()) [[likely]] {
                return n;
            }
            // 环空才碰锁: 稳态下这条分支等于"队列真的空了"
            if (overflow_size_.load(std::memory_order_acquire) == 0) [[likely]] {
                return nullptr;
            }
            return pop_overflow_and_refill();
        }

        /// 近似深度(调度启发与观测用, 不作同步依据)
        [[nodiscard]]
        std::size_t size_approx() const noexcept {
            return ring_.size_approx() + overflow_size_.load(std::memory_order_relaxed);
        }

    private:
        void push_overflow(Node* n) noexcept {
            std::scoped_lock g{lock_};
            n->next_q = nullptr;
            if (tail_) {
                tail_->next_q = n;
            } else {
                head_ = n;
            }
            tail_ = n;
            // 必须在解锁前发布: 生产者的无锁预检据此决定是否绕开环
            overflow_size_.fetch_add(1, std::memory_order_release);
        }

        /// 取走溢出链头, 并顺带把接下来的一批搬回环
        /// @return 最老的那个节点; 若竞争中已被别的消费者取空则 nullptr
        [[nodiscard]]
        Node* pop_overflow_and_refill() noexcept {
            std::scoped_lock g{lock_};
            Node* first = head_;
            if (!first) { // 竞争者先到, 链已空
                return nullptr;
            }

            head_ = first->next_q;
            first->next_q = nullptr;
            std::size_t removed = 1;

            // 回填: 让后续 pop 回到无锁环上; 批量上限保证临界区常数时间.
            // 必须先断链再发布: try_push 一旦成功, 节点立即对其他消费者可见,
            // 随时可能被取走执行并回收(缓存满时直接归还分配器), 此后本线程
            // 不得再触碰它; 未发布成功则链指针原样恢复, 链保持完整
            for (std::size_t i = 0; i < REFILL_BATCH && head_; ++i) {
                Node* n = head_;
                Node* next = n->next_q;
                n->next_q = nullptr;
                if (!ring_.try_push(n)) { // 环又满了(生产者正猛灌), 留在链上
                    n->next_q = next;
                    break;
                }
                head_ = next;
                ++removed;
            }
            if (!head_) {
                tail_ = nullptr;
            }

            overflow_size_.fetch_sub(removed, std::memory_order_release);
            return first;
        }

        mpmc_ring<Node*, RingCap> ring_{};

        /// 溢出链的无锁预检计数. 生产者据此保序(非空即绕开环),
        /// 消费者据此避免在稳态下触碰自旋锁
        alignas(64) std::atomic<std::size_t> overflow_size_{0};

        alignas(64) spinlock lock_{};
        Node* head_ = nullptr;
        Node* tail_ = nullptr;
    };

} // namespace concurrent::detail
