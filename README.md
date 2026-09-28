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
  <a href="#快速开始">快速开始</a> ·
  <a href="#架构一项任务的旅程">架构</a> ·
  <a href="#把结果接起来">任务组合</a> ·
  <a href="#性能与取舍">性能</a> ·
  <a href="#按需选择特性">特性</a>
</p>

把工作拆成小任务，交给线程池，再把结果接起来。**nexus** 是一个 C++26 任务调度器：用工作窃取分担负载，用 `task<T>` 表达结果，用 `map` 和 `when_all` 描述任务之间的关系。

它关注短任务的调度成本：外部提交进入全局队列，工作线程派生的任务留在本地，空闲线程主动寻找工作。核心库只有头文件，无第三方依赖。

| 写下你要做的事 | nexus 负责的部分 |
| :--- | :--- |
| **提交**一个函数 | 排队、唤醒、执行与节点复用 |
| **组合**几个结果 | 完成通知、结果汇合与错误传递 |
| **遍历**一批数据 | 批量提交、并行处理与按序取回 |

## 快速开始

需要 **GCC 16.2+、CMake 3.28+ 和 Ninja**。项目使用 C++26 与 GCC 的契约实现，编译、链接均需 `-fcontracts`；CMake 目标会自动传递这一选项。

```bash
git clone https://github.com/huxint/nexus.git
cd nexus
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/nexus_example
```

第一个程序：提交两个计算，把结果相加。

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

接入已有 CMake 项目：

```cmake
add_subdirectory(external/nexus)
target_link_libraries(your_app PRIVATE huxint::nexus)
```

也可以直接将 `include/` 加入搜索路径，自行指定 `-std=c++26 -fcontracts -pthread`。

## 架构：一项任务的旅程

<a href="docs/assets/architecture.svg">
  <img src="docs/assets/architecture.svg" alt="nexus 架构：调用方提交经闸门登记，进入全局队列，由工作线程取走执行，再通过共享状态发布结果和运行续延。工作线程也可从其他线程的本地队列窃取任务。" width="1040">
</a>

<p align="center"><sub>点击查看原图 · 支持深浅主题 · <a href="docs/assets/architecture.excalidraw">Excalidraw 可编辑图稿</a></sub></p>

**从提交到执行。** 外部线程先登记任务，再将节点放入全局 MPMC 环。环满后转入自旋锁保护的溢出链，不因队列容量耗尽而拒绝任务。本池工作线程内的嵌套提交优先进入本地 Chase–Lev deque，本地满后才落入全局队列。

**让空闲线程找到工作。** 工作线程依次尝试本地 LIFO、全局批取、随机窃取。暂时无事可做时，先短暂自旋，再进入等待；提交方根据空闲状态唤醒线程。默认自旋预算为 64 µs，可按延迟与 CPU 占用要求调整。

**把结果交回来。** `submit` 的任务体异常进入结果通道，`get()` 返回 `std::expected<T, std::exception_ptr>`。续延直接内联执行，无需再次入队；任务已完成时，挂接续延的调用线程也可能执行它。结果只能领取一次，`get()` 与组合子会争用同一份结果。

> 图中画出的是外部提交的主路径。关闭时拒绝新的外部提交；本池工作线程的嵌套提交仍受有限预算约束。全局溢出链和部分缓存使用锁，因此整个调度器并非处处无锁。

## 把结果接起来

`submit(f, args...)` **直接返回 `task<T>`**，提交失败也折入任务结果，因此可以自然地串联计算。

| 组合子 | 用途 |
| :--- | :--- |
| `map(f)` | 将成功值变换为新值；元组结果支持按元素展开传参 |
| `and_then(f)` | `f` 返回另一个 `task<U>`，将两段异步计算接起来 |
| `inspect(f)` | 观察成功值，继续传递原结果 |
| `when_all(tasks...)` | 将多个任务汇合为一个元组结果 |
| `when_all(vector<task<T>>)` | 将同类型任务汇合为一个结果向量 |

续延适合简短的变换。重计算可以在 `and_then` 中再次 `submit`，让它回到线程池调度。

### 一批输入，按序取回

