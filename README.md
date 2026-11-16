<p align="center">
  <img src="docs/assets/hero.svg" alt="nexus — 让任务流动，让结果汇合。C++26 工作窃取任务调度器" width="1040">
</p>

<p align="center">
  <a href="https://en.cppreference.com/w/cpp/26"><img src="https://img.shields.io/badge/C%2B%2B-26-1971c2?logo=cplusplus&logoColor=white" alt="C++26"></a>
  <img src="https://img.shields.io/badge/GCC-16.2%2B-099268" alt="GCC 16.2+">
  <img src="https://img.shields.io/badge/header--only-零核心依赖-6741d9" alt="header-only，零核心依赖">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-495057" alt="MIT license"></a>
</p>

<p align="center">
  <a href="#使用示例">使用示例</a> ·
  <a href="#性能">性能</a> ·
  <a href="#架构">架构</a> ·
  <a href="docs/api.md">接口文档</a> ·
  <a href="docs/build.md">配置与运行</a>
</p>

**nexus** 是面向低延迟、高吞吐并发计算的 C++26 任务调度库。
仅需头文件，核心零依赖；支持任务提交、结果组合与区间并行处理。

## 使用示例

```cpp
#include <nexus/nexus.hpp>
#include <print>

int main() {
    using namespace huxint::nexus;
    pool p;

    auto sum = when_all(
        p.submit([] { return 100; }),
        p.submit([] { return 200; })
    ).map([](int a, int b) { return a + b; });

    if (auto result = sum.get()) {
        std::println("sum = {}", *result); // sum = 300
    } else {
        return 1;
    }
}
```

[构建与接入](docs/build.md#构建与接入) · [更多接口](docs/api.md)

## 性能

吞吐取三次运行最佳值；延迟为采样分位数。

| 场景 | Taskflow | BS::thread_pool | nexus |
| :--- | ---: | ---: | ---: |
| 单生产者即发即忘 · 百万任务/s | 1.81 | 0.40 | **9.32** |
| 逐个提交并取回 · 百万任务/s | 0.36 | 0.27 | **2.28** |
| 递归 fork-join · 百万叶任务/s | 9.86 | 1.38 | **29.06** |
| 多生产者 ×8 · 百万任务/s | 3.00 | 1.41 | **23.04** |
| 往返延迟 P50 / P99 · µs | 2.88 / 6.21 | 4.29 / 7.19 | **0.42 / 0.81** |

## 架构

<a href="docs/assets/architecture.svg">
  <img src="docs/assets/architecture.svg" alt="nexus 架构：提交、全局与本地队列、工作窃取、执行与结果汇合" width="1040">
</a>

[接口与任务模型](docs/api.md)
