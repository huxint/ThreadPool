#pragma once
#include <atomic>
#include <cassert>
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

/**
 * @brief 线程池的任务类型
 * @details 线程池的任务类型可以是普通任务或优先级任务。
 *          普通任务是没有优先级的任务，优先级任务是有优先级的任务。
 *          任务的优先级可以是低、中、高三种等级。
 */
enum class priority_t : std::uint8_t { low = 0, normal = 1, high = 2 };
struct priority_task_t {
    priority_t priority;
    std::move_only_function<void()> task;

    explicit priority_task_t(priority_t priority_, std::move_only_function<void()> &&task_) noexcept(
        std::is_nothrow_move_constructible_v<std::move_only_function<void()>>)
    : priority(priority_),
      task(std::move(task_)) {}

    [[nodiscard]] friend bool operator<(const priority_task_t &lhs, const priority_task_t &rhs) noexcept {
        return lhs.priority < rhs.priority;
    }
};

/**
 * @brief 可取消任务的取消令牌
 * @details 用于检查任务是否被取消，以及请求取消任务。
 */
class cancellation_token {
public:
    cancellation_token()
    : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

    [[nodiscard]] bool is_cancelled() const noexcept {
        return cancelled_->load(std::memory_order_acquire);
    }

    void cancel() noexcept {
        cancelled_->store(true, std::memory_order_release);
    }

    void reset() noexcept {
        cancelled_->store(false, std::memory_order_release);
    }

private:
    std::shared_ptr<std::atomic<bool>> cancelled_;
};

/**
 * @brief 可取消任务的句柄
 * @details 包含 future 和取消令牌，用于获取结果和取消任务。
 */
template <typename T>
struct cancellable_task {
    std::future<T> future;
    cancellation_token token;

    void cancel() noexcept {
        token.cancel();
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return token.is_cancelled();
    }

    T get() {
        return future.get();
    }

    void wait() const {
        future.wait();
    }

    [[nodiscard]] bool valid() const noexcept {
        return future.valid();
    }
};

/**
 * @brief 线程池的操作类型掩码
 */
using opt_t = std::uint8_t;
enum op : opt_t {
    /**
     * @brief 操作掩码
     */
    none = 0,
    priority = 1 << 0,
    cancellable = 1 << 1
};

/**
 * @brief 线程池的操作类型
 * @details 线程池的操作类型可以是 `op::none`、`op::priority`、`op::cancellable` 或它们的组合。
 *          例如，`op::priority` 或者 `op::cancellable | op::priority`
 */
template <opt_t masks = op::none>
class ThreadPool {
    static constexpr bool priority_enabled = (masks & op::priority) != 0;
    static constexpr bool cancellable_enabled = (masks & op::cancellable) != 0;

public:
    explicit ThreadPool(std::size_t thread_count = std::jthread::hardware_concurrency())
    : active_tasks_(0),
      stopping_(false),
      thread_count_(std::max(thread_count, static_cast<std::size_t>(1))) {
        workers_.reserve(thread_count_);
        for (std::size_t i = 0; i < thread_count_; ++i) {
            workers_.emplace_back(&ThreadPool::worker, this);
        }
    }

    ~ThreadPool() noexcept {
        shutdown();
    }

    // 禁用拷贝和移动操作
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool(ThreadPool &&) = delete;
    ThreadPool &operator=(ThreadPool &&) = delete;

