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
