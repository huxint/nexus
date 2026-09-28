<p align="center">
  <img src="docs/assets/hero.svg" alt="nexus — 让任务流动，让结果汇合。C++26 工作窃取任务调度器" width="1040">
</p>

<p align="center">
  <a href="https://en.cppreference.com/w/cpp/26"><img src="https://img.shields.io/badge/C%2B%2B-26-1971c2?logo=cplusplus&logoColor=white" alt="C++26"></a>
  <img src="https://img.shields.io/badge/GCC-16.2%2B-099268" alt="GCC 16.2+">
  <img src="https://img.shields.io/badge/header--only-零核心依赖-6741d9" alt="header-only，零核心依赖">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-495057" alt="MIT license"></a>
  <a href="https://app.codspeed.io/huxint/nexus"><img src="https://img.shields.io/endpoint?url=https://codspeed.io/badge.json" alt="CodSpeed"></a>
</p>

<p align="center">
  <a href="#性能">性能</a> ·
  <a href="#接口">接口</a> ·
  <a href="#构建">构建</a> ·
  <a href="#架构">架构</a>
</p>

**nexus**：C++26 header-only 任务调度器，核心零依赖。工作窃取调度、批量提交与函数式任务组合，面向低延迟、高吞吐的短任务。

## 性能

已有测量记录：**Intel i7-12650H、GCC 16.2、Release，4 次完整运行取中位数**。

| 场景 | Taskflow | BS::thread_pool | oneTBB | mcq 对比池 | nexus |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 单生产者即发即忘 · M/s | 1.30 | 0.37 | 3.51 | 2.73 | **7.72** |
| 逐个提交并取回 · M/s | 0.35 | 0.23 | — | — | **1.90** |
| 递归 fork-join · M leaves/s | 8.66 | 1.24 | **25.47** | 5.47 | 23.06 |
| 多生产者 ×8 · M/s | 2.41 | 1.32 | 6.66 | 6.55 | **7.05** |
| 空池往返 P50 / P99 · µs | 2.75 / 6.63 | 4.59 / 8.22 | — | — | **0.51 / 1.10** |

M/s 为百万任务/秒，M leaves/s 为百万叶任务/秒；`—` 表示无对照数据。mcq 池基于 moodycamel 队列。基准包含共享原子计数器竞争，池创建与销毁不计时。

这组数据中，nexus 在短任务提交与往返延迟上领先，纯递归分治则是 oneTBB 更快。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target nexus_bench -j
./build/nexus_bench --quick  # 缩减规模
./build/nexus_bench          # 完整对照；系统安装 oneTBB 后自动加入
```

[CodSpeed](https://app.codspeed.io/huxint/nexus) 在 push / PR 上持续追踪提交、组合与批量并行的调度开销，并与第三方库对照。simulation 模式串行化线程，衡量指令成本，不代表真实并行加速比。配置见 [CI 工作流](.github/workflows/codspeed.yml)。

## 接口

`submit()` 直接返回 `task<T>`；`get()` 返回 `std::expected<T, std::exception_ptr>`，提交失败与任务体异常进入结果通道。结果只能领取一次。

```cpp
#include <nexus/nexus.hpp>
#include <print>

int main() {
    using namespace huxint::nexus;
    pool p({.threads = 4});

    auto sum = when_all(
        p.submit([] { return 100; }),
        p.submit([] { return 200; })
    ).map([](int a, int b) { return a + b; });

    if (auto result = sum.get()) {
        std::println("sum = {}", *result); // sum = 300
    } else {
        std::println("任务未成功完成");
    }
} // 析构前排空任务，再退出工作线程
```

### 提交与并行

| 接口 | 行为 |
| :--- | :--- |
| `submit(f, args...)` | 返回 `task<R>`，通过 `get()` 等待并取值 |
| `execute(f, args...)` | 即发即忘，返回 `submit_status`；回调须 `noexcept` |
| `submit_each(range, f)` | 批量提交，返回 `expected<vector<task<R>>, submit_error>` |
| `fork_join(f, g)` | `f` 入队、`g` 内联执行；`f` 须 `noexcept`，提交失败时不执行 `g`；由外部 `p.wait()` 汇合 |
| `parallel_map(p, range, f)` | 惰性视图，首次迭代整批提交，按输入顺序交付结果；`batch_error()` 检查批量提交错误 |
| `parallel_for(p, range, f)` | 并行遍历，阻塞至完成并返回状态 |
| `parallel_map_chunked` / `parallel_for_chunked` | 按 `grain` 分块，回调接收子区间视图 |

### 任务组合

| 接口 | 行为 |
| :--- | :--- |
| `map(f)` | 变换成功值；元组支持展开传参 |
| `and_then(f)` | 接续返回 `task<U>` 的计算 |
| `inspect(f)` | 观察成功值，保留原结果 |
| `when_all(tasks...)` / `when_all(vector<task<T>>)` | 汇合为元组 / 向量结果 |

续延内联执行，适合短计算；重计算可在 `and_then` 中再次 `submit`。

### 等待与取消

- `p.wait()` 等待全池任务；`task::wait()` 只等待当前任务，不取值。
- `shutdown(shutdown_policy::drain)` 排空退出（析构默认）；`discard` 丢弃未执行任务，等待运行中的任务结束。
- `task::request_stop()` 请求取消；运行中需回调接收并轮询 `std::stop_token`。错误可用 `submit_error_of()` / `is_cancelled()` 识别。

池级 `wait()` / `shutdown()` 不可在本池任务体内调用；避免所有工作线程阻塞等待排队中的子任务，优先用组合子表达依赖。并行接口使用的底层数据须在任务完成前保持有效。

### 配置

```cpp
huxint::nexus::basic_pool<
    huxint::nexus::priority,
    huxint::nexus::queue_cap<8192, 256>
> p({.threads = 4});
```

| 标签 | 能力 |
| :--- | :--- |
| `priority` | high / normal / low 三层队列，优先级为 best-effort |
| `cancellable` | 接收 `stop_token` 的 `execute` 返回 `expected<stop_source, submit_error>` |
| `trace` | `on_enqueue` / `on_begin` / `on_end` 钩子 |
| `worker_cap<N>` | 工作线程句柄容器使用静态容量 |
| `queue_cap<G, L>` | 全局 / 本地容量，默认 65536 / 256，须为至少 2 的二次幂 |

运行期选项：`threads`（0 为硬件并发数）、`spin_budget`（默认 64 µs）、`hooks`。构造线程池可能抛出；需要返回值式构造时使用 `pool::try_create()`。

## 构建

需要 **GCC 16.2+、CMake 3.28+、Ninja**。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/nexus_example
```

接入已有项目：

```cmake
add_subdirectory(external/nexus)
target_link_libraries(your_app PRIVATE huxint::nexus)
```

或直接添加 `include/`，指定 `-std=c++26 -fcontracts -pthread`。

## 架构

<a href="docs/assets/architecture.svg">
  <img src="docs/assets/architecture.svg" alt="nexus 架构：提交、全局与本地队列、工作窃取、执行与结果汇合" width="1040">
</a>

<p align="center"><sub>支持深浅主题 · <a href="docs/assets/architecture.excalidraw">Excalidraw 图稿</a></sub></p>
