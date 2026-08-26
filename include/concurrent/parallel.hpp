#pragma once
#include "concurrent/pool.hpp"
#include "concurrent/task.hpp"
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

namespace concurrent {

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
     * 构造不提交任何任务 - 未被迭代的视图什么也不做(故 parallel_* 均为 [[nodiscard]])
     * 首次迭代一次性提交全部元素, 随后按**原始顺序**逐个阻塞获取; 即先完成的元素
     * 也要等前序元素被取走, 换来的是结果顺序与输入顺序严格一致
     *
     * 单趟(input_range)语义: 结果值恰好可取一次
     *
     * 生命周期: 析构阻塞至全部已提交任务完成 - 任务闭包持有 `f` 与(可能的)
     * 元素指针, 二者的有效性由本视图存续保证. 底层区间须比本视图更长寿
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
        /// f 的返回类型(parallel_for 下为 void)
        using result_type = std::invoke_result_t<F&, elem_ref>;
        /// 迭代产出的元素类型
        using value_type = std::expected<result_type, std::exception_ptr>;

    private:
        using slot_t = std::expected<task<result_type>, submit_error>;

    public:
        struct sentinel {};

        /// 单趟输入迭代器: `++` 阻塞取回下一个结果, `*` 可重复读当前结果
        class iterator {
        public:
            using iterator_category = std::input_iterator_tag;
            using iterator_concept = std::input_iterator_tag;
            using value_type = parallel_view::value_type;
            using difference_type = std::ptrdiff_t;
            using reference = const value_type&;

            iterator() = default;

            iterator(parallel_view* v, std::size_t i) : view_(v), index_(i) { load(); }

            [[nodiscard]]
            reference operator*() const noexcept {
                return *current_;
            }

            [[nodiscard]]
            const value_type* operator->() const noexcept {
                return &*current_;
            }

            iterator& operator++() {
                ++index_;
                load();
                return *this;
            }

            void operator++(int) { ++*this; } // 单趟迭代器: 后置递增不返回旧值

            [[nodiscard]]
            bool operator==(sentinel) const noexcept {
                return !current_.has_value();
            }

        private:
            void load() {
                if (view_ && index_ < view_->count()) {
                    current_ = view_->fetch(index_);
                } else {
                    current_.reset();
                }
            }

            parallel_view* view_ = nullptr;
            std::size_t index_ = 0;
            std::optional<value_type> current_{};
        };

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
            for (auto& s : slots_) {
                if (s) {
                    s->wait();
                }
            }
        }

        /// 触发整批提交(幂等), 返回首元素迭代器. 单趟语义: 二次调用返回
        /// 末尾迭代器 - 首轮迭代已按序消费全部结果, 重入不会重放任务
        [[nodiscard]]
        iterator begin() {
            launch();
            if (std::exchange(iter_began_, true)) {
                return iterator{};
            }
            return iterator{this, 0};
        }

        [[nodiscard]]
        sentinel end() const noexcept {
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

        /// 已成功提交(入队)的元素个数(launch 之前为 0); 提交失败的槽位不计入
        [[nodiscard]]
        std::size_t submitted() const noexcept {
            std::size_t ok = 0;
            for (const auto& s : slots_) {
                if (s.has_value()) {
                    ++ok;
                }
            }
            return ok;
        }

        /// 整批性失败(提交期抛出的异常): 容器扩容等分配失败仍经 submit_error
        /// 承载, 其余异常(F 拷贝/元素搬运/用户迭代器)原样透传, 不误标为 OOM.
        /// 迭代时以末尾追加的一个错误元素体现, 故不会被静默吞掉
        /// @return 空指针 = 无整批失败; 可用 concurrent::submit_error_of 辨识提交类失败
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
                if constexpr (std::ranges::sized_range<V>) {
                    slots_.reserve(static_cast<std::size_t>(std::ranges::size(range_)));
                }
                // GCC 在 -O3 + 消毒器插桩下对 iota_view 范围 for 的已知误报:
                // 报告编译器内部临时量"可能未初始化", 实际路径恒已构造
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
                for (auto&& e : range_) {
                    slot_t slot = submit_one(std::forward<decltype(e)>(e));
                    slots_.push_back(std::move(slot));
                }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
            } catch (const std::bad_alloc&) {
                fatal_ = std::make_exception_ptr(submit_error::out_of_memory);
            } catch (...) { // F 拷贝/元素搬运/用户迭代器抛出: 原样保留, 不误标为 OOM
                fatal_ = std::current_exception();
            }
        }

        template <typename E>
        [[nodiscard]]
        slot_t submit_one(E&& e) {
            F* fn = std::addressof(fn_); // 本对象不可移动 -> 地址稳定
            if constexpr (detail::carry_by_pointer_v<V>) {
                return pool_->submit([fn, p = std::addressof(e)] { return std::invoke(*fn, *p); });
            } else {
                return pool_->submit(
                    [fn, v = std::ranges::range_value_t<V>(std::forward<E>(e))]() mutable {
                        return std::invoke(*fn, std::move(v));
                    });
            }
        }

        /// 阻塞取回第 i 个结果. 提交期失败经 exception_ptr 承载 submit_error,
        /// 可用 concurrent::submit_error_of 还原
        [[nodiscard]]
        value_type fetch(std::size_t i) {
            if (i >= slots_.size()) {
                // 哨兵位仅在整批失败时可达(count 的定义); 兜底分支防御性保留
                return std::unexpected(
                    fatal_ ? fatal_ : std::make_exception_ptr(submit_error::out_of_memory));
            }
            auto& s = slots_[i];
            if (!s) {
                return std::unexpected(std::make_exception_ptr(s.error()));
            }
            try {
                return s->get();
            } catch (...) { // 结果类型的移动构造可能抛
                return std::unexpected(std::current_exception());
            }
        }

        Pool* pool_;
        V range_;
        F fn_;
        std::vector<slot_t> slots_;
        std::exception_ptr fatal_; ///< 整批性失败(提交期); 空 = 无
        bool launched_ = false;
        bool iter_began_ = false; ///< 单趟: begin 只发一次首元素迭代器
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

    /**
     * @brief 惰性并行遍历: 对区间每个元素并发调用 f(无返回值),
     *        产出 `std::expected<void, std::exception_ptr>` 以逐元素报错
     *
     * 索引区间可用 `std::views::iota(0, n)` 表达
     *
     * @warning 惰性: 通常应直接 `.run()`, 否则一个任务都不会提交
     */
    template <typename Pool, std::ranges::input_range R, typename F>
        requires std::invocable<F&, std::ranges::range_reference_t<R>> &&
                 std::is_void_v<std::invoke_result_t<F&, std::ranges::range_reference_t<R>>>
    [[nodiscard]]
    auto parallel_for(Pool& p, R&& range, F fn) {
        using view_t = std::views::all_t<R&&>;
        return parallel_view<Pool, view_t, F>{p, std::views::all(std::forward<R>(range)),
                                              std::move(fn)};
    }

} // namespace concurrent
