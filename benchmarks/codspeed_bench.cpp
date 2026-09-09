#include <nexus/detail/chase_lev.hpp>
#include <nexus/detail/sbo_function.hpp>
#include <nexus/nexus.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <utility>
#include <vector>

// CodSpeed 回归基准: 只测本库自身的调度路径, 每次迭代完成一批固定工作量
// (与第三方池的横向对比见 benchmarks/bench.cpp)
//
// 约定:
// - 池的创建销毁在计时之外, 计时段只含提交 / 执行 / 汇合
// - 线程数固定, 不随执行机核数漂移, 使 CI 上的历史序列可比
// - 每项以 wait() 或逐个取回收敛, 计入完整的唤醒与记账成本
namespace {

    using namespace huxint::nexus;

    constexpr std::size_t BENCH_THREADS = 4;

    pool::options bench_options() noexcept {
        return {.threads = BENCH_THREADS};
    }

    // 任务体的共享副作用: 兼作完成计数与防优化锚点
    struct counter {
        std::atomic<std::size_t> value{0};
    };

    // work-first 派生: 一支入队供窃取, 一支内联; 叶子数 = 2^depth
    void spawn_tree(pool& p, std::size_t depth, counter& c) noexcept {
        if (depth == 0) {
            c.value.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        static_cast<void>(p.fork_join([&p, depth, &c]() noexcept { spawn_tree(p, depth - 1, c); },
                                      [&p, depth, &c]() noexcept { spawn_tree(p, depth - 1, c); }));
    }

} // namespace

// -------------------------------------------------------------------- 提交路径

// 单生产者即发即忘: execute 的入队 + 唤醒 + 记账
static void BM_execute_fire_and_forget(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    pool p(bench_options());
    counter c;
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            static_cast<void>(
                p.execute([&c]() noexcept { c.value.fetch_add(1, std::memory_order_relaxed); }));
        }
        p.wait();
    }
    benchmark::DoNotOptimize(c.value.load(std::memory_order_relaxed));
}
BENCHMARK(BM_execute_fire_and_forget)->Arg(512)->Arg(4096);

// 带结果通道的提交: 全部落队后逐个取回, 计入 shared_state 的分配与领取
static void BM_submit_then_get(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    pool p(bench_options());
    std::vector<task<std::size_t>> handles;
    handles.reserve(count);
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            if (auto t = p.submit([](std::size_t x) noexcept { return x * 2; }, i)) {
                handles.push_back(std::move(*t));
            }
        }
        std::size_t sum = 0;
        for (auto& t : handles) {
            sum += t.get().value_or(0);
        }
        benchmark::DoNotOptimize(sum);
        handles.clear();
    }
}
BENCHMARK(BM_submit_then_get)->Arg(512);

