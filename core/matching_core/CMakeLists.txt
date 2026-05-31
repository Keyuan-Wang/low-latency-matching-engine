add_library(matching_core
  src/order_book.cpp
  src/price_level.cpp
  src/chunk_pool.cpp
  src/order_pool.cpp
)

target_include_directories(matching_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Try system package first; fall back to FetchContent (e.g. on WSL without libabsl-dev).
find_package(absl QUIET)
if(NOT absl_FOUND)
  include(FetchContent)
  FetchContent_Declare(abseil
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG 20260107.1
  )
  set(ABSL_PROPAGATE_CXX_STD ON)
  FetchContent_MakeAvailable(abseil)
endif()
target_link_libraries(matching_core PUBLIC absl::flat_hash_map)

if(LLMES_BUILD_TESTS)
  add_executable(matching_core_tests
    tests/order_book_test.cpp
  )
  target_link_libraries(matching_core_tests PRIVATE matching_core)
  add_test(NAME matching_core_tests COMMAND matching_core_tests)
endif()
