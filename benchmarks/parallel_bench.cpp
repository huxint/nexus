// 独立、无共享计数器业务负载的并行粒度基准. --quick 缩短扫描.
// 同一源码可直接用旧版头编译, 比较相同接口与回调语义.
// --tuned 单独复测 grain=256, 排除逐元素调度模式对机器负载的影响.
#include <nexus/nexus.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <numeric>
#include <string_view>
#include <vector>

using namespace huxint::nexus;
namespace {
    using clock_type = std::chrono::steady_clock;
    using word = unsigned long long;
    enum class workload { uniform, tail_heavy, head_heavy };

    word compute(word x, std::size_t n, workload kind) noexcept {
        const bool heavy = kind == workload::tail_heavy ? x >= n * 31 / 32 : x < n / 32;
        const int rounds = kind == workload::uniform ? 32 : (heavy ? 512 : 8);
        for (int i = 0; i < rounds; ++i) {
            x ^= x >> 13;
            x *= 0x9e3779b97f4a7c15ULL;
            x ^= x >> 29;
        }
        return x;
    }

    template <typename T>
    void require(const T& result) {
        if (!result) { std::abort(); }
    }

    template <typename Pool>
    void run(Pool& p, std::vector<word>& data, int mode, workload kind) {
        auto f = [&](word& x) { x = compute(x, data.size(), kind); };
        if (mode == 0) {
            for (auto& x : data) { f(x); }
        } else if (mode == 1) {
            require(parallel_for(p, data, f));
        } else {
            require(parallel_for_chunked(p, data, [&](auto block) {
                for (auto& x : block) { f(x); }
            }, mode == 2 ? 1 : (mode == 3 ? 0 : 256)));
        }
    }

    double median(std::vector<double> values) {
        std::ranges::sort(values);
        const auto n = values.size();
        return (values[(n - 1) / 2] + values[n / 2]) / 2;
    }
}

int main(int argc, char** argv) {
    const bool quick = argc > 1 && std::string_view(argv[1]) == "--quick";
    const bool tuned = argc > 1 && std::string_view(argv[1]) == "--tuned";
    const std::vector<std::size_t> sizes = tuned ? std::vector<std::size_t>{262144}
                                       : quick ? std::vector<std::size_t>{4096, 262144}
                                                : std::vector<std::size_t>{64, 4096, 262144};
    const std::vector<std::size_t> threads = tuned ? std::vector<std::size_t>{8}
                                         : quick ? std::vector<std::size_t>{1, 8}
                                                  : std::vector<std::size_t>{1, 2, 4, 8};
    const int rounds = tuned ? 73 : (quick ? 3 : 7);
    const int warmups = tuned ? 7 : 1;
    const char* names[] = {"serial", "auto", "chunk1", "chunk_default", "chunk256"};
    std::puts("threads,elements,workload,mode,median_ms,min_ms,max_ms,cpu_ms,tasks");
    for (auto workers : threads) {
        pool p({.threads = workers});
        for (auto n : sizes) {
            std::vector<word> data(n), expected(n);
            for (auto kind : {workload::uniform, workload::tail_heavy, workload::head_heavy}) {
                for (std::size_t i = 0; i < n; ++i) { expected[i] = compute(i, n, kind); }
                std::vector<double> wall[5], cpu[5];
                // 每轮轮换模式顺序; 校验、初始化与预热不计时.
                for (int r = 0; r < rounds + warmups; ++r) {
                    for (int j = 0; j < (tuned ? 1 : 5); ++j) {
                        const int mode = tuned ? 4 : (j + r) % 5;
                        std::iota(data.begin(), data.end(), word{0});
                        const auto c0 = std::clock();
                        const auto t0 = clock_type::now();
                        run(p, data, mode, kind);
                        const auto ms = std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
                        const double cms = 1000.0 * (std::clock() - c0) / CLOCKS_PER_SEC;
                        require(data == expected);
                        p.wait();
                        if (r >= warmups) { wall[mode].push_back(ms); cpu[mode].push_back(cms); }
                    }
                }
                // trace 单独运行, 不污染计时. 统计实际入队量, 解释粒度收益.
                std::atomic<std::size_t> tasks{0};
                trace_hooks hooks;
                hooks.on_enqueue = [&](trace_event) noexcept { tasks.fetch_add(1); };
                basic_pool<trace> traced({.threads = workers, .hooks = std::move(hooks)});
                for (int mode = tuned ? 4 : 0; mode < 5; ++mode) {
                    std::iota(data.begin(), data.end(), word{0});
                    tasks = 0;
                    run(traced, data, mode, kind);
                    require(data == expected);
                    traced.wait();
                    std::printf("%zu,%zu,%s,%s,%.6f,%.6f,%.6f,%.6f,%zu\n", workers, n,
                        kind == workload::uniform ? "uniform" :
                            (kind == workload::tail_heavy ? "tail_heavy" : "head_heavy"),
                        names[mode], median(wall[mode]),
                        *std::ranges::min_element(wall[mode]), *std::ranges::max_element(wall[mode]),
                        median(cpu[mode]), tasks.load());
                }
            }
        }
    }
}