// 批量提交: 整批单次唤醒, 与逐个 submit 的差量即通知摊薄的收益
static void BM_submit_each(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<std::size_t> input(count);
    std::iota(input.begin(), input.end(), 0uz);
    pool p(bench_options());
    for (auto _ : state) {
        auto batch = p.submit_each(input, [](std::size_t x) noexcept { return x * x; });
        std::size_t sum = 0;
        if (batch) {
            for (auto& t : *batch) {
                sum += t.get().value_or(0);
            }
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_submit_each)->Arg(512);

// 空池往返: 提交 -> 唤醒 -> 取回, 串行重复, 无排队掩盖唤醒成本
static void BM_idle_roundtrip(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    pool p(bench_options());
    for (auto _ : state) {
        std::size_t sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (auto t = p.submit([](std::size_t x) noexcept { return x + 1; }, i)) {
                sum += t->get().value_or(0);
            }
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_idle_roundtrip)->Arg(64);

// priority 标签: 三档轮转提交, 走分层队列的层选择与扫描路径
static void BM_priority_execute(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    basic_pool<decltype(priority)> p({.threads = BENCH_THREADS});
    counter c;
    constexpr task_priority levels[] = {task_priority::high, task_priority::normal,
                                        task_priority::low};
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            static_cast<void>(p.execute(levels[i % 3], [&c]() noexcept {
                c.value.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        p.wait();
    }
    benchmark::DoNotOptimize(c.value.load(std::memory_order_relaxed));
}
BENCHMARK(BM_priority_execute)->Arg(512);

// -------------------------------------------------------------------- 递归派生

static void BM_fork_join_tree(benchmark::State& state) {
    const auto depth = static_cast<std::size_t>(state.range(0));
    pool p(bench_options());
    counter c;
    for (auto _ : state) {
        spawn_tree(p, depth, c);
        p.wait();
    }
    benchmark::DoNotOptimize(c.value.load(std::memory_order_relaxed));
}
BENCHMARK(BM_fork_join_tree)->Arg(9);

// -------------------------------------------------------------------- 批量并行

// 每元素一任务: 调度开销占主导的细粒度形态
static void BM_parallel_for(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<std::uint64_t> data(count, 1);
    pool p(bench_options());
    for (auto _ : state) {
        static_cast<void>(
            parallel_for(p, data, [](std::uint64_t& x) noexcept { x = x * 3 + 1; }).run());
        benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(data.front());
}
BENCHMARK(BM_parallel_for)->Arg(512);

// 分块: 同样的元素数, 任务数降为块数, 差量即摊薄效果
static void BM_parallel_for_chunked(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<std::uint64_t> data(count, 1);
    pool p(bench_options());
    for (auto _ : state) {
        static_cast<void>(parallel_for_chunked(
                              p, data,
                              [](auto&& chunk) noexcept {
                                  for (auto& x : chunk) {
                                      x = x * 3 + 1;
                                  }
                              },
                              64)
                              .run());
        benchmark::ClobberMemory();
    }
    benchmark::DoNotOptimize(data.front());
}
BENCHMARK(BM_parallel_for_chunked)->Arg(4096);

// 惰性视图逐个取回: 按输入顺序消费 expected 结果
static void BM_parallel_map(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::vector<std::uint64_t> data(count, 7);
    pool p(bench_options());
    for (auto _ : state) {
        std::uint64_t sum = 0;
        for (auto&& r : parallel_map(p, data, [](std::uint64_t x) noexcept { return x * x; })) {
            sum += r.value_or(0);
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_parallel_map)->Arg(512);

// ---------------------------------------------------------------------- 组合子

// when_all 汇合 + map 变换: 多父状态的续延挂接
static void BM_when_all_map(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    pool p(bench_options());
    for (auto _ : state) {
        std::size_t sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
            auto a = p.submit([]() noexcept { return 100uz; });
            auto b = p.submit([]() noexcept { return 200uz; });
            if (a && b) {
                auto merged = when_all(std::move(*a), std::move(*b)).map([](auto&& tup) noexcept {
                    return std::get<0>(tup) + std::get<1>(tup);
                });
                sum += merged.get().value_or(0);
            }
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_when_all_map)->Arg(32);

// 续延链: map / inspect 逐级挂接, 完成后在工作线程上内联展开
static void BM_task_continuation_chain(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    pool p(bench_options());
    for (auto _ : state) {
        std::size_t sum = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (auto t = p.submit([]() noexcept { return 1uz; })) {
                auto chained =
                    t->map([](std::size_t x) noexcept { return x + 1; })
                        .map([](std::size_t x) noexcept { return x * 2; })
                        .inspect([](std::size_t& x) noexcept { benchmark::DoNotOptimize(x); })
                        .map([](std::size_t x) noexcept { return x - 1; });
                sum += chained.get().value_or(0);
            }
        }
        benchmark::DoNotOptimize(sum);
    }
}
BENCHMARK(BM_task_continuation_chain)->Arg(32);

// -------------------------------------------------------------------- 内部结构

// 工作窃取 deque 的所有者端: LIFO push/pop 快路径(单线程, 无争用)
static void BM_chase_lev_push_pop(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    detail::chase_lev_deque<void*, 256> deque;
    std::vector<int> slots(64);
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            benchmark::DoNotOptimize(deque.push(&slots[i % slots.size()]));
            benchmark::DoNotOptimize(deque.pop());
        }
    }
}
BENCHMARK(BM_chase_lev_push_pop)->Arg(1024);

// 任务体的小缓冲存储: 就地构造 + 调用 + 析构, 提交路径的每任务固定成本
static void BM_sbo_function_inplace(benchmark::State& state) {
    const auto count = static_cast<std::size_t>(state.range(0));
    std::size_t acc = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < count; ++i) {
            detail::sbo_function<48> fn{[&acc, i]() noexcept { acc += i; }};
            fn();
        }
        benchmark::DoNotOptimize(acc);
    }
}
BENCHMARK(BM_sbo_function_inplace)->Arg(1024);

BENCHMARK_MAIN();