`parallel_map` 是惰性视图：构造时不提交，首次迭代时整批入队。任务可以乱序完成，结果始终按输入顺序交付。

```cpp
#include <nexus/nexus.hpp>
#include <print>
#include <vector>

int main() {
    using namespace huxint::nexus;
    pool p({.threads = 4});
    std::vector<int> values{1, 2, 3, 4};

    auto squares = parallel_map(p, values, [](int x) { return x * x; });
    for (auto&& result : squares) {
        if (result) {
            std::println("{}", *result); // 1, 4, 9, 16
        }
    }
    if (squares.batch_error()) {
        std::println("批量提交失败");
    }

    // 原地处理；调用返回时全部任务已完成。
    auto status = parallel_for(p, values, [](int& x) { x *= 2; });
    if (!status) {
        std::println("并行处理失败");
    }
}
```

大区间可用 `parallel_map_chunked` / `parallel_for_chunked`：每个任务处理一个子区间，回调接收子区间视图，`grain` 控制分块粒度。底层数据须在任务使用期间保持有效。

### 选择合适的提交方式

| 接口 | 返回值与等待行为 |
| :--- | :--- |
| `submit(f, args...)` | `task<R>`；通过 `get()` 等待并领取结果 |
| `execute(f, args...)` | 通常返回 `submit_status`；即发即忘，回调须 `noexcept` |
| `submit_each(range, f)` | `expected<vector<task<R>>, submit_error>`；按元素提交，摊薄登记与唤醒成本 |
| `fork_join(f, g)` | 将 `f` 入队、在调用线程执行 `g`；返回时不等待 `f`，最终由外部 `p.wait()` 汇合 |

`fork_join` 的 `f` 须 `noexcept`；若提交失败，`g` 不执行。启用 `cancellable` 后，接收 `stop_token` 的 `execute` 重载返回 `expected<stop_source, submit_error>`。

### 错误、取消与收尾

任务结果中的提交错误可由 `submit_error_of(result.error())` 还原，取消可由 `is_cancelled(...)` 识别。普通 `submit` 就支持 `request_stop()`；任务尚未运行时可以跳过任务体，运行中则需要回调接收并轮询 `std::stop_token`。

- `p.wait()` 等待池中任务全部完成；`task::wait()` 只等待该任务，且不取值。
- `p.shutdown(shutdown_policy::drain)` 排空后退出，也是析构的默认行为。
- `p.shutdown(shutdown_policy::discard)` 丢弃尚未执行的任务，等待已经运行的任务收尾。

池级 `wait()` / `shutdown()` 不可在本池任务体内调用。`task::get()` 是阻塞等待，也应避免让全部工作线程同时等待仍在排队的子任务；结果依赖优先用组合子表达。

库将提交失败和任务执行异常放入返回值通道，但不能笼统视为所有调用都 `noexcept`：直接构造线程池可能抛出；提交时用户对象的拷贝 / 移动构造异常可能传播；`fork_join` 内联分支遵循普通调用语义。需要返回值式构造时使用 `pool::try_create()`。

## 性能与取舍

短任务的执行可能很快，调度本身却并不免费。nexus 通过本地队列、批量通知、节点缓存与内联续延，减少每个任务的额外工作。

以下保留项目原 README 的测量记录：**Intel i7-12650H、GCC 16.2、Release，4 次完整运行取中位数**。这些是特定环境下的参考值，本次文档更新未重新测量。

| 场景 | Taskflow | BS::thread_pool | oneTBB | mcq 对比池 | nexus |
| :--- | ---: | ---: | ---: | ---: | ---: |
| 单生产者即发即忘 · M/s | 1.30 | 0.37 | 3.51 | 2.73 | **7.72** |
| 逐个提交并取回 · M/s | 0.35 | 0.23 | — | — | **1.90** |
| 递归 fork-join · M leaves/s | 8.66 | 1.24 | **25.47** | 5.47 | 23.06 |
| 多生产者 ×8 · M/s | 2.41 | 1.32 | 6.66 | 6.55 | **7.05** |
| 空池往返 P50 / P99 · µs | 2.75 / 6.63 | 4.59 / 8.22 | — | — | **0.51 / 1.10** |

