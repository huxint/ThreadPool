# ThreadPool

C++26 高效线程池，基于**工作窃取 + 无锁 MPMC 环**，任务抽象与结果通道采用**函数式风格**。

## 特性

- **工作窃取调度**：每线程分层本地 deque（LIFO）+ Chase-Lev 窃取（FIFO）+ 分层全局 Vyukov 环兜底。
- **零 throw 契约**：提交失败经 `std::expected` 报告；任务体异常被捕获透传至结果通道。
- **函数式组合子**：`task` 支持 `map` / `and_then` / `inspect`，自由函数 `when_all` 汇合多任务。
- **可组合特性标签**：`priority`（分层优先级）、`cancellable`（协作取消）、`trace`（调试钩子）、`worker_cap<N>`（静态 worker 存储）。
- **协作取消**：任务可接收 `std::stop_token`；未开跑即取消的任务会被跳过并标记 `operation_cancelled`。

## 用法

```cpp
#include <concurrent/pool.hpp>

using namespace concurrent;

// 基础用法
pool p({.threads = 4});
auto t = p.submit([](int a, int b) { return a + b; }, 10, 20);
if (t) {
    auto r = t->get();      // std::expected<int, std::exception_ptr>
    if (r) std::println("{}", *r);
}

// 即发即忘（callable 需 noexcept）
(void)p.execute([]{ /* 后台任务 */ });

// 函数式组合
auto a = p.submit([]{ return 100; });
auto b = p.submit([]{ return 200; });
auto sum = when_all(std::move(*a), std::move(*b))
               .map([](auto&& t){ return std::get<0>(t) + std::get<1>(t); });
if (auto r = sum.get()) std::println("sum: {}", *r);

// 带优先级
basic_pool<decltype(priority)> pp({.threads = 2});
pp.execute(task_priority::high, []{ /* 高优先级 */ });

// 协作取消
pool cp({.threads = 2});
auto task = cp.submit([](std::stop_token tok){
    while (!tok.stop_requested()) { /* 工作 */ }
});
task->request_stop(); // 请求取消
```

## 构建

```bash
cmake -B build
cmake --build build
./build/ThreadPool
```

## 要求

- GCC 16.2+（C++26，含契约支持 `-fcontracts`）
- CMake 3.25+
