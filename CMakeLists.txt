cmake_minimum_required(VERSION 3.25)

project(ThreadPool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -fcontracts)
add_link_options(-fcontracts)

add_executable(ThreadPool src/main.cpp)
target_include_directories(ThreadPool PRIVATE include)
