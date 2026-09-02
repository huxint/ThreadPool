#pragma once
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace concurrent::detail {

    /// 小缓冲优化的移动专用函数. 可调用体不超过 SboBytes 时零堆分配
    /// 调用签名固定为 R(). 构造时的堆分配仅发生在提交边界, 由调用方 catch
    template <std::size_t SboBytes, typename R = void>
    class sbo_function {
        struct vtable {
            R (*invoke)(std::byte* self);
            void (*destroy)(std::byte* self) noexcept;
            /// 从 src 构造到 dst(就地模式移动对象; 堆模式复制指针), 不清空 src 的 vptr
            void (*move)(std::byte* dst, std::byte* src) noexcept;
        };

        /// 就地存储的准入: 尺寸与对齐都落在缓冲内
        template <typename FD>
        static constexpr bool inplace =
            sizeof(FD) <= SboBytes && alignof(FD) <= alignof(std::max_align_t);

        template <typename FD>
        static const vtable* inplace_vt() noexcept {
            static const vtable vt{
                [](std::byte* s) -> R { return (*std::launder(reinterpret_cast<FD*>(s)))(); },
                [](std::byte* s) noexcept {
                    std::destroy_at(std::launder(reinterpret_cast<FD*>(s)));
                },
                [](std::byte* d, std::byte* s) noexcept {
                    auto* f = std::launder(reinterpret_cast<FD*>(s));
                    ::new (static_cast<void*>(d)) FD(std::move(*f));
                    std::destroy_at(f);
                },
            };
            return &vt;
        }

        template <typename FD>
        static const vtable* heap_vt() noexcept {
            static const vtable vt{
                [](std::byte* s) -> R { return (**std::launder(reinterpret_cast<FD**>(s)))(); },
                [](std::byte* s) noexcept { delete *std::launder(reinterpret_cast<FD**>(s)); },
                [](std::byte* d, std::byte* s) noexcept {
                    ::new (static_cast<void*>(d)) FD*(*std::launder(reinterpret_cast<FD**>(s)));
                },
            };
            return &vt;
        }

    public:
        using result_type = R;

        sbo_function() noexcept = default;

        template <typename F>
            requires(!std::same_as<std::remove_cvref_t<F>, sbo_function>) &&
                    std::is_invocable_r_v<R, std::decay_t<F>&>
        sbo_function(F&& f) {
            emplace_with([&]() -> std::decay_t<F> { return std::forward<F>(f); });
        }

        ~sbo_function() {
            if (vt_) {
                vt_->destroy(storage_);
            }
        }

        sbo_function(sbo_function&& other) noexcept : vt_(other.vt_) {
            if (vt_) {
                vt_->move(storage_, other.storage_);
                other.vt_ = nullptr;
            }
        }

        sbo_function& operator=(sbo_function&& other) noexcept {
            if (this != &other) {
                reset();
                if ((vt_ = other.vt_)) {
                    vt_->move(storage_, other.storage_);
                    other.vt_ = nullptr;
                }
            }
            return *this;
        }

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
            requires std::is_invocable_r_v<R, std::decay_t<std::invoke_result_t<Factory>>&>
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

        R operator()() { return vt_->invoke(storage_); }

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

} // namespace concurrent::detail
