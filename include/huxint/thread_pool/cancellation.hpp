#pragma once
#include <atomic>
#include <future>
#include <memory>

namespace thread_pool {
namespace cancellation {
/**
 * @brief 可取消任务的取消令牌
 * @details 用于检查任务是否被取消，以及请求取消任务
 */
class token {
public:
    token()
    : cancelled_(std::make_shared<std::atomic<bool>>(false)) {}

    [[nodiscard]]
    bool cancelled() const noexcept {
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
 * @details 包含 future 和取消令牌，用于获取结果和取消任务
 */
template <typename T>
struct task {
    std::future<T> future;
    token token;

    void cancel() noexcept {
        token.cancel();
    }

    [[nodiscard]]
    bool cancelled() const noexcept {
        return token.cancelled();
    }

    T get() {
        return future.get();
    }

    void wait() const {
        future.wait();
    }

    [[nodiscard]]
    bool valid() const noexcept {
        return future.valid();
    }
};
} // namespace cancellation
/// 取消令牌的常量引用类型别名
using token_ref = const cancellation::token &;
} // namespace thread_pool