#pragma once
#include <algorithm>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace huxint::nexus {

    /// 任务优先级档位(priority 标签下生效, best-effort 语义)
    /// 枚举值即层级序: 层索引 = 层数-1-档位(高优先级层号小)
    enum class task_priority : std::uint8_t {
        low = 0,
        normal = 1,
        high = 2,
    };

    namespace detail {
        struct priority_flag {};
        struct cancellable_flag {};
        struct trace_flag {};

        /// 容量合法性在标签类型上约束: 非法值(含 0)在写出标签处即编译失败,
        /// 不会被当作"未提供"而静默替换为缺省值
        template <std::size_t N>
            requires(N >= 1)
        struct worker_cap_flag {};

        template <std::size_t Global, std::size_t Local>
            requires(Global >= 2 && std::has_single_bit(Global) && Local >= 2 &&
                     std::has_single_bit(Local))
        struct queue_cap_flag {};

        /// queue_cap 标签的缺省容量. 全局环每槽按缓存行填充, 故容量直接
        /// 决定池的常驻内存(每层 Global x 64B): 65536 槽 = 4 MiB/层.
        /// 这是拿内存换吞吐 - 实测降到 8192 后 8 生产者场景积压溢出环,
        /// 落到自旋锁保护的溢出链上, 吞吐掉一半以上. 内存敏感的部署
        /// 可用 queue_cap<N> 自行下调, 正确性不受影响
        inline constexpr std::size_t queue_cap_default_global = 65536;
        inline constexpr std::size_t queue_cap_default_local = 256;
    } // namespace detail

    /// 特性标签: priority - 启用分层优先级队列
    inline constexpr detail::priority_flag priority{};
    /// 特性标签: cancellable - 任务可携带 std::stop_token 协作取消
    inline constexpr detail::cancellable_flag cancellable{};
    /// 特性标签: trace - 启用调试钩子
    inline constexpr detail::trace_flag trace{};

    /// 值标签: worker_cap<N> - workers 以 inplace_vector<jthread, N> 静态容量存储;
    /// 不带该标签时使用 std::vector 动态存储
    template <std::size_t N>
    inline constexpr detail::worker_cap_flag<N> worker_cap{};

    /// 值标签: queue_cap<Global, Local> - 全局无锁环每层容量与 worker 本地
    /// deque 容量(均须为 2 的幂且至少为 2). 容量决定队列类型尺寸, 故只能做编译期标签.
    /// 全局环满后溢出链接管, 提交不等待空槽; 容量影响内存占用与快路径占比.
    /// 大环吸收多生产者积压, 避免溢出链自旋锁争用; 缺省 65536/256
    template <std::size_t Global = detail::queue_cap_default_global,
              std::size_t Local = detail::queue_cap_default_local>
    inline constexpr detail::queue_cap_flag<Global, Local> queue_cap{};

    namespace detail {
        template <typename Tag, typename... Flags>
        inline constexpr bool has_flag_v = (std::same_as<Flags, Tag> || ...);

        /// 值标签的容量提取; 非该类标签取 0. 标签自身约束容量为正,
        /// 故 0 只可能表示"未提供"
        template <typename T>
        inline constexpr std::size_t worker_cap_of = 0;
        template <std::size_t N>
        inline constexpr std::size_t worker_cap_of<worker_cap_flag<N>> = N;

        template <typename T>
        inline constexpr std::size_t global_cap_of = 0;
        template <std::size_t G, std::size_t L>
        inline constexpr std::size_t global_cap_of<queue_cap_flag<G, L>> = G;

        template <typename T>
        inline constexpr std::size_t local_cap_of = 0;
        template <std::size_t G, std::size_t L>
        inline constexpr std::size_t local_cap_of<queue_cap_flag<G, L>> = L;

        /// 可作池模板实参的特性标签: 非标签值(如 basic_pool<3>)在实例化处即被拒
        template <typename T>
        concept pool_flag = std::same_as<T, priority_flag> || std::same_as<T, cancellable_flag> ||
                            std::same_as<T, trace_flag> || worker_cap_of<T> != 0 ||
                            global_cap_of<T> != 0;

        /// 同类值标签的出现次数(池侧 static_assert 限定至多一份)
        template <typename... Flags>
        inline constexpr std::size_t worker_cap_count_v = (0uz + ... + (worker_cap_of<Flags> != 0));
        template <typename... Flags>
        inline constexpr std::size_t queue_cap_count_v = (0uz + ... + (global_cap_of<Flags> != 0));

        /// 聚合提取容量: 0 = 动态存储(未提供 worker_cap)
        template <typename... Flags>
        inline constexpr std::size_t worker_capacity_v = std::max({worker_cap_of<Flags>..., 0uz});

        /// 无 queue_cap 标签时折叠为 0, 替换为缺省值
        template <typename... Flags>
        inline constexpr std::size_t queue_global_cap_v = [] {
            constexpr std::size_t v = std::max({global_cap_of<Flags>..., 0uz});
            return v != 0 ? v : queue_cap_default_global;
        }();
        template <typename... Flags>
        inline constexpr std::size_t queue_local_cap_v = [] {
            constexpr std::size_t v = std::max({local_cap_of<Flags>..., 0uz});
            return v != 0 ? v : queue_cap_default_local;
        }();
    } // namespace detail
} // namespace huxint::nexus
