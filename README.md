# nexus

![C++](https://img.shields.io/badge/C%2B%2B-26-00599C?logo=cplusplus&logoColor=white)
![GCC](https://img.shields.io/badge/GCC-16.2%2B-blue)
![CMake](https://img.shields.io/badge/CMake-3.28%2B-064F8C?logo=cmake&logoColor=white)
![header-only](https://img.shields.io/badge/layout-header--only-purple)
![license](https://img.shields.io/badge/license-MIT-success)

C++26 高性能任务调度器: 工作窃取调度 + 无锁队列 + 函数式任务组合, 全库 API 零异常. 适合低延迟提交、高吞吐短任务流与错误安全并发的场景

选型约束: 依赖 C++26 与 GCC 契约实现(`-fcontracts`), GCC 16.2+ / CMake 3.28+

## 快速开始

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build             # 测试
./build/nexus_example              # 示例
```

零依赖 header-only: `add_subdirectory` 后 `target_link_libraries(app PRIVATE huxint::nexus)`, 或把 `include/` 加入头文件搜索路径并自行链接线程库(`-pthread` / CMake 的 `Threads::Threads`)

```cpp
#include <nexus/pool.hpp>
#include <print>

using namespace huxint::nexus;

pool p({.threads = 4});

// 即发即忘, callable 必须 noexcept
static_cast<void>(p.execute([]() noexcept { std::println("hi"); }));

// submit 返回 expected<task<T>, submit_error>, 结果恰好可取一次
auto t = p.submit([](int a, int b) { return a + b; }, 10, 20);
if (t && auto r = t->get()) {
    std::println("{}", *r);
}

// 函数式组合: when_all 汇合, map 变换
auto a = p.submit([] { return 100; });
auto b = p.submit([] { return 200; });
if (a && b) {
    auto sum = when_all(std::move(*a), std::move(*b))
                   .map([](auto&& tup) { return std::get<0>(tup) + std::get<1>(tup); });
    std::println("sum = {}", sum.get().value_or(0));
}
p.wait();
```

## 性能

基准对比 [Taskflow](https://github.com/taskflow/taskflow)、[BS::thread_pool](https://github.com/bshoshany/thread-pool)、可选的 [oneTBB](https://github.com/oneapi-src/oneTBB), 以及基于 [moodycamel](https://github.com/cameron314/concurrentqueue) 队列的对比池:`./build/nexus_bench`(`--quick` 缩减规模)

同环境 4 次完整运行取中位数(i7-12650H, GCC 16.2, Release):

| 场景 | Taskflow | BS | oneTBB | mcq 池 | nexus |
|------|---------:|---:|-------:|-------:|------:|
| 单生产者即发即忘(M/s) | 1.30 | 0.37 | 3.51 | 2.73 | **7.72** |
| 逐个提交并取回(M/s) | 0.35 | 0.23 | - | - | **1.90** |
| 递归 fork-join(M leaves/s) | 8.66 | 1.24 | **25.47** | 5.47 | 23.06 |
| 多生产者 ×8(M/s) | 2.41 | 1.32 | 6.66 | 6.55 | **7.05** |
| 空池往返 P50 / P99(µs) | 2.75 / 6.63 | 4.59 / 8.22 | - | - | **0.51** / **1.10** |

短任务提交与往返延迟是优势面; 纯递归分治 oneTBB 略快. 基准含共享原子计数器的竞争成本, 池创建销毁在计时之外

## 用法

提交接口:

| 接口 | 说明 |
|------|------|
| `submit(f, args...)` | 返回 `expected<task<R>, submit_error>` |
| `execute(f, args...)` | `f` 须 `noexcept`, 无结果通道 |
| `submit_each(range, f)` | 每元素一任务, 整批单次唤醒 |
| `fork_join(f, g)` | work-first 派生: `f` 入队供窃取, `g` 内联执行 |

`task` 组合子(结果恰好可领取一次): `map` / `and_then` / `inspect`, `when_all(tasks...)` 汇合为 `task<tuple<...>>`

批量: `parallel_map(p, range, f)` 惰性视图, 首次迭代整批入队; 大区间用 `parallel_map_chunked`(每 grain 元素一任务); 无返回值版 `parallel_for(...).run()`

生命周期: `p.wait()` 阻塞至全部完成; `p.shutdown(drain \| discard)` 排空或丢弃后退出(析构默认 drain). 不得在任务体内调用 `wait`/`shutdown`

## 特性标签

变参无序组合, 如 `basic_pool<decltype(priority), decltype(trace)>`:

| 标签 | 说明 |
|------|------|
| `priority` | high/normal/low 三档分层队列 |
| `cancellable` | execute 可返回 `stop_source`, 任务体收 `stop_token` |
| `trace` | `on_enqueue` / `on_begin` / `on_end` 钩子(吞吐约 0.7x) |
| `worker_cap<N>` | workers 静态存储, 无动态分配 |
| `queue_cap<G, L>` | 全局环 / 本地 deque 容量, 缺省 65536 / 256 |

运行期选项: `threads`(0 = 硬件数)、`spin_budget`(睡前自旋预算, 默认 64µs)、`hooks`

## stdexec(P2300)

可选对接 [stdexec](https://github.com/NVIDIA/stdexec), 核心库零依赖:

```cpp
#include <nexus/execution.hpp>

huxint::nexus::pool p({.threads = 4});
auto sched = huxint::nexus::ex::as_scheduler(p);
```

## 协议

[MIT](LICENSE)
