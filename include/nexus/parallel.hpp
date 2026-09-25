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

        /// 元素在任务闭包中的携带方式. 真左值引用 -> 存指针(零拷贝, 指向底层区间);
        /// 生成式区间产出的 prvalue -> 存值副本(否则闭包运行时已悬垂)
        template <typename V>
        inline constexpr bool carry_by_pointer_v =
            std::is_lvalue_reference_v<std::ranges::range_reference_t<V>>;

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
        using result_type = std::invoke_result_t<F&, elem_ref>;
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
                                return std::invoke(*fn, std::move(v));
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
        requires std::invocable<F&, std::ranges::range_reference_t<R>>
    [[nodiscard]]
    auto parallel_map(Pool& p, R&& range, F fn) {
        using view_t = std::views::all_t<R&&>;
        return parallel_view<Pool, view_t, F>{p, std::views::all(std::forward<R>(range)),
                                              std::move(fn)};
    }

    namespace detail {

        /// parallel_for 的汇合点: 剩余计数 + 首错. 计数只在整批发布前(单线程)
        /// 累加, 发布后由各元素递减; 归零方推进池的空闲代际唤醒等待方
        class for_join {
        public:
            void add() noexcept { remaining_.fetch_add(1, std::memory_order_relaxed); }

            [[nodiscard]]
            bool done() const noexcept {
                return remaining_.load(std::memory_order_acquire) == 0;
            }

            /// 一个元素的完结: 执行(或以取消语义丢弃)并计数. 首错的写入经
            /// remaining_ 上 RMW 的释放序列对等待方可见. 归零后本对象随时可能
            /// 被等待方销毁, 故唤醒经闭包持有的池引用完成, 不再触碰 self
            template <typename Pool, typename Body>
            static void settle(for_join* self, const Pool& p, bool run, Body&& body) noexcept {
                if (!run) {
                    self->fail(shared_error<operation_cancelled>());
                } else {
                    try {
                        body();
                    } catch (...) {
                        self->fail(std::current_exception());
                    }
                }
                if (self->remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    pool_access::bump(p);
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

            std::atomic<std::size_t> remaining_{0};
            std::atomic<bool> errored_{false};
            std::exception_ptr first_;
        };

    } // namespace detail

    /**
     * @brief 并行遍历: 对区间每个元素并发调用 f(无返回值), 阻塞至全部完成
     *
     * 整批一次登记、按批唤醒; 元素任务不带结果通道(无逐元素共享状态分配),
     * 以一个计数汇合. 调用方是本池 worker 时边等边帮忙执行排队任务, 嵌套
     * parallel_for 不会因 worker 全部阻塞而死锁
     *
     * 元素携带方式同 parallel_map: 左值区间按指针(f 可原地改写), 生成式区间
     * 按值. 索引区间可用 `std::views::iota(0, n)` 表达
     *
     * @return 首个错误(f 抛出的异常 / 被关闭丢弃的 operation_cancelled / 提交失败的
     *         submit_error, 后者可经 submit_error_of 还原); 全部成功则为空
     */
    template <typename Pool, std::ranges::input_range R, typename F>
        requires std::invocable<F&, std::ranges::range_reference_t<R>> &&
                 std::is_void_v<std::invoke_result_t<F&, std::ranges::range_reference_t<R>>>
    [[nodiscard]]
    std::expected<void, std::exception_ptr> parallel_for(Pool& p, R&& range, F fn) {
        detail::for_join join;
        auto make = [&](auto&& e) {
            join.add();
            if constexpr (std::is_lvalue_reference_v<std::ranges::range_reference_t<R>>) {
                return [self = &join, pool = &p, f = &fn, elem = std::addressof(e)](
                           bool run) noexcept {
                    detail::for_join::settle(self, *pool, run, [&] { std::invoke(*f, *elem); });
                };
            } else {
                return [self = &join, pool = &p, f = &fn,
                        v = std::ranges::range_value_t<R>(std::forward<decltype(e)>(e))](
                           bool run) mutable noexcept {
                    detail::for_join::settle(self, *pool, run,
                                             [&] { std::invoke(*f, std::move(v)); });
                };
            }
        };
        // 库表面零 throw: 提交期的用户异常(元素搬运/用户迭代器)经返回值报告
        try {
            if (auto ok = detail::pool_access::execute_each(p, range, make); !ok) {
                return std::unexpected(detail::submit_error_ptr(ok.error()));
            }
        } catch (...) {
            return std::unexpected(std::current_exception());
        }
        auto done = [&] { return join.done(); };
        detail::pool_access::help_until(
            p, done, [&] { detail::pool_access::sleep_until_bumped(p, done); });
        return join.result();
    }

    namespace detail {

        /// 分块视图类型: 经 views::all 归一后的底层区间再 chunk
        template <typename R>
        using chunked_of = std::ranges::chunk_view<std::views::all_t<R&&>>;

        /// 分块: grain == 0 时块大小取池线程数
        template <typename Pool, typename R>
        [[nodiscard]]
        chunked_of<R> chunked(const Pool& p, R&& range, std::size_t grain) {
            const std::size_t size = grain != 0 ? grain : p.thread_count();
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
     * @param grain 块大小; 0 = 按 `p.thread_count()` 自动取块
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
     * @brief 分块并行遍历: 每 grain 个元素一块提交, `f` 对每块调用一次,
     *        接收子区间(range), 阻塞至全部完成. 返回语义同 parallel_for
     *
     * @param grain 块大小; 0 = 按 `p.thread_count()` 自动取块
     */
    template <typename Pool, std::ranges::forward_range R, typename F>
        requires std::invocable<F&, std::ranges::range_reference_t<detail::chunked_of<R>>> &&
                 std::is_void_v<
                     std::invoke_result_t<F&, std::ranges::range_reference_t<detail::chunked_of<R>>>>
    [[nodiscard]]
    std::expected<void, std::exception_ptr> parallel_for_chunked(Pool& p, R&& range, F fn,
                                                                 std::size_t grain = 0) {
        return parallel_for(p, detail::chunked(p, std::forward<R>(range), grain), std::move(fn));
    }

} // namespace huxint::nexus
