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

// 第三方基线头未必按本仓库的警告集编写, 仅对引入行关闭诊断
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#include "third_party/BS_thread_pool.hpp"
#include <taskflow/taskflow.hpp>
#ifdef NEXUS_BENCH_TBB
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#endif
#pragma GCC diagnostic pop

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

// -------------------------------------------------------------------- 第三方对照
//
// 同一负载 / 同一线程数(4) / 同一任务体, 每库一个基准, 命名 BM_<lib>_<case>[n],
// 便于在 CodSpeed 页面上并排比较"完成同样的活花多少指令"(CPU 模拟, 硬件无关)。
//
// 口径注意:
// - 模拟器串行化线程, 这里反映的是每任务调度开销, 不含并行加速比
// - 各库一律用其惯用 API, 并以阻塞式等待收敛(不忙等轮询), 免得空转指令污染计数
// - 池的创建销毁在计时段之外; 任务体与计数口径同本库各基准
// - 基于 moodycamel 队列的自建池是忙等轮询实现, 在指令计数下会失真,
//   故不进入本组对照(它的墙钟对比见 benchmarks/bench.cpp)

namespace {

    // 统一入口: make() 建池; run_batch() 提交 count 个空任务并阻塞至全部完成;
    // spawn() + wait_leaves() 为 work-first 二叉派生(一支入队供窃取, 一支内联)
    struct nexus_backend {
        using pool_t = pool;
        static pool_t make() { return pool({.threads = BENCH_THREADS}); }
        static void run_batch(pool_t& p, counter& c, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                static_cast<void>(
                    p.execute([&c]() noexcept { c.value.fetch_add(1, std::memory_order_relaxed); }));
            }
            p.wait();
        }
        static void spawn(pool_t& p, counter& c, std::size_t depth) { spawn_tree(p, depth, c); }
        static void wait_leaves(pool_t& p, counter&, std::size_t) { p.wait(); }
    };

    struct taskflow_backend {
        using pool_t = tf::Executor;
        static pool_t make() { return tf::Executor(BENCH_THREADS); }
        static void run_batch(pool_t& p, counter& c, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                p.silent_async([&c] { c.value.fetch_add(1, std::memory_order_relaxed); });
            }
            p.wait_for_all();
        }
        static void spawn(pool_t& p, counter& c, std::size_t depth) {
            if (depth == 0) {
                c.value.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            p.silent_async([&p, &c, depth] { spawn(p, c, depth - 1); });
            spawn(p, c, depth - 1);
        }
        static void wait_leaves(pool_t& p, counter&, std::size_t) { p.wait_for_all(); }
    };

    struct bs_backend {
        using pool_t = BS::thread_pool<>;
        static pool_t make() { return BS::thread_pool(BENCH_THREADS); }
        static void run_batch(pool_t& p, counter& c, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                p.detach_task([&c] { c.value.fetch_add(1, std::memory_order_relaxed); });
            }
            p.wait();
        }
        static void spawn(pool_t& p, counter& c, std::size_t depth) {
            if (depth == 0) {
                c.value.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            p.detach_task([&p, &c, depth] { spawn(p, c, depth - 1); });
            spawn(p, c, depth - 1);
        }
        static void wait_leaves(pool_t& p, counter&, std::size_t) { p.wait(); }
    };

#ifdef NEXUS_BENCH_TBB
    // task_arena 不可移动, 靠保证的复制消除按值返回
    struct onetbb_backend {
        struct pool_t {
            tbb::task_arena arena;
            explicit pool_t(std::size_t n) : arena(static_cast<int>(n), 0) {}
        };
        static pool_t make() { return pool_t{BENCH_THREADS}; }
        static void run_batch(pool_t& p, counter& c, std::size_t count) {
            p.arena.execute([&] {
                tbb::task_group g;
                for (std::size_t i = 0; i < count; ++i) {
                    g.run([&c] { c.value.fetch_add(1, std::memory_order_relaxed); });
                }
                g.wait();
            });
        }
        static void spawn(pool_t& p, counter& c, std::size_t depth) {
            p.arena.execute([&] {
                tbb::task_group g;
                const auto go = [&](auto&& self, std::size_t d) -> void {
                    if (d == 0) {
                        c.value.fetch_add(1, std::memory_order_relaxed);
                        return;
                    }
                    g.run([&, d] { self(self, d - 1); });
                    self(self, d - 1);
                };
                go(go, depth);
                g.wait();
            });
        }
        static void wait_leaves(pool_t&, counter&, std::size_t) {}
    };
#endif

    template <typename Backend>
    void execute_impl(benchmark::State& state) {
        const auto count = static_cast<std::size_t>(state.range(0));
        auto p = Backend::make();
        counter c;
        for (auto _ : state) {
            c.value.store(0, std::memory_order_relaxed);
            Backend::run_batch(p, c, count);
        }
        benchmark::DoNotOptimize(c.value.load(std::memory_order_relaxed));
    }

    template <typename Backend>
    void fork_join_impl(benchmark::State& state) {
        const auto depth = static_cast<std::size_t>(state.range(0));
        const std::size_t leaves_expect = std::size_t{1} << depth;
        auto p = Backend::make();
        counter c;
        for (auto _ : state) {
            c.value.store(0, std::memory_order_relaxed);
            Backend::spawn(p, c, depth);
            Backend::wait_leaves(p, c, leaves_expect);
        }
        benchmark::DoNotOptimize(c.value.load(std::memory_order_relaxed));
    }

} // namespace

// 单生产者即发即忘
static void BM_nexus_execute(benchmark::State& state) { execute_impl<nexus_backend>(state); }
static void BM_taskflow_execute(benchmark::State& state) { execute_impl<taskflow_backend>(state); }
static void BM_bs_execute(benchmark::State& state) { execute_impl<bs_backend>(state); }
BENCHMARK(BM_nexus_execute)->Arg(512)->Arg(4096);
BENCHMARK(BM_taskflow_execute)->Arg(512)->Arg(4096);
BENCHMARK(BM_bs_execute)->Arg(512)->Arg(4096);
#ifdef NEXUS_BENCH_TBB
static void BM_onetbb_execute(benchmark::State& state) { execute_impl<onetbb_backend>(state); }
BENCHMARK(BM_onetbb_execute)->Arg(512)->Arg(4096);
#endif

// work-first 二叉派生
static void BM_nexus_fork_join(benchmark::State& state) { fork_join_impl<nexus_backend>(state); }
static void BM_taskflow_fork_join(benchmark::State& state) {
    fork_join_impl<taskflow_backend>(state);
}
static void BM_bs_fork_join(benchmark::State& state) { fork_join_impl<bs_backend>(state); }
BENCHMARK(BM_nexus_fork_join)->Arg(9);
BENCHMARK(BM_taskflow_fork_join)->Arg(9);
BENCHMARK(BM_bs_fork_join)->Arg(9);
#ifdef NEXUS_BENCH_TBB
static void BM_onetbb_fork_join(benchmark::State& state) { fork_join_impl<onetbb_backend>(state); }
BENCHMARK(BM_onetbb_fork_join)->Arg(9);
#endif

BENCHMARK_MAIN();

