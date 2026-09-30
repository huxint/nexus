# 接口与任务模型

[项目首页](../README.md) · [配置与运行](build.md)

`submit()` 返回 `task<T>`，`get()` 返回 `std::expected<T, std::exception_ptr>`。
提交失败与任务体异常进入同一个结果通道，成功结果只能领取一次。
完整使用示例见 [src/main.cpp](../src/main.cpp)。

## 提交与并行

| 接口 | 行为 |
| :--- | :--- |
| `submit(f, args...)` | 返回 `task<R>`，通过 `get()` 等待并取值 |
| `execute(f, args...)` | 即发即忘，返回 `submit_status`；回调须 `noexcept` |
| `submit_each(range, f)` | 批量提交，返回 `expected<vector<task<R>>, submit_error>` |
| `fork_join(f, g)` | `f` 入队、`g` 内联执行；`f` 须 `noexcept`，提交失败时不执行 `g`；由外部 `p.wait()` 汇合 |
| `parallel_map(p, range, f)` | 惰性视图，首次迭代整批提交，按输入顺序交付结果；`batch_error()` 检查批量提交错误 |
| `parallel_for(p, range, f)` | 对每个元素调用 `f`，阻塞至完成并返回状态 |
| `parallel_map_chunked` / `parallel_for_chunked` | 按 `grain` 分块，回调接收子区间视图 |

`parallel_for` 自动安排调度，单个元素抛出异常后仍会处理其余元素。
需要按子区间处理或每块返回一个结果时使用 `*_chunked`；`grain` 决定回调看到的块大小，
省略时取池线程数。

多趟区间的左值元素按引用访问，可原地修改；单趟输入（如 `std::views::istream`）和代理引用
按值快照，回调修改快照不会写回输入。所有快照准备成功后才发布任务。
并行回调需要支持并发调用，底层数据须在任务完成前保持有效。

## 任务组合

| 接口 | 行为 |
| :--- | :--- |
| `map(f)` | 变换成功值；元组支持展开传参 |
| `and_then(f)` | 接续返回 `task<U>` 的计算 |
| `inspect(f)` | 观察成功值，保留原结果 |
| `when_all(tasks...)` / `when_all(vector<task<T>>)` | 汇合为元组 / 向量结果 |

```cpp
auto sum = when_all(
    p.submit([] { return 100; }),
    p.submit([] { return 200; })
).map([](int a, int b) { return a + b; });
```

续延内联执行，适合短计算；父任务已完成时直接在调用线程执行，并省去续延节点分配。
重计算可在 `and_then` 中再次 `submit`。

## 等待与取消

- `p.wait()` 等待全池任务；`task::wait()` 只等待当前任务，不取值。
- `shutdown(shutdown_policy::drain)` 排空退出，析构默认采用此策略。
- `shutdown(shutdown_policy::discard)` 丢弃未执行任务，等待运行中的任务结束。
- `task::request_stop()` 请求取消；运行中需回调接收并轮询 `std::stop_token`。
- 错误可用 `submit_error_of()` / `is_cancelled()` 识别。

池级 `wait()` / `shutdown()` 不可在本池任务体内调用。
避免所有工作线程阻塞等待排队中的子任务，优先用组合子表达依赖；
并行接口的嵌套等待会帮助执行本池任务。

## 架构

[架构总览](../README.md#架构)
