cmake_minimum_required(VERSION 3.20)
project(llmes LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(LLMES_BUILD_TESTS "Build tests" ON)
option(LLMES_BUILD_BENCHMARKS "Build benchmarks" ON)

if(LLMES_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(core/SPSC)
add_subdirectory(core/matching_core)
add_subdirectory(core/order_entry)

if(LLMES_BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()