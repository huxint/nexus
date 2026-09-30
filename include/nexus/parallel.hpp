#pragma once
#include "nexus/pool.hpp"
#include "nexus/task.hpp"
#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <generator>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace huxint::nexus {

    namespace detail {

        /// 多趟区间的左值引用可借用; 单趟输入可能反复引用同一个缓存,
        /// 必须在推进迭代器前保存值快照. prvalue/proxy 同样保存 range_value_t.
        template <typename V>
        inline constexpr bool carry_by_pointer_v =
            std::ranges::forward_range<V> &&
            std::is_lvalue_reference_v<std::ranges::range_reference_t<V>>;

        /// 快照仍按输入的值类别交付: istream 的 int& 引用自己的副本,
        /// iota 的 prvalue 以右值交付. 约束和结果推导必须使用实际交付类型.
        template <typename V>
        using parallel_arg_t = std::conditional_t<
            carry_by_pointer_v<V>, std::ranges::range_reference_t<V>,
            std::conditional_t<std::is_lvalue_reference_v<std::ranges::range_reference_t<V>>,
                               std::ranges::range_value_t<V>&, std::ranges::range_value_t<V>&&>>;

        template <typename V, typename F>
        concept parallel_callable = std::ranges::input_range<V> &&
            std::invocable<F&, parallel_arg_t<V>> &&
            (carry_by_pointer_v<V> ||
             std::constructible_from<std::ranges::range_value_t<V>,
                                     std::ranges::range_reference_t<V>>);

    } // namespace detail

    /**
     * @brief 惰性批量视图: `begin()`(或 `run()`)时整批入队, 迭代按序阻塞取回 expected
     *
     * 构造不提交任何任务 - 未被迭代的视图什么也不做(故 parallel_map 为 [[nodiscard]])
     * 首次迭代经 submit_each 一次登记、单次唤醒地提交全部元素, 随后按**原始顺序**
     * 逐个阻塞获取; 即先完成的元素也要等前序元素被取走, 换来的是结果顺序与输入
     * 顺序严格一致. 本池 worker 上的取回是帮助式的(边等边执行排队任务), 嵌套使用
     * 不会因 worker 全部阻塞而死锁
     *
     * 单趟(input_range)语义: 结果值恰好可取一次. 迭代面经 `std::generator`
     * 实现, `results()` 亦可独立消费并直接组合 ranges 管道
     *
     * 生命周期: 析构阻塞至全部已提交任务完成 - 任务闭包持有 `f` 与(可能的)
     * 元素指针, 二者的有效性由本视图存续保证. 底层区间须比本视图更长寿.
     * `results()` 产出的生成器捕获本视图地址, 其存续期内本视图不可亡逸
     *
     * 线程安全: `f` 会在多个 worker 上并发调用, 须自行保证可重入
     *
     * @tparam Pool 线程池类型(任何提供 submit 的 basic_pool 实例)
     * @tparam V    经 std::views::all 归一后的区间视图
     * @tparam F    元素变换体
     */
    template <typename Pool, typename V, typename F>
        requires std::ranges::input_range<V>
    class parallel_view {
        using elem_ref = std::ranges::range_reference_t<V>;

    public:
        /// f 的返回类型
        using result_type = std::invoke_result_t<F&, detail::parallel_arg_t<V>>;
        /// 迭代产出的元素类型
        using value_type = std::expected<result_type, std::exception_ptr>;

        /// 迭代面经 std::generator 实现(见 results). generator 的迭代器类型
        /// 是实现定义细节(无公开嵌套名), 经 decltype 于 begin 取回
        using iterator = decltype(std::declval<std::generator<value_type>&>().begin());

        parallel_view(Pool& p, V range, F fn)
            : pool_(std::addressof(p)), range_(std::move(range)), fn_(std::move(fn)) {}

        /// 闭包捕获 &fn_ 与元素地址 -> 本对象地址必须稳定, 故不可拷贝也不可移动
        /// 返回语句中的 prvalue 初始化仍受强制省略保障, `auto v = parallel_map(...)` 照常可用
        parallel_view(const parallel_view&) =
            delete ("parallel_view is not copyable: closures capture its interior address");
        parallel_view&
        operator=(const parallel_view&) = delete ("parallel_view is not copy-assignable");
        parallel_view(parallel_view&&) =
            delete ("parallel_view is not movable: closures capture its interior address");
        parallel_view& operator=(parallel_view&&) = delete ("parallel_view is not move-assignable");

        /// 阻塞至全部已提交任务完成 - 闭包引用的 f 与元素指针在此之后才可失效
        ~parallel_view() {
            for (auto& t : slots_) {
                await(t);
            }
        }

        /// 结果流: 触发整批提交(幂等)后按原始顺序逐个阻塞取回. 单趟 -
        /// 首次调用产出全部结果, 后续调用(含 begin 的二次进入)产出空流,
        /// 重入不会重放任务. 附带收益: 可直接接 ranges 管道,
        /// 如 `v.results() | std::views::take(3)`
        std::generator<value_type> results() {
            launch();
            if (std::exchange(iter_began_, true)) {
                co_return; // 单趟: 已开始的流不再重放
            }
            for (std::size_t i = 0; i < count(); ++i) {
                co_yield fetch(i);
            }
        }

        /// 触发整批提交(幂等), 返回首元素迭代器. begin() 即预取首个
        /// 结果(generator 首次 begin 恢复协程至首个 co_yield)
        [[nodiscard]]
        iterator begin() {
            gen_.emplace(results());
            return gen_->begin();
        }

        [[nodiscard]]
        std::default_sentinel_t end() const noexcept {
            return {};
        }

        /// 整批提交并阻塞至全部完成, 丢弃结果值
        /// @return 首个错误(提交失败或任务体异常); 全部成功则为空
        std::expected<void, std::exception_ptr> run() {
            launch();
            std::exception_ptr first;
            for (std::size_t i = 0; i < count(); ++i) {
                if (auto r = fetch(i); !r && !first) {
                    first = r.error();
                }
            }
            if (first) {
                return std::unexpected(first);
            }
            return {};
        }

        /// 已提交(入队)的元素个数(launch 之前为 0). 整批提交, 故要么全部要么为 0
        [[nodiscard]]
        std::size_t submitted() const noexcept {
            return slots_.size();
        }

        /// 整批性失败: 池已关闭与分配失败经 submit_error 承载, 其余提交期异常
        /// (F 拷贝/元素搬运/用户迭代器)原样透传, 不误标为 OOM. 迭代时以末尾
        /// 追加的一个错误元素体现, 故不会被静默吞掉
        /// @return 空指针 = 无整批失败; 可用 huxint::nexus::submit_error_of 辨识提交类失败
        [[nodiscard]]
        std::exception_ptr batch_error() const noexcept {
            return fatal_;
        }

    private:
        /// 迭代长度: 已提交元素 + 可能的整批失败标记位
        [[nodiscard]]
        std::size_t count() const noexcept {
            return slots_.size() + (fatal_ ? 1u : 0u);
        }

        void launch() {
            if (launched_) {
                return;
            }
            launched_ = true;
            // 库表面零 throw: 提交期一切异常就地转入整批错误通道
            try {
                F* fn = std::addressof(fn_); // 本对象不可移动 -> 地址稳定
                auto batch = [&] {
                    if constexpr (detail::carry_by_pointer_v<V>) {
                        // 经 ref_view 迭代本对象持有的区间: 元素地址指向 range_ 本身,
                        // 而非某个临时副本
                        using elem_ptr = std::remove_reference_t<elem_ref>*;
                        return pool_->submit_each(
                            std::ranges::ref_view(range_) |
                                std::views::transform([](elem_ref e) { return std::addressof(e); }),
                            [fn](elem_ptr e) { return std::invoke(*fn, *e); });
                    } else {
                        return pool_->submit_each(
                            std::ranges::ref_view(range_),
                            [fn](std::ranges::range_value_t<V> v) mutable {
                                return std::invoke(*fn, static_cast<detail::parallel_arg_t<V>>(v));
                            });
                    }
                }();
                if (batch) {
                    slots_ = std::move(*batch);
                } else {
                    fatal_ = detail::submit_error_ptr(batch.error());
                }
            } catch (...) { // F 拷贝/元素搬运/用户迭代器抛出: 原样保留, 不误标为 OOM
                fatal_ = std::current_exception();
            }
        }

        /// 等待单个任务完成; 本池 worker 上边等边帮忙执行排队任务
        void await(task<result_type>& t) noexcept {
            detail::pool_access::help_until(
                *pool_, [&] { return t.ready(); }, [&] { t.wait(); });
        }

        /// 阻塞取回第 i 个结果
        [[nodiscard]]
        value_type fetch(std::size_t i) {
            if (i >= slots_.size()) {
                return std::unexpected(fatal_); // 哨兵位: 仅整批失败时可达(见 count)
            }
            auto& t = slots_[i];
            await(t);
            try {
                return t.get();
            } catch (...) { // 结果类型的移动构造可能抛
                return std::unexpected(std::current_exception());
            }
        }

        Pool* pool_;
        V range_;
        F fn_;
        std::vector<task<result_type>> slots_;
        std::exception_ptr fatal_; ///< 整批性失败(提交期); 空 = 无
        /// begin() 委托的生成器. 单趟: begin 重新 emplace, 旧生成器销毁即
        /// 弃流(已提交任务不受影响, 由 slots_ 与析构等待兜底)
        std::optional<std::generator<value_type>> gen_{};
        bool launched_ = false;
        bool iter_began_ = false; ///< 单趟: results/begin 只发一次真流
    };

    /**
     * @brief 惰性并行映射: 对区间每个元素并发调用 f, 按输入顺序产出
     *        `std::expected<f 的返回类型, std::exception_ptr>`
     *
     * 每元素一个任务 - 元素级工作量过小时请先自行分块(如 `std::views::chunk`),
     * 以摊薄单任务调度开销
     *
     * @warning 惰性: 不迭代(或不调用 run())则一个任务都不会提交
     */
    template <typename Pool, std::ranges::input_range R, typename F>
        requires detail::parallel_callable<R, F>
    [[nodiscard]]
    auto parallel_map(Pool& p, R&& range, F fn) {
        using view_t = std::views::all_t<R&&>;
        return parallel_view<Pool, view_t, F>{p, std::views::all(std::forward<R>(range)),
                                              std::move(fn)};
    }

    namespace detail {

        /// parallel_for 的汇合点: 发布前确定任务数, 完结时递减并保留首错.
        /// 归零方推进池的空闲代际, 唤醒等待方.
        class for_join {
        public:
            explicit for_join(std::size_t count) noexcept : remaining_(count) {}

            /// 用于未知长度的分块视图; 只在整批发布前调用.
            void add() noexcept { remaining_.fetch_add(1, std::memory_order_relaxed); }

            template <typename Pool, typename R, typename Make>
            std::expected<void, std::exception_ptr> run(Pool& p, R&& range, Make&& make) {
                try {
                    if (auto ok = pool_access::execute_each(
                            p, std::forward<R>(range), std::forward<Make>(make)); !ok) {
                        return std::unexpected(submit_error_ptr(ok.error()));
                    }
                } catch (...) {
                    return std::unexpected(std::current_exception());
                }
                auto done = [&] { return this->done(); };
                pool_access::help_until(p, done, [&] { pool_access::sleep_until_bumped(p, done); });
                return result();
            }

            [[nodiscard]]
            bool done() const noexcept {
                return remaining_.load(std::memory_order_acquire) == 0;
            }

            /// 一个调度任务的完结: 执行(或以取消语义丢弃)并计数. 首错的写入经
            /// remaining_ 上 RMW 的释放序列对等待方可见. 归零后本对象随时可能
            /// 被等待方销毁, 故唤醒经闭包持有的池引用完成, 不再触碰 self
            template <typename Pool, typename Body>
            static void settle(for_join* self, const Pool& p, bool run, Body&& body) noexcept {
                if (!run) {
                    self->fail(shared_error<operation_cancelled>());
                } else {
                    self->invoke(std::forward<Body>(body));
                }
                if (self->remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    pool_access::bump(p);
                }
            }

            /// 每元素隔离异常: 一个元素失败不能跳过同块剩余元素.
            template <typename Body>
            void invoke(Body&& body) noexcept {
                try {
                    body();
                } catch (...) {
                    fail(std::current_exception());
                }
            }

            /// @pre done()
            [[nodiscard]]
            std::expected<void, std::exception_ptr> result() const {
                if (errored_.load(std::memory_order_relaxed)) {
                    return std::unexpected(first_);
                }
                return {};
            }

        private:
            void fail(std::exception_ptr e) noexcept {
                if (!errored_.exchange(true, std::memory_order_relaxed)) {
                    first_ = std::move(e);
                }
            }

            std::atomic<std::size_t> remaining_;
            std::atomic<bool> errored_{false};
            std::exception_ptr first_;
        };

        /// 只为 worker 任务分配节点和记账, 小块由一个游标分发.
        /// 元素访问器与其引用的数据存活至帮助式汇合结束.
        template <typename Pool, typename F>
        std::expected<void, std::exception_ptr> bulk_for(Pool& p, std::size_t n, F&& fn) {
            constexpr auto blocks_per_worker = 128uz;
            const auto threads = p.thread_count();
            const auto per_worker = n / threads + (n % threads != 0);
            const auto grain = std::max(
                1uz, per_worker / blocks_per_worker + (per_worker % blocks_per_worker != 0));
            const auto blocks = n / grain + (n % grain != 0);
            const auto tasks = std::min(blocks, threads);
            for_join join{tasks};
            // 首块按任务号分配; 后续领取只需一次 relaxed RMW.
            // 块数与线程数成比例, 游标不会随元素总数膨胀.
            std::atomic<std::size_t> next{tasks};
            auto work = [&](std::size_t block) {
                for (;;) {
                    const auto first = block * grain;
                    const auto last = first + std::min(grain, n - first);
                    for (auto i = first; i < last; ++i) {
                        join.invoke([&] { std::invoke(fn, i); });
                    }
                    block = next.fetch_add(1, std::memory_order_relaxed);
                    if (block >= blocks) {
                        return;
                    }
                }
            };
            auto make = [&](std::size_t block) {
                return [self = &join, pool = &p, f = &work, block](bool run) noexcept {
                    for_join::settle(self, *pool, run, [&] { (*f)(block); });
                };
            };
            return join.run(p, std::views::iota(std::size_t{0}, tasks), make);
        }

        template <typename R>
        struct carried_element {
            using storage_t = std::conditional_t<carry_by_pointer_v<R>,
                std::add_pointer_t<std::remove_reference_t<std::ranges::range_reference_t<R>>>,
                std::ranges::range_value_t<R>>;
            storage_t value;

            explicit carried_element(std::ranges::range_reference_t<R> e)
                : value([&]() -> storage_t {
                    if constexpr (carry_by_pointer_v<R>) {
                        return std::addressof(e);
                    } else {
                        return storage_t(std::forward<std::ranges::range_reference_t<R>>(e));
                    }
                }()) {}

            parallel_arg_t<R> get() {
                if constexpr (carry_by_pointer_v<R>) {
                    return *value;
                } else {
                    return static_cast<parallel_arg_t<R>>(value);
                }
            }
        };

        /// 调用方已经划好块: 直接提交, 不再准备另一份元素表或重复分块.
        template <typename Pool, typename R, typename F>
        std::expected<void, std::exception_ptr> for_chunks(Pool& p, R&& chunks, F& fn) {
            for_join join{0};
            auto make = [&](auto&& chunk) {
                join.add();
                return [self = &join, pool = &p, f = &fn,
                        value = carried_element<R>(std::forward<decltype(chunk)>(chunk))]
                       (bool run) mutable noexcept {
                    for_join::settle(self, *pool, run, [&] { std::invoke(*f, value.get()); });
                };
            };
            return join.run(p, std::forward<R>(chunks), make);
        }
    } // namespace detail

    /**
     * @brief 并行遍历: 对每个元素调用 f, 阻塞至全部完成; 单元素失败不跳过其余元素.
     *
     * 默认由少量任务动态领取块, 回调仍接收单个元素. 连续区间直接借用;
     * 其他区间在提交线程准备元素指针/值快照, 保持用户迭代器串行访问与
     * 准备期失败的整批回滚. 单趟输入的左值引用也必须快照.
     *
     * 本池 worker 等待时帮助执行任务. f 会被并发调用; 调度粒度由库管理.
     */
    template <typename Pool, std::ranges::input_range R, typename F>
        requires detail::parallel_callable<R, F> &&
                 std::is_void_v<std::invoke_result_t<F&, detail::parallel_arg_t<R>>>
    [[nodiscard]]
    std::expected<void, std::exception_ptr> parallel_for(Pool& p, R&& range, F fn) {
        try {
            if constexpr (std::ranges::contiguous_range<R> && std::ranges::sized_range<R>) {
                auto* data = std::ranges::data(range);
                return detail::bulk_for(p, static_cast<std::size_t>(std::ranges::size(range)),
                    [&](std::size_t i) { std::invoke(fn, data[i]); });
            } else {
                std::vector<detail::carried_element<R>> elements;
                if constexpr (std::ranges::sized_range<R>) {
                    elements.reserve(static_cast<std::size_t>(std::ranges::size(range)));
                }
                for (auto&& e : range) {
                    elements.emplace_back(std::forward<decltype(e)>(e));
                }
                return detail::bulk_for(p, elements.size(),
                    [&](std::size_t i) { std::invoke(fn, elements[i].get()); });
            }
        } catch (...) {
            return std::unexpected(std::current_exception());
        }
    }

    namespace detail {

        /// 分块视图类型: 经 views::all 归一后的底层区间再 chunk
        template <typename R>
        using chunked_of = std::ranges::chunk_view<std::views::all_t<R&&>>;

        /// 回调可见的块边界保持稳定: grain == 0 时块大小取池线程数.
        template <typename Pool, typename R>
        [[nodiscard]]
        chunked_of<R> chunked(const Pool& p, R&& range, std::size_t grain) {
            const auto size = grain != 0 ? grain : p.thread_count();
            return chunked_of<R>{
                std::views::all(std::forward<R>(range)),
                static_cast<std::ranges::range_difference_t<std::views::all_t<R&&>>>(size)};
        }

    } // namespace detail

    /**
     * @brief 惰性分块并行映射: 每 grain 个元素一块提交, `f` 对每块调用一次,
     *        接收子区间(range), 产出 `std::expected<f 的返回类型, exception_ptr>`
     *
     * 元素级开销摊薄到块级 - 大区间应优先分块(每元素一任务的调度开销在细粒度
     * 元素上是主要成本); 亦缓解整批提交期的内存尖峰: 任务数从元素数降为块数
     *
     * 块是引用底层的视图: 底层区间须比本视图更长寿(同 parallel_map)
     *
     * @param grain 块大小; 0 = 按 `p.thread_count()` 取块
     *
     * @warning 惰性: 不迭代(或不调用 run())则一个任务都不会提交
     */
    template <typename Pool, std::ranges::forward_range R, typename F>
        requires std::invocable<F&, std::ranges::range_reference_t<detail::chunked_of<R>>>
    [[nodiscard]]
    auto parallel_map_chunked(Pool& p, R&& range, F fn, std::size_t grain = 0) {
        return parallel_view<Pool, detail::chunked_of<R>, F>{
            p, detail::chunked(p, std::forward<R>(range), grain), std::move(fn)};
    }

    /**
     * @brief 分块并行遍历: 每 grain 个元素一块, `f` 对每块调用一次,
     *        接收子区间(range), 阻塞至全部完成. 返回语义同 parallel_for
     *
     * @param grain 块大小; 0 = 按 `p.thread_count()` 取块
     */
    template <typename Pool, std::ranges::forward_range R, typename F>
        requires std::invocable<F&, std::ranges::range_reference_t<detail::chunked_of<R>>> &&
                 std::is_void_v<
                     std::invoke_result_t<F&, std::ranges::range_reference_t<detail::chunked_of<R>>>>
    [[nodiscard]]
    std::expected<void, std::exception_ptr> parallel_for_chunked(Pool& p, R&& range, F fn,
                                                                 std::size_t grain = 0) {
        return detail::for_chunks(p, detail::chunked(p, std::forward<R>(range), grain), fn);
    }

} // namespace huxint::nexus