    // 向线程池提交一个任务
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(f), std::move(args)...);
            });
        auto result = task->get_future();
        enqueue(priority_t::normal /* 这只是占位符 */, [task]() {
            (*task)();
        });
        return result;
    }

    // 向线程池提交一个带优先级的任务（仅当 priority 启用时可用）
    template <typename F, typename... Args>
        requires priority_enabled
    auto submit(priority_t priority, F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(f), std::move(args)...);
            });
        auto result = task->get_future();
        enqueue(priority, [task]() {
            (*task)();
        });
        return result;
    }

    // 向线程池提交一个可取消的任务（仅当 cancellable 启用时可用）
    template <typename F, typename... Args>
        requires cancellable_enabled
    auto submit_cancellable(F &&f, Args &&...args)
        -> cancellable_task<std::invoke_result_t<F, const cancellation_token &, Args...>> {
        using return_type = std::invoke_result_t<F, const cancellation_token &, Args...>;
        cancellation_token token;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [f = std::forward<F>(f), token, ... args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(f), std::cref(token), std::move(args)...);
            });
        auto result = task->get_future();
        enqueue(priority_t::normal /* 这只是占位符 */, [task]() {
            (*task)();
        });
        return cancellable_task<return_type>{std::move(result), token};
    }

    // 向线程池提交一个带优先级的可取消任务（仅当 cancellable 和 priority 都启用时可用）
    template <typename F, typename... Args>
        requires(cancellable_enabled && priority_enabled)
    auto submit_cancellable(priority_t priority, F &&f, Args &&...args)
        -> cancellable_task<std::invoke_result_t<F, const cancellation_token &, Args...>> {
        using return_type = std::invoke_result_t<F, const cancellation_token &, Args...>;
        cancellation_token token;
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [f = std::forward<F>(f), token, ... args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(f), std::cref(token), std::move(args)...);
            });
        auto result = task->get_future();
        enqueue(priority, [task]() {
            (*task)();
        });
        return cancellable_task<return_type>{std::move(result), token};
    }

    // 获取线程池中的线程数量
    [[nodiscard]] std::size_t thread_count() const noexcept {
        return thread_count_;
    }

    // 等待所有任务执行完毕(不可提交到当前线程池中)
    void wait() {
        std::unique_lock lock(tasks_mutex_);
        idle_condition_.wait(lock, [this] {
            return tasks_.empty() && active_tasks_.load() == 0;
        });
    }

    // 重启线程池(不可提交到当前线程池中)
    void launch() {
        std::scoped_lock lock(lifecycle_mutex_);
        shutdown();
        active_tasks_.store(0);
        stopping_.store(false, std::memory_order_release);
        workers_.reserve(thread_count_);
        for (std::size_t i = 0; i < thread_count_; ++i) {
            workers_.emplace_back(&ThreadPool::worker, this);
        }
    }

    // 关闭线程池(不可提交到当前线程池中)，等待所有已提交任务执行完毕
    void shutdown() noexcept {
        std::scoped_lock lock(lifecycle_mutex_);
        stopping_.store(true, std::memory_order_release);
        wait();
        // request_stop 会让 stop_token 生效
        for (auto &worker : workers_) {
            worker.request_stop();
        }
        tasks_condition_.notify_all();
        workers_.clear(); // jthread 析构自动 join
    }

    // 判断线程池是否运行
    [[nodiscard]] bool running() const noexcept {
        return !workers_.empty() && !stopping_.load(std::memory_order_acquire) &&
               !workers_.front().get_stop_token().stop_requested();
    }

private:
    // 将任务加入队列的公共逻辑
    void enqueue(priority_t priority, std::move_only_function<void()> &&task_wrapper) {
        {
            std::scoped_lock lock(tasks_mutex_);
            if (stopping_.load(std::memory_order_acquire)) {
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

                // 等待任务队列不为空或收到停止请求
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
            // 执行任务
            task();
            {
                std::scoped_lock lock(tasks_mutex_);
                --active_tasks_;
                if (tasks_.empty() && active_tasks_.load() == 0) {
                    idle_condition_.notify_all();
                }
            }
        }
    } // 工作线程主循环

    std::recursive_mutex lifecycle_mutex_;    // 生命周期操作的互斥锁
    mutable std::mutex tasks_mutex_;          // 任务队列的互斥锁
    std::condition_variable tasks_condition_; // 任务队列的条件变量
    std::condition_variable idle_condition_;  // 空闲等待的条件变量
    std::vector<std::jthread> workers_;       // 线程池中的线程
    std::conditional_t<priority_enabled,
                       std::priority_queue<priority_task_t>,
                       std::queue<std::move_only_function<void()>>>
        tasks_;                               // 任务队列
    std::atomic<std::uint64_t> active_tasks_; // 正在执行的任务数
    std::size_t thread_count_;                // 线程池中的线程数量
    std::atomic<bool> stopping_;              // 是否正在停止
};