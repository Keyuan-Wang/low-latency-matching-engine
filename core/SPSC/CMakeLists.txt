add_library(spsc INTERFACE)

target_include_directories(spsc INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(LLMES_BUILD_TESTS)
  add_executable(spsc_tests test.cpp)
  target_link_libraries(spsc_tests PRIVATE spsc)

  find_package(Threads REQUIRED)
  target_link_libraries(spsc_tests PRIVATE Threads::Threads)

  add_test(NAME spsc_tests COMMAND spsc_tests mutex 1000)
endif()
