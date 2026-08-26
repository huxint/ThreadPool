#pragma once
#include "tags.hpp"
#include <cstdint>
#include <functional>

namespace concurrent {

    /// trace_event 的阶段
    enum class task_phase : std::uint8_t {
        enqueue, ///< 任务成功入队(提交线程上触发)
        begin,   ///< worker 即将执行任务体
        end,     ///< 任务结束(含失败与取消跳过)
    };

    /// 任务的最终结局, 仅 end 阶段有意义
    enum class task_outcome : std::uint8_t {
        completed, ///< 正常完成
        failed,    ///< 任务体抛出异常, 异常已捕获进错误通道
        cancelled, ///< 排队中即被取消, 任务体未执行
    };

    /// 调试事件的纯值视图. 时间戳由钩子自行测量, 用 id 做跨阶段配对
    struct trace_event {
        std::uint64_t id{};
        task_phase phase{};
        task_outcome outcome{task_outcome::completed};
        task_priority priority{task_priority::normal}; ///< 未启用 priority 标签时恒为 normal
        std::size_t worker{};                          ///< worker 索引; enqueue 阶段无意义
    };

    /// 调试钩子槽位. 签名中的 noexcept 使"钩子抛异常"直接编译失败,
    /// 与库的零 throw 契约自洽. 用户义务: 线程安全, 执行迅速
    struct trace_hooks {
        std::move_only_function<void(trace_event) noexcept> on_enqueue;
        std::move_only_function<void(trace_event) noexcept> on_begin;
        std::move_only_function<void(trace_event) noexcept> on_end;
    };

} // namespace concurrent
