add_library(benchmark_runner_lib
	src/benchmark_runner.cpp
)
target_include_directories(benchmark_runner_lib
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(benchmark_runner_lib PUBLIC matching_core)

# --- HFT macro benchmarks ---
add_executable(bench_hft_macro
	src/hft/bench_hft_macro.cpp
)
target_link_libraries(bench_hft_macro PRIVATE benchmark_runner_lib)

add_executable(bench_hft_macro_scenarios
	src/hft/bench_hft_macro_scenarios.cpp
)
target_link_libraries(bench_hft_macro_scenarios PRIVATE benchmark_runner_lib)

if(LLMES_BUILD_TESTS)
	add_executable(benchmark_smoke_test
		tests/benchmark_smoke_test.cpp
	)
	target_link_libraries(benchmark_smoke_test PRIVATE benchmark_runner_lib)
	add_test(NAME benchmark_smoke_test COMMAND benchmark_smoke_test)
endif()
