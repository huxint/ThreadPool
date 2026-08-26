# 第三方基线线程池（仅用于性能对照）

| 目录 / 文件 | 来源 | 许可证 | 说明 |
|------|------|--------|------|
| `taskflow_repo/` | https://github.com/taskflow/taskflow @ 83f90a2 | MIT | 工作窃取任务图库，高性能基线；稀疏检出仅含头文件树 |
| `BS_thread_pool.hpp` | https://github.com/bshoshany/thread-pool （v5.1.0） | MIT — © 2021-2026 Barak Shoshany | 学术引用：doi:10.1016/j.softx.2024.101687 |

以上文件按原样保留版权与许可证条款；本仓库主线代码不依赖它们，
仅 `tp_bench` 目标引用以提供同机对比基线。
