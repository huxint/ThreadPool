cmake_minimum_required(VERSION 3.28)
project(ThreadPool CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
     "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json" SYMBOLIC)

find_package(Threads REQUIRED)
include(CTest)

# -DSANITIZER=address|thread, UBSan always layered on top
set(SANITIZER "" CACHE STRING "sanitizer: address or thread")
if(SANITIZER)
    add_compile_options(-fsanitize=${SANITIZER},undefined -fno-omit-frame-pointer -g
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${SANITIZER},undefined)
endif()

# Header-only library. 契约语法写在公共头里, 消费方 TU 须以 -fcontracts 才能解析:
# 经 INTERFACE 注入但仅限 GCC - Clang 无此 flag, 之前被一刀切强制, 完全无法
# 消费; 契约求值语义不再随之强加, 交由下游自选
add_library(concurrent INTERFACE)
target_include_directories(concurrent INTERFACE include)
target_compile_features(concurrent INTERFACE cxx_std_26)
target_link_libraries(concurrent INTERFACE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(concurrent INTERFACE -fcontracts)
    target_link_options(concurrent INTERFACE -fcontracts)
endif()

# 自有目标的编译面: 警告(收窄到 PRIVATE, 不再全局 add_compile_options,
# 不波及 taskflow/doctest 等第三方头) + 契约求值语义(自有目标 Debug 下
# enforce, 其余 ignore; 不挂 INTERFACE, 不随之下发)
function(concurrent_own_compile_options tgt)
    target_compile_options(
        ${tgt} PRIVATE -Wall -Wextra -Wshadow
        $<IF:$<CONFIG:Debug>,-fcontract-evaluation-semantic=enforce,-fcontract-evaluation-semantic=ignore>)
endfunction()

add_executable(concurrent_example src/main.cpp)
target_link_libraries(concurrent_example PRIVATE concurrent)
concurrent_own_compile_options(concurrent_example)

add_executable(concurrent_tests tests/main.cpp tests/pool.cpp tests/task.cpp tests/parallel.cpp
                                tests/detail.cpp)
target_link_libraries(concurrent_tests PRIVATE concurrent)
target_include_directories(concurrent_tests PRIVATE tests tests/third_party)
concurrent_own_compile_options(concurrent_tests)
add_test(NAME concurrent_tests COMMAND concurrent_tests)

add_executable(concurrent_bench benchmarks/bench.cpp)
target_link_libraries(concurrent_bench PRIVATE concurrent)
target_include_directories(concurrent_bench PRIVATE benchmarks benchmarks/third_party/taskflow_repo)
concurrent_own_compile_options(concurrent_bench)

# -DBUILD_MODULE=ON: module wrapper `concurrent.pool` (requires Ninja)
option(BUILD_MODULE "build the concurrent.pool module wrapper" OFF)
if(BUILD_MODULE)
    add_library(concurrent_module STATIC)
    target_sources(concurrent_module PUBLIC FILE_SET CXX_MODULES FILES module/concurrent.cppm)
    target_link_libraries(concurrent_module PUBLIC concurrent)
    set_target_properties(concurrent_module PROPERTIES CXX_SCAN_FOR_MODULES ON)
    concurrent_own_compile_options(concurrent_module)

    add_executable(concurrent_module_smoke module/smoke.cpp)
    target_link_libraries(concurrent_module_smoke PRIVATE concurrent_module)
    set_target_properties(concurrent_module_smoke PROPERTIES CXX_SCAN_FOR_MODULES ON)
    concurrent_own_compile_options(concurrent_module_smoke)
    add_test(NAME concurrent_module_smoke COMMAND concurrent_module_smoke)
endif()

# -DWITH_STDEXEC=ON -DSTDEXEC_ROOT=<含 stdexec/ 的目录>: P2300 scheduler 集成
# 可选依赖, 不进核心构建; 核心库保持零依赖 header-only
option(WITH_STDEXEC "build stdexec (P2300) scheduler integration tests" OFF)
if(WITH_STDEXEC)
    if(NOT STDEXEC_ROOT)
        message(FATAL_ERROR "WITH_STDEXEC=ON requires -DSTDEXEC_ROOT=<include root containing stdexec/>")
    endif()
    add_executable(concurrent_stdexec_tests tests/stdexec.cpp)
    target_link_libraries(concurrent_stdexec_tests PRIVATE concurrent)
    # SYSTEM: 第三方头告警不属于本库承诺的零警告面
    target_include_directories(concurrent_stdexec_tests SYSTEM PRIVATE ${STDEXEC_ROOT})
    target_include_directories(concurrent_stdexec_tests PRIVATE tests tests/third_party)
    concurrent_own_compile_options(concurrent_stdexec_tests)
    # 关闭模块依赖扫描: 扫描注入的 -fmodules-ts 与 stdexec 头在 GCC 16 上
    # 触发编译器内部错误(本目标无模块单元, 扫描本无必要)
    set_target_properties(concurrent_stdexec_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    add_test(NAME concurrent_stdexec_tests COMMAND concurrent_stdexec_tests)
endif()

if(SANITIZER STREQUAL "thread")
    set_tests_properties(concurrent_tests PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
endif()
