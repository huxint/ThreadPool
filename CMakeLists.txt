cmake_minimum_required(VERSION 3.14)

project(ThreadPool LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(ThreadPool src/main.cpp)
target_link_libraries(ThreadPool stdc++exp)
target_include_directories(ThreadPool PRIVATE include)