吞吐越高越好，延迟越低越好；`—` 表示没有该项对照数据。mcq 对比池基于 moodycamel 队列实现。短任务提交与往返延迟是这组数据中的优势面，纯递归分治则是 oneTBB 更快。

基准包含共享原子计数器的竞争成本，池创建与销毁在计时之外。默认全局环每层 65,536 槽、每槽按 64 B 缓存行填充，仅槽位就约 **4 MiB / 层**；`priority` 开启三层队列。可通过 `queue_cap` 降低常驻内存，但队列更容易落入溢出路径。

```bash
./build/nexus_bench --quick  # 缩减规模，快速体验
./build/nexus_bench          # 完整对照；oneTBB 在系统中可用时加入
```

<details>
<summary><strong>持续性能回归 · CodSpeed</strong></summary>

[CI 工作流](.github/workflows/codspeed.yml)在 `main` 的 push 和 PR 上运行 [CodSpeed 基准](benchmarks/codspeed_bench.cpp)，覆盖提交、派生、批量并行、组合子，以及与其他库的相同负载对照。

simulation 模式衡量模拟环境下的指令成本，线程被串行化，适合追踪调度开销变化，不能代表真实硬件的并行加速比或尾延迟。

```bash
cmake -S . -B build-codspeed -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DBUILD_CODSPEED_BENCH=ON -DCODSPEED_MODE=simulation
cmake --build build-codspeed --target nexus_codspeed_bench -j
./build-codspeed/nexus_codspeed_bench
```

</details>

## 按需选择特性

特性以编译期**值标签**组合，顺序不限：

```cpp
huxint::nexus::basic_pool<
    huxint::nexus::priority,
    huxint::nexus::worker_cap<8>,
    huxint::nexus::queue_cap<8192, 256>
> p({.threads = 4});
```

| 标签 | 带来的能力 |
| :--- | :--- |
| `priority` | high / normal / low 三层队列；优先级为 best-effort |
| `cancellable` | 为即发即忘任务提供协作取消源 |
| `trace` | `on_enqueue` / `on_begin` / `on_end` 生命周期钩子 |
| `worker_cap<N>` | worker 线程句柄容器使用静态容量；不代表整个池无堆分配 |
| `queue_cap<G, L>` | 全局环与本地 deque 容量；均须为至少 2 的二次幂 |

运行期通过 `options` 配置 `threads`（0 表示硬件并发数）、`spin_budget`（默认 64 µs）和 `hooks`。

<details>
<summary><strong>可选集成 · C++ modules 与 stdexec</strong></summary>

模块包装器名为 `huxint.nexus`，使用 Ninja 构建：

```bash
cmake -S . -B build-module -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_MODULE=ON
cmake --build build-module -j
ctest --test-dir build-module --output-on-failure
```

链接 `nexus_module` 后可用 `import huxint.nexus;`。参见[模块示例](module/smoke.cpp)。

[stdexec](https://github.com/NVIDIA/stdexec) 是可选依赖，适配头提供 scheduler：

```cpp
#include <nexus/execution.hpp>

huxint::nexus::pool p({.threads = 4});
auto scheduler = huxint::nexus::ex::as_scheduler(p);
```

集成测试可通过 `-DWITH_STDEXEC=ON -DSTDEXEC_ROOT=<包含 stdexec/ 的目录>` 启用。

</details>

## 继续探索

| 想了解什么 | 从这里开始 |
| :--- | :--- |
| 完整用法：提交、组合、取消、追踪 | [src/main.cpp](src/main.cpp) |
| 调度与队列实现 | [pool.hpp](include/nexus/pool.hpp) · [detail/](include/nexus/detail/) |
| 任务与并行接口 | [task.hpp](include/nexus/task.hpp) · [parallel.hpp](include/nexus/parallel.hpp) |
| 行为验证与性能对照 | [tests/](tests/) · [benchmarks/](benchmarks/) |
| 编辑 README 插图 | [图稿与字体许可](docs/assets/README.md) |

---

<p align="center"><sub>nexus · C++26 · <a href="LICENSE">MIT License</a></sub></p>
