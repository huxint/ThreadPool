#pragma once
// 契约断言的可用性门控: 契约语法写在公共头里, 但下游(如 Clang, 或未开
// -fcontracts 的 GCC)可能不支持 - GCC 在无该 flag 时语法虽可过编译,
// 链接期却缺 handle_contract_violation 运行时. 仅在消费方开启
// -fcontracts(__cpp_contracts)时编译为真断言, 否则退化为空表达式,
// 不把可用性前提强加给下游(与 CMake 侧的 GNU 门控配套)

#if defined(__cpp_contracts)
#define CONCURRENT_CONTRACT_ASSERT(cond) contract_assert(cond)
#else
#define CONCURRENT_CONTRACT_ASSERT(cond) static_cast<void>(0)
#endif
