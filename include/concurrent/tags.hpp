#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace concurrent {

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

        template <std::size_t N>
        struct worker_cap_flag {};
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

    namespace detail {
        // 标签以 `inline constexpr` 声明 -> decltype(标签) 携带顶层 const,
        // 以下所有判别均先剥离 cv 再比较, 避免 const 导致匹配失败
        template <typename T>
        inline constexpr bool is_priority_flag_v = std::same_as<std::remove_cv_t<T>, priority_flag>;
        template <typename T>
        inline constexpr bool is_cancellable_flag_v =
            std::same_as<std::remove_cv_t<T>, cancellable_flag>;
        template <typename T>
        inline constexpr bool is_trace_flag_v = std::same_as<std::remove_cv_t<T>, trace_flag>;

        template <typename T>
        inline constexpr bool is_worker_cap_flag_impl_v = false;
        template <std::size_t N>
        inline constexpr bool is_worker_cap_flag_impl_v<worker_cap_flag<N>> = true;
        template <typename T>
        inline constexpr bool is_worker_cap_flag_v = is_worker_cap_flag_impl_v<std::remove_cv_t<T>>;

        template <typename... Flags>
        inline constexpr bool has_priority_v = (is_priority_flag_v<Flags> || ...);
        template <typename... Flags>
        inline constexpr bool has_cancellable_v = (is_cancellable_flag_v<Flags> || ...);
        template <typename... Flags>
        inline constexpr bool has_trace_v = (is_trace_flag_v<Flags> || ...);

        /// 提取 worker_cap<N> 的容量; 0 表示动态存储(未提供标签)
        template <typename T>
        struct worker_cap_value_impl {
            static constexpr std::size_t value = 0;
        };
        template <std::size_t N>
        struct worker_cap_value_impl<worker_cap_flag<N>> {
            static constexpr std::size_t value = N;
        };
        template <typename T>
        struct worker_cap_value : worker_cap_value_impl<std::remove_cv_t<T>> {};

        template <typename... Flags>
        inline constexpr std::size_t worker_capacity_v =
            (std::max({worker_cap_value<Flags>::value..., std::size_t{0}}));
    } // namespace detail
} // namespace concurrent
