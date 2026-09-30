// 统计普通全局 operator new 的调用与请求字节数; 不计 malloc、对齐分配或峰值内存.
// 与计时基准分开运行, 避免计数原子操作影响耗时对照.
#include <nexus/nexus.hpp>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <ranges>
#include <vector>

namespace {
    std::atomic<bool> tracking{false};
    std::atomic<std::size_t> calls{0}, bytes{0};

    template <typename T>
    void require(const T& result) {
        if (!result) {
            std::abort();
        }
    }

    template <typename F>
    void measure(const char* name, F fn) {
        calls = 0;
        bytes = 0;
        tracking = true;
        fn();
        tracking = false;
        std::printf("%s,%zu,%zu\n", name, calls.load(), bytes.load());
    }
}

[[gnu::noinline]]
void* operator new(std::size_t n) {
    void* p = std::malloc(n == 0 ? 1 : n);
    if (!p) {
        throw std::bad_alloc{};
    }
    if (tracking.load(std::memory_order_relaxed)) {
        calls.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(n, std::memory_order_relaxed);
    }
    return p;
}

[[gnu::noinline]]
void operator delete(void* p) noexcept { std::free(p); }
[[gnu::noinline]]
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main() {
    using namespace huxint::nexus;
    pool p({.threads = 8});
    std::vector<std::size_t> data(262144);
    auto visit = [&] {
        require(parallel_for(p, data, [](auto& x) { ++x; }));
        p.wait();
    };
    visit(); // 预热节点缓存; 初始化与池构造不计入测量.
    std::puts("operation,allocations,requested_bytes");
    measure("parallel_for_262144", visit);

    {
        // 独立预热并固定单个 worker, 减少节点缓存分布对扩容对照的影响.
        pool batch_pool({.threads = 1});
        auto batch = [&] {
            auto tasks = batch_pool.submit_each(std::views::iota(0, 10000), [](int x) { return x; });
            require(tasks);
            for (int i = 0; auto& task : *tasks) {
                require(task.get().value_or(-1) == i++);
            }
            batch_pool.wait();
        };
        batch();
        measure("submit_each_10000", batch);
    }

    auto t = p.submit([] { return 0; });
    t.wait();
    p.wait();
    measure("completed_map_10000", [&] {
        for (int i = 0; i < 10000; ++i) {
            t = t.map([](int x) { return x + 1; });
            t.wait();
        }
    });
    require(t.get().value_or(-1) == 10000);
}
