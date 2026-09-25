#include <atomic>
#include <chrono>
#include <nexus/nexus.hpp>
#include <print>
#include <string>
#include <thread>
#include <vector>

using namespace huxint::nexus;

int main() {
    // 基础用法: submit 返回 task<T>, 提交失败同样经 get() 的错误通道报告
    pool p({.threads = 4});

    // 即发即忘(callable 需 noexcept)
    std::atomic<int> fire{0};
    p.execute([&fire]() noexcept { fire.fetch_add(1, std::memory_order_relaxed); });
    p.wait();
    std::println("fire-and-forget count: {}", fire.load());

    // 有返回值
    auto f1 = p.submit([] { return true; });
    auto f2 = p.submit([](int a, int b) { return a + b; }, 10, 20);
    auto r1 = f1.get();
    auto r2 = f2.get();
    if (r1 && r2) {
        std::println("result: {}, {}", *r1, *r2);
    }

    // 函数式组合子: when_all 汇合, map 按元素展开元组
    auto sum = when_all(p.submit([] { return 100; }), p.submit([] { return 200; }))
                   .map([](int x, int y) { return x + y; });
    if (auto r = sum.get()) {
        std::println("when_all sum: {}", *r);
    }

    // 批量: 同类任务区间汇合为 task<vector<T>>
    std::vector<int> nums{1, 2, 3, 4};
    if (auto batch = p.submit_each(nums, [](int x) { return x * 10; })) {
        auto all = when_all(std::move(*batch)).get();
        std::println("submit_each + when_all: {}", all.value_or(std::vector<int>{}));
    }

    // 并行遍历: 阻塞至全部完成, 返回首个错误
    std::vector<int> scaled(8, 1);
    if (parallel_for(p, scaled, [](int& x) { x *= 7; })) {
        std::println("parallel_for: {}", scaled);
    }

    // 惰性并行批量: 构造不提交, begin()/run() 时整批入队, 按输入顺序取回
    std::vector<int> data{1, 2, 3, 4, 5, 6, 7, 8};
    auto squares = parallel_map(p, data, [](int x) noexcept { return x * x; });
    for (auto&& r : squares) {
        if (r) {
            std::println("square: {}", *r);
        }
    }

    // 协作取消: callable 接收 std::stop_token
    pool log_pool({.threads = 2});
    auto log_task = log_pool.submit(
        [](std::stop_token tok, const std::string& msg) {
            for (int i = 0; i < 5; ++i) {
                if (tok.stop_requested()) {
                    std::println("log task cancelled: {}", msg);
                    return;
                }
                std::println("log: {} ({}/{})", msg, i + 1, 5);
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        },
        "important log");

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    log_task.request_stop(); // 请求取消日志任务

    // 带优先级(callable 需 noexcept)
    basic_pool<priority> prio_pool({.threads = 2});
    prio_pool.execute(task_priority::low, []() noexcept { std::println("low-priority task"); });
    prio_pool.execute(task_priority::high, []() noexcept { std::println("high-priority task"); });
    prio_pool.wait();

    // trace 钩子: 三阶段事件流(enqueue / begin / end)
    basic_pool<trace> traced(
        {.threads = 2,
         .hooks = {.on_enqueue = {}, .on_begin = {}, .on_end = [](trace_event e) noexcept {
                       std::println("[trace] task {} finished, outcome {}", e.id,
                                    static_cast<int>(e.outcome));
                   }}});
    traced.execute([]() noexcept {});
    traced.wait();

    return 0;
}
