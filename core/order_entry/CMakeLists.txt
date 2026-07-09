add_library(order_entry INTERFACE)
target_include_directories(order_entry INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_executable(order_entry_blocking_server examples/blocking_server.cpp)
target_link_libraries(order_entry_blocking_server PRIVATE order_entry)

add_executable(order_entry_blocking_client examples/blocking_client.cpp)
target_link_libraries(order_entry_blocking_client PRIVATE order_entry)

add_executable(order_entry_echo_bench_server examples/blocking_echo_bench_server.cpp)
target_link_libraries(order_entry_echo_bench_server PRIVATE order_entry)

add_executable(order_entry_echo_bench_client examples/blocking_echo_bench_client.cpp)
target_link_libraries(order_entry_echo_bench_client PRIVATE order_entry)

add_executable(order_entry_epoll_server examples/epoll_server.cpp)
target_link_libraries(order_entry_epoll_server PRIVATE order_entry spsc)

if(LLMES_BUILD_TESTS)
  add_executable(order_entry_tests tests/test.cpp)
  target_link_libraries(order_entry_tests PRIVATE order_entry)
  add_test(NAME order_entry_tests COMMAND order_entry_tests)
endif()
