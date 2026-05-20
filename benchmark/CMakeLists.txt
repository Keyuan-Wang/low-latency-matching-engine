add_library(benchmark_runner_lib
	runner/benchmark_runner.cpp
)
target_include_directories(benchmark_runner_lib
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(benchmark_runner_lib PUBLIC matching_core)

# Benchmark executables — each contains its own IBenchScenario subclass
add_executable(bench_lmt_rest
	src/bench_lmt_rest.cpp
)
target_link_libraries(bench_lmt_rest PRIVATE benchmark_runner_lib)

add_executable(bench_lmt_cross_deep
	src/bench_lmt_cross_deep.cpp
)
target_link_libraries(bench_lmt_cross_deep PRIVATE benchmark_runner_lib)

add_executable(bench_mkt_sweep_deep
	src/bench_mkt_sweep_deep.cpp
)
target_link_libraries(bench_mkt_sweep_deep PRIVATE benchmark_runner_lib)

add_executable(bench_cxl_miss
	src/bench_cxl_miss.cpp
)
target_link_libraries(bench_cxl_miss PRIVATE benchmark_runner_lib)

add_executable(bench_dup_reject
	src/bench_dup_reject.cpp
)
target_link_libraries(bench_dup_reject PRIVATE benchmark_runner_lib)

add_executable(bench_cxl_hit
	src/bench_cxl_hit.cpp
)
target_link_libraries(bench_cxl_hit PRIVATE benchmark_runner_lib)

add_executable(bench_lmt_cross_shallow
	src/bench_lmt_cross_shallow.cpp
)
target_link_libraries(bench_lmt_cross_shallow PRIVATE benchmark_runner_lib)

add_executable(bench_overall
	src/bench_overall.cpp
)
target_link_libraries(bench_overall PRIVATE benchmark_runner_lib)

# Smoke test: validates runner infrastructure (ParseArgs, CSV output, etc.)
add_executable(benchmark_smoke_test
	tests/benchmark_smoke_test.cpp
)
target_link_libraries(benchmark_smoke_test PRIVATE benchmark_runner_lib)
