#pragma once
// P2300 (stdexec) 暴露面: 池作为 scheduler. 可选依赖 - 本头不进
// concurrent.hpp 的无条件包含链, 核心库保持零依赖 header-only;
// 消费方须自行提供 stdexec (2024.12.08 验证) 并显式包含本头
//
// 与零 throw 组合子(task::map / when_all)的取舍: stdexec 面获得标准
// sender/receiver 算法组合(then / when_all / split / bulk ...), 代价是
// 依赖与 sync_wait 的重抛语义(本库组合子经 expected 报错, 不抛)
#include "concurrent/pool.hpp"
#include <stdexec/execution.hpp>
#include <exception>
#include <memory>
#include <type_traits>
#include <utility>

namespace concurrent::ex {

    template <typename Pool>
    class pool_scheduler;

    namespace exd {

        /// connect 产物: start() 把完成工作投递到池. op state 由消费者按
        /// sender/receiver 协议持有至完成信号发出, 闭包捕获 this 依赖此约定
        template <typename Pool, typename Receiver>
        class schedule_op {
        public:
            schedule_op(Pool* p, Receiver&& rcvr) noexcept(
                std::is_nothrow_move_constructible_v<Receiver>)
                : pool_(p), rcvr_(std::move(rcvr)) {}

            void start() & noexcept {
                auto* self = this;
                // execute 要求闭包 noexcept: receiver 的 set_value 允许抛出
                // (P2300 语义), 就地转入错误通道 - 与本库零 throw 契约的边界
                if (!pool_->execute([self]() noexcept {
                        try {
                            stdexec::set_value(std::move(self->rcvr_));
                        } catch (...) {
                            stdexec::set_error(std::move(self->rcvr_),
                                               std::current_exception());
                        }
                    })) {
                    // 池已关闭(OOM 同此): 提交被拒不等于完成 - 报停止
                    stdexec::set_stopped(std::move(rcvr_));
                }
            }

        private:
            Pool* pool_;
            Receiver rcvr_;
        };

        /// schedule 的产物: 完成签名确定的环境确定性 sender
        template <typename Pool>
        class schedule_sender {
        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(),
                stdexec::set_error_t(std::exception_ptr),
                stdexec::set_stopped_t()>;

            explicit schedule_sender(Pool& p) noexcept : pool_(std::addressof(p)) {}

            template <typename Receiver>
            friend auto tag_invoke(stdexec::connect_t, const schedule_sender& self,
                                   Receiver&& rcvr) {
                return schedule_op<Pool, std::decay_t<Receiver>>{self.pool_,
                                                                 std::forward<Receiver>(rcvr)};
            }

            /// 环境暴露完成调度器: when_all / continuation 等算法据此转发
            friend auto tag_invoke(stdexec::get_env_t, const schedule_sender& self) noexcept {
                return stdexec::env{stdexec::prop{
                    stdexec::get_completion_scheduler<stdexec::set_value_t>,
                    pool_scheduler<Pool>{*self.pool_}}};
            }

        private:
            Pool* pool_;
        };

    } // namespace exd

    /**
     * @brief 池的 scheduler 视图: schedule() 在池上投递一个完成信号
     *
     * 轻量值类型(单指针), 拷贝廉价; 相等比较按池身份
     *
     * stdexec 2024.12 的 scheduler / operation_state 为结构化概念,
     * 仅 sender 需要 sender_concept 别名
     */
    template <typename Pool>
    class pool_scheduler {
    public:
        explicit pool_scheduler(Pool& p) noexcept : pool_(std::addressof(p)) {}

        [[nodiscard]]
        exd::schedule_sender<Pool> schedule() const noexcept {
            return exd::schedule_sender<Pool>{*pool_};
        }

        [[nodiscard]]
        friend bool operator==(const pool_scheduler& a, const pool_scheduler& b) noexcept {
            return a.pool_ == b.pool_;
        }

    private:
        Pool* pool_;
    };

    /// 池 -> scheduler 的适配入口(独立自由函数, 避免 pool.hpp 反向依赖)
    template <typename Pool>
    [[nodiscard]]
    pool_scheduler<Pool> as_scheduler(Pool& p) noexcept {
        return pool_scheduler<Pool>{p};
    }

} // namespace concurrent::ex
