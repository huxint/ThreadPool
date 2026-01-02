#pragma once
#include "priority.hpp"
#include "cancellation.hpp"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <vector>

namespace thread_pool {
/**
 * @brief 线程池的操作类型掩码
 */
enum op : std::uint8_t {
    none = 0,
    priority = 1 << 0,
    cancellable = 1 << 1
};

/**
 * @brief 线程池
 * @tparam masks 操作类型掩码，可组合 op::priority 和 op::cancellable
 */
template <std::uint8_t masks = op::none>
class ThreadPool {
    static constexpr bool priority_enabled = (masks & op::priority) != 0;
    static constexpr bool cancellable_enabled = (masks & op::cancellable) != 0;

public:
    explicit ThreadPool(std::size_t thread_size = std::jthread::hardware_concurrency())
    : active_tasks_(0) {
        stopping_.clear();
        thread_size_ = std::max<std::size_t>(thread_size, 1);
        spawn();
    }

    ~ThreadPool() noexcept {
        shutdown();
    }

    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    /// 执行任务（fire-and-forget，无返回值，避免 shared_ptr 开销）
    template <typename F, typename... Args>
    void execute(F &&f, Args &&...args) {
        enqueue(priority_t::normal, [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
            std::invoke(std::move(f), std::move(args)...);
        });
    }

    /// 执行带优先级的任务（fire-and-forget）
    template <typename F, typename... Args>
        requires priority_enabled
    void execute(priority_t priority, F &&f, Args &&...args) {
        enqueue(priority, [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
            std::invoke(std::move(f), std::move(args)...);
        });
    }

    /// 执行可取消的任务（fire-and-forget，返回 token 用于取消）
    template <typename F, typename... Args>
        requires cancellable_enabled
    cancellation::token execute(F &&f, Args &&...args) {
        cancellation::token token;
        enqueue(priority_t::normal, [f = std::forward<F>(f), token, ... args = std::forward<Args>(args)]() mutable {
            std::invoke(std::move(f), std::cref(token), std::move(args)...);
        });
        return token;
    }

    /// 执行带优先级的可取消任务（fire-and-forget）
    template <typename F, typename... Args>
        requires(cancellable_enabled && priority_enabled)
    cancellation::token execute(priority_t priority, F &&f, Args &&...args) {
        cancellation::token token;
        enqueue(priority, [f = std::forward<F>(f), token, ... args = std::forward<Args>(args)]() mutable {
            std::invoke(std::move(f), std::cref(token), std::move(args)...);
        });
        return token;
    }

    /// 提交普通任务
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<return_type()>>([f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::move(args)...);
        });
        auto result = task->get_future();
        enqueue(priority_t::normal, [task]() {
            (*task)();
        });
        return result;
    }

    /// 提交带优先级的任务
    template <typename F, typename... Args>
        requires priority_enabled
    auto submit(priority_t priority, F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<return_type()>>([f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::move(args)...);
        });
        auto result = task->get_future();
        enqueue(priority, [task]() {
            (*task)();
        });
        return result;
    }

    /// 提交可取消的任务
    template <typename F, typename... Args>
        requires cancellable_enabled
    auto submit(F &&f, Args &&...args) -> cancellation::task<std::invoke_result_t<F, const cancellation::token &, Args...>> {
        using return_type = std::invoke_result_t<F, const cancellation::token &, Args...>;
        cancellation::token token;
        auto task = std::make_shared<std::packaged_task<return_type()>>([f = std::forward<F>(f), token, ... args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::cref(token), std::move(args)...);
        });
        auto result = task->get_future();
        enqueue(priority_t::normal, [task]() {
            (*task)();
        });
        return cancellation::task<return_type>{std::move(result), token};
    }

    /// 提交带优先级的可取消任务
    template <typename F, typename... Args>
        requires(cancellable_enabled && priority_enabled)
    auto submit(priority_t priority, F &&f, Args &&...args) -> cancellation::task<std::invoke_result_t<F, const cancellation::token &, Args...>> {
        using return_type = std::invoke_result_t<F, const cancellation::token &, Args...>;
        cancellation::token token;
        auto task = std::make_shared<std::packaged_task<return_type()>>([f = std::forward<F>(f), token, ... args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::cref(token), std::move(args)...);
        });
        auto result = task->get_future();
        enqueue(priority, [task]() {
            (*task)();
        });
        return cancellation::task<return_type>{std::move(result), token};
    }

    [[nodiscard]]
    std::size_t thread_size() const noexcept {
        return thread_size_;
    }

    void wait() {
        std::unique_lock lock(tasks_mutex_);
        idle_condition_.wait(lock, [this] {
            return tasks_.empty() && active_tasks_.load() == 0;
        });
    }

    void launch() {
        std::scoped_lock lock(lifecycle_mutex_);
        shutdown();
        active_tasks_.store(0);
        stopping_.clear(std::memory_order_release);
        spawn();
    }

    void shutdown() noexcept {
        std::scoped_lock lock(lifecycle_mutex_);
        stopping_.test_and_set(std::memory_order_release);
        wait();
        for (auto &worker : workers_) {
            worker.request_stop();
        }
        tasks_condition_.notify_all();
        workers_.clear();
    }

    [[nodiscard]]
    bool running() const noexcept {
        return !workers_.empty() && !stopping_.test(std::memory_order_acquire) && !workers_.front().get_stop_token().stop_requested();
    }

private:
    void spawn() {
        workers_.reserve(thread_size_);
        for (std::size_t i = 0; i < thread_size_; ++i) {
            workers_.emplace_back([this](std::stop_token st) {
                worker(st);
            });
        }
    }

    void enqueue(priority_t priority, std::move_only_function<void()> &&task_wrapper) {
        {
            std::scoped_lock lock(tasks_mutex_);
            if (stopping_.test(std::memory_order_acquire)) {
                throw std::runtime_error("submit task to stopped thread pool");
            }
            if constexpr (priority_enabled) {
                tasks_.emplace(priority, std::move(task_wrapper));
            } else {
                tasks_.emplace(std::move(task_wrapper));
            }
        }
        tasks_condition_.notify_one();
    }

    void worker(std::stop_token stop_token) {
        while (true) {
            std::move_only_function<void()> task;
            {
                std::unique_lock lock(tasks_mutex_);
                tasks_condition_.wait(lock, [this, &stop_token] {
                    return stop_token.stop_requested() || !tasks_.empty();
                });

                if (stop_token.stop_requested() && tasks_.empty()) {
                    return;
                }

                if constexpr (priority_enabled) {
                    task = std::move(const_cast<priority_task_t &>(tasks_.top()).task);
                } else {
                    task = std::move(tasks_.front());
                }
                tasks_.pop();
                ++active_tasks_;
            }
            task();
            {
                std::scoped_lock lock(tasks_mutex_);
                --active_tasks_;
                if (tasks_.empty() && active_tasks_.load() == 0) {
                    idle_condition_.notify_all();
                }
            }
        }
    }

    std::recursive_mutex lifecycle_mutex_;
    mutable std::mutex tasks_mutex_;
    std::condition_variable tasks_condition_;
    std::condition_variable idle_condition_;
    std::vector<std::jthread> workers_;
    std::conditional_t<priority_enabled, std::priority_queue<priority_task_t>, std::queue<std::move_only_function<void()>>> tasks_;
    std::atomic<std::uint64_t> active_tasks_;
    std::size_t thread_size_;
    std::atomic_flag stopping_;
};
} // namespace thread_pool