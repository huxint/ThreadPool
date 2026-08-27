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

        template <std::size_t Global, std::size_t Local>
        struct queue_cap_flag {};

        /// queue_cap 标签的缺省容量
        inline constexpr std::size_t queue_cap_default_global = 1024;
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
    /// deque 容量(均须为 2 的幂). 容量决定队列类型尺寸, 故只能做编译期标签.
    /// 全局环满后溢出链接管(不拒绝不阻塞), 环容量只影响内存占用与快路径
    /// 占比; 缺省 1024/256
    template <std::size_t Global = detail::queue_cap_default_global,
              std::size_t Local = detail::queue_cap_default_local>
    inline constexpr detail::queue_cap_flag<Global, Local> queue_cap{};

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

        template <typename T>
        inline constexpr bool is_queue_cap_flag_impl_v = false;
        template <std::size_t G, std::size_t L>
        inline constexpr bool is_queue_cap_flag_impl_v<queue_cap_flag<G, L>> = true;
        template <typename T>
        inline constexpr bool is_queue_cap_flag_v = is_queue_cap_flag_impl_v<std::remove_cv_t<T>>;

        /// 提取 queue_cap<Global, Local> 的容量; 0 表示未提供该值
        template <typename T>
        struct queue_cap_value_impl {
            static constexpr std::size_t global = 0;
            static constexpr std::size_t local = 0;
        };
        template <std::size_t G, std::size_t L>
        struct queue_cap_value_impl<queue_cap_flag<G, L>> {
            static constexpr std::size_t global = G;
            static constexpr std::size_t local = L;
        };
        template <typename T>
        struct queue_cap_value : queue_cap_value_impl<std::remove_cv_t<T>> {};

        /// 聚合提取容量(至多一份 queue_cap 标签, 池侧 static_assert 限定):
        /// 无标签时折叠为 0, 替换为缺省值
        template <typename... Flags>
        inline constexpr std::size_t queue_global_cap_v = [] {
            std::size_t v = 0uz;
            ((v = std::max(v, queue_cap_value<Flags>::global)), ...);
            return v != 0 ? v : queue_cap_default_global;
        }();
        template <typename... Flags>
        inline constexpr std::size_t queue_local_cap_v = [] {
            std::size_t v = 0uz;
            ((v = std::max(v, queue_cap_value<Flags>::local)), ...);
            return v != 0 ? v : queue_cap_default_local;
        }();

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
            (std::max({worker_cap_value<Flags>::value..., 0uz}));
    } // namespace detail
} // namespace concurrent
