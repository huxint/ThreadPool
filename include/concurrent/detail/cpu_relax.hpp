#pragma once

#if !(defined(__x86_64__) || defined(__i386__) || defined(__aarch64__) || defined(__arm__) ||     \
      defined(__powerpc__))
#include <thread> // 仅兜底分支需要
#endif

namespace concurrent::detail {

    /// 自旋等待中的一次"让核"提示: 在超线程上把流水线资源让给兄弟逻辑核,
    /// 并降低退出自旋时的内存序错误推测惩罚. 非阻塞, 不进内核
    inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#elif defined(__powerpc__)
        __asm__ __volatile__("or 27,27,27" ::: "memory");
#else
        std::this_thread::yield(); // 兜底: 语义偏重, 但保证可移植
#endif
    }

} // namespace concurrent::detail
