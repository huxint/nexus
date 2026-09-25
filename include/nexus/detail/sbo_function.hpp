#pragma once
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace huxint::nexus::detail {

    template <std::size_t SboBytes, typename Sig = void()>
    class sbo_function;

    /// 小缓冲优化的类型擦除函数, 就地驻留于宿主(任务节点)且不可移动.
    /// 可调用体不超过 SboBytes 时零堆分配. 构造时的堆分配仅发生在提交边界,
    /// 由调用方 catch
    template <std::size_t SboBytes, typename R, typename... Args>
    class sbo_function<SboBytes, R(Args...)> {
        struct vtable {
            R (*invoke)(std::byte* self, Args... args);
            /// 调用后即析构: 执行一次即弃的宿主(任务节点)只付一次间接调用
            R (*consume)(std::byte* self, Args... args);
            void (*destroy)(std::byte* self) noexcept;
        };

        /// consume 的析构守卫: 可调用体抛出时同样析构
        template <typename FD>
        struct destroy_on_exit {
            FD* f;
            ~destroy_on_exit() { std::destroy_at(f); }
        };

        /// 就地存储的准入: 尺寸与对齐都落在缓冲内
        template <typename FD>
        static constexpr bool inplace =
            sizeof(FD) <= SboBytes && alignof(FD) <= alignof(std::max_align_t);

        template <typename FD>
        static const vtable* inplace_vt() noexcept {
            static const vtable vt{
                [](std::byte* s, Args... args) -> R {
                    return (*std::launder(reinterpret_cast<FD*>(s)))(std::forward<Args>(args)...);
                },
                [](std::byte* s, Args... args) -> R {
                    const destroy_on_exit<FD> guard{std::launder(reinterpret_cast<FD*>(s))};
                    return (*guard.f)(std::forward<Args>(args)...);
                },
                [](std::byte* s) noexcept {
                    std::destroy_at(std::launder(reinterpret_cast<FD*>(s)));
                },
            };
            return &vt;
        }

        template <typename FD>
        static const vtable* heap_vt() noexcept {
            static const vtable vt{
                [](std::byte* s, Args... args) -> R {
                    return (**std::launder(reinterpret_cast<FD**>(s)))(std::forward<Args>(args)...);
                },
                [](std::byte* s, Args... args) -> R {
                    const std::unique_ptr<FD> owned{*std::launder(reinterpret_cast<FD**>(s))};
                    return (*owned)(std::forward<Args>(args)...);
                },
                [](std::byte* s) noexcept { delete *std::launder(reinterpret_cast<FD**>(s)); },
            };
            return &vt;
        }

    public:
        using result_type = R;

        sbo_function() noexcept = default;

        template <typename F>
            requires(!std::same_as<std::remove_cvref_t<F>, sbo_function>) &&
                    std::is_invocable_r_v<R, std::decay_t<F>&, Args...>
        sbo_function(F&& f) {
            emplace_with([&]() -> std::decay_t<F> { return std::forward<F>(f); });
        }

        ~sbo_function() { reset(); }

        sbo_function(const sbo_function&) = delete;
        sbo_function& operator=(const sbo_function&) = delete;

        [[nodiscard]]
        explicit operator bool() const noexcept {
            return vt_ != nullptr;
        }

        /**
         * @brief 就地构造可调用体: make() 返回的纯右值直接落在本对象的存储上
         *        (保证的复制消除), 免去"先建临时再移动进来"的一次移动构造 + 析构
         *
         * @pre 当前为空(*this 为 false)
         * @note make() 抛出时本对象保持为空, 存储原样可用
         */
        template <typename Factory>
            requires std::is_invocable_r_v<R, std::decay_t<std::invoke_result_t<Factory>>&, Args...>
        void emplace_with(Factory&& make) {
            using FD = std::decay_t<std::invoke_result_t<Factory>>;
            if constexpr (inplace<FD>) {
                ::new (static_cast<void*>(storage_)) FD(std::forward<Factory>(make)());
                vt_ = inplace_vt<FD>();
            } else {
                FD* p = new FD(std::forward<Factory>(make)()); // bad_alloc 仅在提交边界被捕获
                ::new (static_cast<void*>(storage_)) FD*(p);
                vt_ = heap_vt<FD>();
            }
        }

        R operator()(Args... args) { return vt_->invoke(storage_, std::forward<Args>(args)...); }

        /// 调用并析构可调用体, 之后为空. 等价于 operator() 接 reset(), 但只有
        /// 一次间接调用. 可调用体抛出时同样析构并置空
        /// @pre 非空
        R consume(Args... args) {
            const vtable* vt = std::exchange(vt_, nullptr);
            return vt->consume(storage_, std::forward<Args>(args)...);
        }

        void reset() noexcept {
            if (vt_) {
                vt_->destroy(storage_);
                vt_ = nullptr;
            }
        }

    private:
        alignas(std::max_align_t)
            std::byte storage_[SboBytes > sizeof(void*) ? SboBytes : sizeof(void*)]{};
        const vtable* vt_ = nullptr;
    };

} // namespace huxint::nexus::detail
