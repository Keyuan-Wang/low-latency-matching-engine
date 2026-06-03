add_library(benchmark_runner_lib
	src/benchmark_runner.cpp
)
target_include_directories(benchmark_runner_lib
	PUBLIC
		${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(benchmark_runner_lib PUBLIC matching_core)

# --- Legacy benchmarks (Phase 1–2, preserved for baseline comparison) ---
add_executable(bench_lmt_rest
	src/legacy/bench_lmt_rest.cpp
)
target_link_libraries(bench_lmt_rest PRIVATE benchmark_runner_lib)

add_executable(bench_lmt_cross_deep
	src/legacy/bench_lmt_cross_deep.cpp
)
target_link_libraries(bench_lmt_cross_deep PRIVATE benchmark_runner_lib)

add_executable(bench_mkt_sweep_deep
	src/legacy/bench_mkt_sweep_deep.cpp
)
target_link_libraries(bench_mkt_sweep_deep PRIVATE benchmark_runner_lib)



add_executable(bench_cxl_hit
	src/legacy/bench_cxl_hit.cpp
)
target_link_libraries(bench_cxl_hit PRIVATE benchmark_runner_lib)

add_executable(bench_lmt_cross_shallow
	src/legacy/bench_lmt_cross_shallow.cpp
)
target_link_libraries(bench_lmt_cross_shallow PRIVATE benchmark_runner_lib)

add_executable(bench_overall
	src/legacy/bench_overall.cpp
)
target_link_libraries(bench_overall PRIVATE benchmark_runner_lib)

# --- HFT micro benchmarks (Phase 3) ---
add_executable(bench_hft_add_near
	src/hft/bench_hft_add_near.cpp
)
target_link_libraries(bench_hft_add_near PRIVATE benchmark_runner_lib)

add_executable(bench_hft_add_far
	src/hft/bench_hft_add_far.cpp
)
target_link_libraries(bench_hft_add_far PRIVATE benchmark_runner_lib)

add_executable(bench_hft_cancel_hot
	src/hft/bench_hft_cancel_hot.cpp
)
target_link_libraries(bench_hft_cancel_hot PRIVATE benchmark_runner_lib)

add_executable(bench_hft_cancel_cold
	src/hft/bench_hft_cancel_cold.cpp
)
target_link_libraries(bench_hft_cancel_cold PRIVATE benchmark_runner_lib)

add_executable(bench_hft_modify_near
	src/hft/bench_hft_modify_near.cpp
)
target_link_libraries(bench_hft_modify_near PRIVATE benchmark_runner_lib)


add_executable(bench_hft_market_small
	src/hft/bench_hft_market_small.cpp
)
target_link_libraries(bench_hft_market_small PRIVATE benchmark_runner_lib)

add_executable(bench_hft_market_large
	src/hft/bench_hft_market_large.cpp
)
target_link_libraries(bench_hft_market_large PRIVATE benchmark_runner_lib)

add_executable(bench_hft_macro
	src/hft/bench_hft_macro.cpp
)
target_link_libraries(bench_hft_macro PRIVATE benchmark_runner_lib)

# --- Smoke test ---
add_executable(benchmark_smoke_test
	tests/benchmark_smoke_test.cpp
)
target_link_libraries(benchmark_smoke_test PRIVATE benchmark_runner_lib)
