module;
// 命名模块封装：实现仍是 header-only（本单元仅做接口再导出）。
// 使用方既可 #include <concurrent/concurrent.hpp>，也可 import concurrent.pool;
//
// 混用须知：模板体在使用方翻译单元实例化，其标准库依赖必须在该单元可见，
// 且文本包含必须置于 import 语句之前（先解析后合并，避免重定义冲突）：
//
//     #include <vector>            // 先
//     import concurrent.pool;      // 后
#include <concurrent/concurrent.hpp>

export module concurrent.pool;

export namespace concurrent {
    // ---- 池 ----
    using concurrent::basic_pool;
    using concurrent::pool;
    using concurrent::shutdown_policy;

    // ---- 任务与错误通道 ----
    using concurrent::invalid_task_error;
    using concurrent::is_cancelled;
    using concurrent::operation_cancelled;
    using concurrent::submit_error;
    using concurrent::submit_error_of;
    using concurrent::task;

    // ---- 组合子 ----
    using concurrent::when_all;

    // ---- 惰性批量 ----
    using concurrent::parallel_for;
    using concurrent::parallel_map;

    // ---- 特性标签 ----
    using concurrent::cancellable;
    using concurrent::priority;
    using concurrent::task_priority;
    using concurrent::trace;
    using concurrent::worker_cap;

    // ---- 调试钩子 ----
    using concurrent::task_outcome;
    using concurrent::task_phase;
    using concurrent::trace_event;
    using concurrent::trace_hooks;
} // namespace concurrent
