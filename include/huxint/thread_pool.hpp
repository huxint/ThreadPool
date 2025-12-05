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
 * @brief 线程池的操作类型掩码
 */
using opt_t = std::uint8_t;
enum op : opt_t {
    /**
     * @brief 无操作
     */
    none = 0,

    /**
     * @brief 支持任务优先级
     */
    priority = 1 << 0
};

/**
 * @brief 线程池的操作类型
 * @details 线程池的操作类型可以是 `op::none`、`op::priority` 或它们的组合。(后续可能加上其他操作)
 *          例如，`op::priority` 或者 `op::none | op::priority` 表示线程池支持任务优先级。
 */
template <opt_t masks = op::none>
class ThreadPool {
    static constexpr bool priority_enabled = (masks & op::priority) != 0;

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
    // 默认优先级为 Normal
    template <typename F, typename... Args>
    auto submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
        return submit(priority_t::normal, std::forward<F>(f), std::forward<Args>(args)...);
    }

    // 向线程池提交一个任务
    template <typename F, typename... Args>
    auto submit(priority_t priority, F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
                return std::invoke(std::move(f), std::move(args)...);
            });

        std::future<return_type> result = task->get_future();

        {
            std::scoped_lock lock(tasks_mutex_);
            if (stopping_.load(std::memory_order_acquire)) {
                throw std::runtime_error("submit task to stopped thread pool");
            }

            if constexpr (priority_enabled) {
                tasks_.emplace(priority, [task]() {
                    (*task)();
                });
            } else {
                tasks_.emplace([task]() {
                    (*task)();
                });
            }
        }

        // 通知一个等待的线程来执行任务
        tasks_condition_.notify_one();
        return result;
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