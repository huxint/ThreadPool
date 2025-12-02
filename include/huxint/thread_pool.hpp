#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

enum class TaskPriority : std::uint8_t { High = 0, Normal = 1, Low = 2 };

class ThreadPool {
    // 任务类型：(优先级, 序列号, 任务函数)
    struct PriorityTask {
        TaskPriority priority;
        std::uint64_t sequence;
        std::move_only_function<void()> task;

        // 优先级值越小优先级越高，同优先级按序列号排序
        bool operator>(const PriorityTask &other) const noexcept {
            if (priority != other.priority) {
                return priority > other.priority;
            }
            return sequence > other.sequence;
        }
    };

public:
    explicit ThreadPool(std::size_t thread_count = std::thread::hardware_concurrency());
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
    auto submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>>;

    // 向线程池提交一个任务
    template <typename F, typename... Args>
    auto submit(TaskPriority priority, F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>>;

    // 获取线程池中的线程数量
    std::size_t thread_count() const noexcept {
        return workers_.size();
    }

    // 获取队列中待处理任务的数量
    std::size_t pending_task_count() const noexcept {
        std::scoped_lock lock(tasks_mutex_);
        return tasks_.size();
    }

    void shutdown() noexcept;

    // 判断线程池是否运行
    bool running() const noexcept {
        return running_;
    }

private:
    void worker_loop();                                                                  // 工作线程主循环
    mutable std::mutex tasks_mutex_;                                                     // 任务队列的互斥锁
    std::condition_variable tasks_condition_;                                            // 任务队列的条件变量
    std::vector<std::thread> workers_;                                                   // 线程池中的线程
    std::priority_queue<PriorityTask, std::vector<PriorityTask>, std::greater<>> tasks_; // 任务队列
    std::uint64_t task_sequence_;                                                        // 任务序列号
    std::atomic<bool> running_;                                                          // 线程池是否运行
};

inline ThreadPool::ThreadPool(std::size_t thread_count)
: task_sequence_(0),
  running_(true) {
    thread_count = std::max(thread_count, static_cast<std::size_t>(1)); // 至少有一个线程
    workers_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
        workers_.emplace_back(&ThreadPool::worker_loop, this);
    }
}

inline void ThreadPool::shutdown() noexcept {
    if (!running_.exchange(false)) {
        return; // 已经关闭过了
    }
    // 通知所有等待的线程来执行任务
    tasks_condition_.notify_all();
    // 等待所有线程执行完毕
    for (std::thread &worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

template <typename F, typename... Args>
auto ThreadPool::submit(F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    return submit(TaskPriority::Normal, std::forward<F>(f), std::forward<Args>(args)...);
}

template <typename F, typename... Args>
auto ThreadPool::submit(TaskPriority priority, F &&f, Args &&...args) -> std::future<std::invoke_result_t<F, Args...>> {
    using return_type = std::invoke_result_t<F, Args...>;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
        [f = std::forward<F>(f), ... args = std::forward<Args>(args)]() mutable {
            return std::invoke(std::move(f), std::move(args)...);
        });

    std::future<return_type> result = task->get_future();

    {
        std::scoped_lock lock(tasks_mutex_);
        if (!running()) {
            throw std::runtime_error("submit task to stopped thread pool");
        }
        tasks_.emplace(priority, task_sequence_++, [task]() {
            (*task)();
        });
    }

    // 通知一个等待的线程来执行任务
    tasks_condition_.notify_one();
    return result;
}

inline void ThreadPool::worker_loop() {
    while (true) {
        std::move_only_function<void()> task;
        {
            std::unique_lock lock(tasks_mutex_);

            // 等待任务队列不为空或线程池停止运行
            tasks_condition_.wait(lock, [this] {
                return !tasks_.empty() || !running();
            });

            if (!running() && tasks_.empty()) {
                return;
            }

            // 从任务队列中取出一个任务
            // top() 返回 const&，但我们即将 pop，所以 move 是安全的
            task = std::move(const_cast<PriorityTask &>(tasks_.top()).task);
            tasks_.pop();
        }
        // 执行任务
        task();
    }
}