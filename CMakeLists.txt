cmake_minimum_required(VERSION 3.28)
project(ThreadPool CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
     "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json" SYMBOLIC)

find_package(Threads REQUIRED)
include(CTest)

add_compile_options(-Wall -Wextra -Wshadow)

# -DSANITIZER=address|thread, UBSan always layered on top
set(SANITIZER "" CACHE STRING "sanitizer: address or thread")
if(SANITIZER)
    add_compile_options(-fsanitize=${SANITIZER},undefined -fno-omit-frame-pointer -g
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${SANITIZER},undefined)
endif()

# Header-only library. Contracts enforce in Debug, ignore otherwise.
add_library(concurrent INTERFACE)
target_include_directories(concurrent INTERFACE include)
target_compile_features(concurrent INTERFACE cxx_std_26)
target_link_libraries(concurrent INTERFACE Threads::Threads)
target_compile_options(
    concurrent INTERFACE
    -fcontracts $<IF:$<CONFIG:Debug>,-fcontract-evaluation-semantic=enforce,-fcontract-evaluation-semantic=ignore>)
target_link_options(concurrent INTERFACE -fcontracts)

add_executable(concurrent_example src/main.cpp)
target_link_libraries(concurrent_example PRIVATE concurrent)

add_executable(concurrent_tests tests/main.cpp tests/pool.cpp tests/task.cpp tests/parallel.cpp
                                tests/detail.cpp)
target_link_libraries(concurrent_tests PRIVATE concurrent)
target_include_directories(concurrent_tests PRIVATE tests tests/third_party)
add_test(NAME concurrent_tests COMMAND concurrent_tests)

add_executable(concurrent_bench benchmarks/bench.cpp)
target_link_libraries(concurrent_bench PRIVATE concurrent)
target_include_directories(concurrent_bench PRIVATE benchmarks benchmarks/third_party/taskflow_repo)

# -DBUILD_MODULE=ON: module wrapper `concurrent.pool` (requires Ninja)
option(BUILD_MODULE "build the concurrent.pool module wrapper" OFF)
if(BUILD_MODULE)
    add_library(concurrent_module STATIC)
    target_sources(concurrent_module PUBLIC FILE_SET CXX_MODULES FILES module/concurrent.cppm)
    target_link_libraries(concurrent_module PUBLIC concurrent)
    set_target_properties(concurrent_module PROPERTIES CXX_SCAN_FOR_MODULES ON)

    add_executable(concurrent_module_smoke module/smoke.cpp)
    target_link_libraries(concurrent_module_smoke PRIVATE concurrent_module)
    set_target_properties(concurrent_module_smoke PROPERTIES CXX_SCAN_FOR_MODULES ON)
    add_test(NAME concurrent_module_smoke COMMAND concurrent_module_smoke)
endif()

if(SANITIZER STREQUAL "thread")
    set_tests_properties(concurrent_tests PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
endif()
