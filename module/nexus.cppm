module;
// 命名模块封装: 实现仍是 header-only(本单元仅做接口再导出).
// 使用方既可 #include <nexus/nexus.hpp>, 也可 import huxint.nexus;
//
// 混用须知: 模板体在使用方翻译单元实例化, 其标准库依赖必须在该单元可见,
// 且文本包含必须置于 import 语句之前(先解析后合并, 避免重定义冲突):
//
//     #include <vector>            // 先
//     import huxint.nexus;      // 后
#include <nexus/nexus.hpp>

export module huxint.nexus;

export namespace huxint::nexus {
    using huxint::nexus::basic_pool;
    using huxint::nexus::pool;
    using huxint::nexus::shutdown_policy;

    using huxint::nexus::invalid_task;
    using huxint::nexus::invalid_task_error;
    using huxint::nexus::is_cancelled;
    using huxint::nexus::is_invalid_task;
    using huxint::nexus::operation_cancelled;
    using huxint::nexus::submit_error;
    using huxint::nexus::submit_error_of;
    using huxint::nexus::task;

    using huxint::nexus::when_all;

    using huxint::nexus::parallel_for;
    using huxint::nexus::parallel_for_chunked;
    using huxint::nexus::parallel_map;
    using huxint::nexus::parallel_map_chunked;
    using huxint::nexus::parallel_view;

    using huxint::nexus::cancellable;
    using huxint::nexus::priority;
    using huxint::nexus::queue_cap;
    using huxint::nexus::task_priority;
    using huxint::nexus::trace;
    using huxint::nexus::worker_cap;

    using huxint::nexus::no_worker;
    using huxint::nexus::task_outcome;
    using huxint::nexus::task_phase;
    using huxint::nexus::trace_event;
    using huxint::nexus::trace_hooks;
} // namespace huxint::nexus
