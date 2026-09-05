# ThreadPool

![C++](https://img.shields.io/badge/C%2B%2B-26-00599C?logo=cplusplus&logoColor=white)
![GCC](https://img.shields.io/badge/GCC-16.2%2B-blue)
![CMake](https://img.shields.io/badge/CMake-3.28%2B-064F8C?logo=cmake&logoColor=white)
![header-only](https://img.shields.io/badge/layout-header--only-purple)
![tests](https://img.shields.io/badge/tests-doctest-green)
![license](https://img.shields.io/badge/license-MIT-success)

C++26 高性能线程池: 工作窃取调度 + 无锁队列 + 函数式任务组合, 全库 API 零异常

## 为什么选它

零依赖的 header-only 线程池, 面向两类场景: 追求低延迟与高吞吐的任务流(唤醒路径有界自旋, 空池往返亚微秒级), 以及需要错误安全的服务型代码(库自身失败走 `std::expected`, 任务体异常透传至结果通道, 不泄漏到池外). 任务以 128B 节点 + SBO 闭包承载, 外部提交进无锁全局队列, worker 内嵌套提交进本地 deque, 队列满时使用溢出链, 提交不等待空槽.

| 特性 | 说明 |
|------|------|
| 工作窃取调度 | 每线程本地 deque(LIFO) + Chase-Lev 窃取(FIFO); 全局侧 Vyukov MPMC 环 + 保序溢出链兜底 |
| work-first 派生 | `fork_join(f, g)` 递归分治惯用形态: 每层只付一次提交通道, 内联分支沿深度优先保有缓存局部性, 递归树随 `wait()` 汇合 |
| 零 throw 契约 | 库自身的失败经 `std::expected` 报告; 任务体异常透传至结果通道; `execute` 编译期强制 `noexcept`. 仅 `submit`/`submit_each` 提交期的用户代码(callable 与实参的拷贝构造)异常原样透传 |
| 函数式组合子 | `task` 支持 `map` / `and_then` / `inspect`, `when_all` 汇合多任务为 `task<tuple<...>>`; 续延链深度守卫, 万级链不爆栈 |
| 惰性批量 | `parallel_map` / `parallel_for` 返回轻量视图, 首次迭代整批入队, 按输入顺序取回 `expected`; 迭代面经 `std::generator`, `results()` 可直接组合 ranges 管道 |
| 分块批量 | `parallel_map_chunked` / `parallel_for_chunked` 每块一任务, 摊薄元素级调度开销, 缓解大区间整批提交的内存尖峰 |
| P2300 scheduler | 可选对接 [stdexec](https://github.com/NVIDIA/stdexec): `ex::as_scheduler(pool)` 暴露标准 sender/receiver 算法组合(构建开关, 核心库零依赖) |
| 可组合特性标签 | `priority`, `cancellable`, `trace`, `worker_cap<N>`, `queue_cap<Global, Local>` 变参无序组合, 编译期开关零抽象税 |
| 协作取消 | 任务可接收 `std::stop_token`; 未开跑即取消的任务体被跳过并以 `operation_cancelled` 标记 |

## 快速开始

要求: GCC 16.2+, CMake 3.28+

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

ctest --test-dir build          # 测试
./build/concurrent_example      # 示例
./build/concurrent_bench        # 基准(--quick 缩减规模)
```

库本身零依赖 header-only: 消费方 `add_subdirectory` 后 `target_link_libraries(app PRIVATE concurrent)` 即可, 或直接把 `include/` 加入头文件搜索路径并链接 Threads, 且 GCC 下须以 `-fcontracts` 编译链接(公共头含契约语法, 缺该 flag 时链接期缺 `handle_contract_violation`)

```cpp
#include <concurrent/pool.hpp>
#include <print>

using namespace concurrent;

pool p({.threads = 4});

// 即发即忘, callable 必须 noexcept
static_cast<void>(p.execute([]() noexcept { /* ... */ }));

// 有返回值: submit 返回 expected<task<T>, submit_error>; 结果恰好可取一次
auto t = p.submit([](int a, int b) { return a + b; }, 10, 20);
if (t) {
    if (auto r = t->get()) {
        std::println("{}", *r);
    }
}

// 函数式组合: when_all 汇合多任务, map 变换结果
auto a = p.submit([] { return 100; });
auto b = p.submit([] { return 200; });
if (a && b) {
    auto sum = when_all(std::move(*a), std::move(*b))
                   .map([](auto&& tup) { return std::get<0>(tup) + std::get<1>(tup); });
    if (auto r = sum.get()) {
        std::println("sum = {}", *r);
    }
}
p.wait();
```

## 用法

提交接口(`R` 为 callable 返回类型):

| 接口 | 约束 | 返回 |
|------|------|------|
| `submit(f, args...)` | - | `expected<task<R>, submit_error>` |
| `submit(prio, f, ...)` | 需 `priority` 标签 | 同上 |
| `execute(f, args...)` | `f` 必须 `noexcept` | `expected<void, submit_error>` |
| `execute(prio, f, ...)` | 同上 + `priority` 标签 | 同上 |
| `execute(f, ...)` 带 stop_token 形参 | `cancellable` 标签 | `expected<stop_source, submit_error>` |
| `submit_each(range, f)` | 区间每元素一任务, 整批单次唤醒; 元素按值拷贝 | `expected<vector<task<R>>, submit_error>` |
| `fork_join(f, g)` | work-first 派生: `f` 入队供窃取, `g` 由调用线程内联执行; `f` 须 `noexcept`, `g` 异常按直接调用传播; worker 内调用安全, 以池级 `wait()` 汇合 | `expected<void, submit_error>` |

`submit` 的 callable 接受 `const std::stop_token&` 首参即获得协作取消能力, 返回的 `task` 携带 `request_stop()`

task 组合子(均在完成任务的工作线程上内联执行; 结果值恰好可领取一次, 不论经 `get` 还是续延, 迟到者得到 `invalid_task`):

| 组合子 | 行为 |
|--------|------|
| `map(f)` | 变换成功值, `f` 接收 `T&&`(void 任务无参), 错误/取消透传 |
| `and_then(f)` | 绑定返回 `task<U>` 的后续操作, 内层结果透传 |
| `inspect(f)` | 旁观副作用, 不改变结果与错误通道 |
| `when_all(tasks...)` | 全部成功 -> `task<tuple<...>>`; 任一失败/取消 -> 以首个错误失败 |

惰性批量与生命周期:

| 接口 | 行为 |
|------|------|
| `parallel_map(p, range, f)` | 惰性视图: 不迭代则不提交; `begin()` 整批入队, 按输入顺序阻塞取回 `expected`; 析构等待全部完成 |
| `parallel_map_chunked(p, range, f, grain)` | 分块版: 每 grain 个元素一块(0 = 按线程数), `f` 对每块调用一次并接收子区间; 大区间优先用此入口 |
| `parallel_for(p, range, f).run()` | 同上, 无返回值版; `.run()` 直接执行并返回首个错误 |
| `parallel_view::results()` | 结果流: 单趟 `std::generator`, 可直接组合 ranges 管道(如 `v.results() \| std::views::take(3)`) |
| `p.wait()` / `wait_for` / `wait_until` | 阻塞至任务体、续延与 callable 析构全部完成 / 超时变体. 不得在任务体(worker 线程)内调用 `wait`/`shutdown` - 必死锁, Debug 构建下契约断言终止 |
| `p.shutdown(policy)` | `drain` 排空后退出(析构默认); `discard` 尽力丢弃排队任务并以取消语义终结其结果通道(worker 已取出未开跑的部分可能照常执行); 运行中任务两者都会等待完成(join 固有语义); 两者均放行 worker 内嵌套提交(fork-join 析构安全) |

错误辨识: `is_cancelled(e)` 判取消, `is_invalid_task(e)` 判无效任务句柄, `submit_error_of(e)` 还原提交阶段失败

## 特性标签

变参无序组合, 例如 `basic_pool<decltype(priority), decltype(trace)>`:

| 标签 | 效果 |
|------|------|
| `priority` | high/normal/low 三档分层队列, 消费时高到低扫描(best-effort) |
| `cancellable` | 解锁返回 `stop_source` 的 execute 重载 |
| `trace` | 运行期钩子 `on_enqueue` / `on_begin` / `on_end`(签名强制 `noexcept`), 事件含 id/phase/outcome/priority/worker |
| `worker_cap<N>` | workers 以 `inplace_vector<jthread, N>` 静态存储, 无标签用 `vector` |
| `queue_cap<Global, Local>` | 全局环 / 本地 deque 容量(2 的幂且至少为 2, 缺省 65536 / 256). 环满自动落入保序溢出链, 容量影响内存占用与无锁快路径占比; 大环吸收多生产者积压避免溢出链争用(每槽按缓存行填充, 65536 槽 = 4 MiB/层, 内存敏感时可下调) |

运行期配置 `pool::options`:

| 字段 | 说明 |
|------|------|
| `threads` | worker 数, 0 = `hardware_concurrency()` |
| `spin_budget` | 睡前有界自旋时间预算(默认 64µs, 0 = 不自旋直接睡). 任务到达间隔稳定小于该值时 N 个 worker 全程占核, 稀疏流量的服务型池宜调小 |
| `hooks` | trace 钩子(仅 `trace` 标签下生效) |

## stdexec(P2300)可选面

核心库零依赖; 需要 sender/receiver 生态(`then` / `when_all` / `split` / 标准算法)时:

```cpp
#include <concurrent/execution.hpp>   // 提供 stdexec 的 TU 须先自行引入该库

concurrent::pool p({.threads = 4});
auto sched = concurrent::ex::as_scheduler(p);

using namespace stdexec;
auto [v] = sync_wait(sched.schedule() | then([] { return 42; })).value();
```

池已关闭时 `schedule` 以 `set_stopped` 完成; 续延异常经 `set_error` 送达(`sync_wait` 按 P2300 约定重抛, 与本库组合子的 `expected` 通道不同). 组合子(`map` / `and_then` / `when_all`)是零依赖路径的唯一选择, 两套面并存各取所长

## 性能

基准对比 [Taskflow](https://github.com/taskflow/taskflow)、[BS::thread_pool](https://github.com/bshoshany/thread-pool)、可选的 [oneTBB](https://github.com/oneapi-src/oneTBB), 以及基于 [moodycamel](https://github.com/cameron314/concurrentqueue) 队列的对比池:

```bash
./build/concurrent_bench --quick
./build/concurrent_bench
```

程序输出单生产者与多生产者吞吐、提交并取回结果的往返延迟、递归 fork-join、混合负载、线程数扩展性和分块并行映射. 完整运行的吞吐与耗时项目预热后测量 3 次并取最短耗时, 延迟统计 30,000 次连续往返的分位数; `--quick` 缩减任务数与延迟样本, 吞吐与耗时仅测量一次. 池的创建和销毁在计时之外, 吞吐测试复用已创建的生产者线程.

比较时需结合负载理解结果:

- 递归统一采用一支入队、一支内联, 以完成的叶子数计吞吐.
- 吞吐任务会更新共享原子完成计数器, 测得的成本包含该计数器的竞争.
- 延迟来自连续的 `submit` / `get`, 主要反映 worker 自旋时的响应. 稀疏请求需另外测量 worker 入睡后的唤醒延迟与 CPU 占用.
- oneTBB 的 enqueue 场景不预留主线程槽位; 递归和 `parallel_for` 场景允许调用线程进入 arena.
- moodycamel 对比池使用 `std::function` 与 `std::counting_semaphore`, 结果包含池的通知与任务包装成本.

默认 `spin_budget` 为 64µs, 适合成簇到达的短任务. 持续低于这一间隔的请求会让 worker 保持忙等; 服务型负载应结合 CPU 占用调整预算. 细粒度区间可使用 `parallel_map_chunked` / `parallel_for_chunked` 并选择合适的 grain. 下调 `queue_cap` 可以节省内存, 也会更早进入溢出链, 应按实际积压量测量.

本库将全局取任务和跨线程节点回收按小批次处理, 余下任务放入仍可被窃取的本地队列. 任务完成由单调提交计数与完成计数核对, worker 与外部生产者使用独立的记账分片.

## 构建与测试

| 选项 | 说明 |
|------|------|
| `-DBUILD_MODULE=ON` | 模块封装 `concurrent.pool`(需 Ninja) |
| `-DSANITIZER=address\|thread` | 叠加 UBSan 的消毒器构建 |
| `-DWITH_STDEXEC=ON -DSTDEXEC_ROOT=<path>` | P2300 scheduler 集成测试(独立目标; path 为含 `stdexec/` 的包含根) |

契约(Contracts)在 Debug 下 enforce, 其余配置 ignore(零开销). 配置即生成 `compile_commands.json` 并软链到仓库根目录

模块封装: `-DBUILD_MODULE=ON` 构建后即可 `import concurrent.pool;`. 注意标准库文本包含须置于 `import` 之前(GCC 16 工具链限制), 完整示例见 `module/smoke.cpp`

测试基于 [doctest](https://github.com/doctest/doctest)(单头, 零依赖), 覆盖提交语义, 优先级, 取消, 异常通道, 生命周期, 组合子, 惰性批量与无锁容器并发回归:

| 构建 | 结果 |
|------|------|
| Release(契约 ignore) | 通过, 零警告 |
| Debug(契约 enforce) | 通过, 零警告 |
| `-DSANITIZER=address` | 无报告 |
| `-DSANITIZER=thread` | 无库代码报告 |

TSan 抑制清单 `tests/tsan.supp` 由 CTest 自动挂载, 仅滤除 libubsan/libstdc++ 运行时自扰, 库代码零抑制

## 协议

[MIT](LICENSE). 欢迎提交 issue 与 PR
