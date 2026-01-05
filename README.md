# ThreadPool

简易的 C++23 线程池，支持可组合的特性掩码。

## 特性

- `op::none` - 基础线程池
- `op::priority` - 任务优先级（low/normal/high）
- `op::cancellable` - 可取消任务

特性可自由组合：`ThreadPool<op::priority | op::cancellable>`

## 用法

```cpp
#include <huxint/thread_pool.hpp>

// 基础用法
ThreadPool<> pool;
auto future = pool.submit([] { return 42; });

// 带优先级
ThreadPool<op::priority> pool;
pool.submit(priority_t::high, [] { /* 高优先级任务 */ });

// 可取消任务
ThreadPool<op::cancellable> pool;
auto task = pool.submit([](token_ref token) {
    while (!token.cancelled()) {
        // 工作...
    }
});
task.cancel();  // 请求取消
```

## 构建

```bash
cmake -B build
cmake --build build
```

## 要求

- C++23
- CMake 3.14+
