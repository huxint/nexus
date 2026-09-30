# 配置与运行

[项目首页](../README.md) · [接口与任务模型](api.md)

## 构建与接入

需要 **GCC 16.2+、CMake 3.28+、Ninja**。核心库仅含头文件，无第三方运行时依赖。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/nexus_example
```

接入已有 CMake 项目：

```cmake
add_subdirectory(external/nexus)
target_link_libraries(your_app PRIVATE huxint::nexus)
```

直接使用头文件时，添加 `include/` 并指定 `-std=c++26 -fcontracts -pthread`。

### 可选构建项

将所需选项加在 CMake 配置命令后即可，无需修改核心库。

| 选项 | 用途 |
| :--- | :--- |
| `-DCMAKE_BUILD_TYPE=Debug` | 调试构建，启用契约检查 |
| `-DSANITIZER=address` / `thread` | 启用 ASan 或 TSan，均同时启用 UBSan |
| `-DBUILD_MODULE=ON` | 构建 `huxint.nexus` 模块及冒烟测试 |
| `-DWITH_STDEXEC=ON -DSTDEXEC_ROOT=<include-root>` | 构建 stdexec 集成测试；路径下须包含 `stdexec/` |
| `-DBUILD_CODSPEED_BENCH=ON` | 拉取并构建 CodSpeed 基准依赖 |

模块模式使用 `import huxint.nexus;`；使用方所需的标准库头应置于 `import` 之前。
参见 [模块示例](../module/smoke.cpp)。

## 线程池配置

```cpp
huxint::nexus::basic_pool<huxint::nexus::priority> p({.threads = 4});
```

普通用途使用 `pool` 即可。运行期选项为 `threads`（0 表示硬件并发数）、
`spin_budget`（默认 64 µs）和 `hooks`。构造可能抛出；需要返回值式构造时使用 `pool::try_create()`。

| `basic_pool` 标签 | 能力 |
| :--- | :--- |
| `priority` | high / normal / low 三层优先级，按尽力而为的方式调度 |
| `cancellable` | 接收 `stop_token` 的 `execute` 返回 `expected<stop_source, submit_error>` |
| `trace` | `on_enqueue` / `on_begin` / `on_end` 钩子 |
| `worker_cap<N>` | 工作线程句柄容器使用静态容量 |
| `queue_cap<G, L>` | 全局 / 本地容量，默认 65536 / 256，须为至少 2 的二次幂 |

## 运行基准

默认构建包含以下程序，运行时避免同时编译或执行其他基准：

| 程序 | 用途 |
| :--- | :--- |
| `./build/nexus_bench` | 与 Taskflow、BS::thread_pool、moodycamel 对比池进行完整对照 |
| `./build/nexus_parallel_bench` | 扫描输入规模、线程数与负载分布，输出 wall/CPU 时间及单独采集的任务数 |
| `./build/nexus_allocation_bench` | 统计普通 `operator new` 的调用次数和累计请求字节数，不是峰值内存 |

前两个程序支持 `--quick` 缩短扫描。粒度基准还支持 `--tuned`，单独测量显式 `grain=256`。
安装 oneTBB 后，配置阶段会自动将其加入第三方对照。

[CodSpeed](https://app.codspeed.io/huxint/nexus) 在 push / PR 上持续追踪基准。
`CODSPEED_MODE` 可选 `simulation`、`walltime`、`memory`；simulation 串行化线程，
衡量指令成本，不代表实际并行加速比。配置见 [CI 工作流](../.github/workflows/codspeed.yml)。

## 基准数据与测量口径

2026-09-30，Intel i7-12650H、GCC 16.2.1、Release、8 个 worker。
[首页主基准](../README.md#性能) 的吞吐与耗时取三次运行中的最佳值，延迟报告采样分位数。
负载包含共享原子计数器竞争，池创建与销毁不计时。
固定计算负载、4 线程时，nexus 为 3.26 ms，Taskflow 为 2.59 ms，算力型负载不总是领先。

同一轮完整运行中的扩展对照独立采样；mcq 池基于 moodycamel 队列：

| 场景 | oneTBB | mcq 对比池 | nexus |
| :--- | ---: | ---: | ---: |
| 单生产者即发即忘 · 百万任务/s | 3.65 | 2.62 | **8.40** |
| 递归 fork-join · 百万叶任务/s | 25.69 | 5.90 | **27.39** |
| 多生产者 ×8 · 百万任务/s | 6.53 | 7.18 | **7.31** |

默认 `parallel_for` 的优化前后对照使用 [同一份源码](../benchmarks/parallel_bench.cpp)，
旧版为提交 `464e331`。同机、8 个 worker、262,144 个元素；新旧版本交替运行三组，
每组预热后测量七次，取各组中位数的中位数，业务负载无共享写计数器。

| 负载 | 修改前 / ms | 修改后 / ms | 加速 |
| :--- | ---: | ---: | ---: |
| 均匀 | 99.62 | 1.92 | 51.9× |
| 末尾重计算 | 101.32 | 1.86 | 54.5× |
| 开头重计算 | 103.18 | 1.83 | 56.5× |

调度任务从 262,144 个降到 8 个，元素回调次数不变。已手动调优的显式分块基本持平。
已完成任务连续 `map` 10,000 次，普通 `operator new` 调用从 20,000 次降到 10,000 次。
数据受负载和机器状态影响；上述倍数针对原来的逐元素调度路径。
