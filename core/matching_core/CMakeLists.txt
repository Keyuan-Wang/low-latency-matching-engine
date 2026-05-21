add_library(matching_core
  src/order_book.cpp
  src/order_pool.cpp
  src/flat_hash_map.cpp
)

target_include_directories(matching_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if(LLMES_BUILD_TESTS)
  add_executable(matching_core_tests
    tests/order_book_test.cpp
  )
  target_link_libraries(matching_core_tests PRIVATE matching_core)
  add_test(NAME matching_core_tests COMMAND matching_core_tests)
endif()