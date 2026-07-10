/**
	* @file benchmark_smoke_test.cpp
	* @brief Lightweight correctness tests for the benchmark runner infrastructure.
	*
	* Validates:
	*   - EnsureCsvHeader(): writes header only when file is new or empty.
	*   - Percentile(): correct interpolation for p0/p50/p100 and empty input.
	*   - PrefillSellBook(): book is populated and orders can be swept.
	*   - RunScenario(): full latency run with a trivial scenario, CSV output
	*     format, and exit code.
	*
	* Does not require hardware PMC support or root privileges.
	*/

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;

/** @brief Assert a condition; print FAIL on failure. */
void check(bool cond, const char* msg) {
	if (!cond) {
		std::cerr << "FAIL: " << msg << "\n";
		++failures;
	}
}

/** @brief Test EnsureCsvHeader idempotency. */
void test_ensure_csv_header() {
	const char* path = "/tmp/bench_smoke_test_header.csv";
	std::remove(path);

	benchmark_runner::EnsureCsvHeader(path, "col1,col2,col3");
	{
		std::ifstream f(path);
		std::string line;
		bool got = static_cast<bool>(std::getline(f, line));
		check(got, "header file created");
		check(line == "col1,col2,col3", "header content matches");
	}

	// Second call on an existing non-empty file must be a no-op.
	benchmark_runner::EnsureCsvHeader(path, "col1,col2,col3");
	{
		std::ifstream f(path);
		std::string line;
		bool got = static_cast<bool>(std::getline(f, line));
		check(got, "header still readable");
		check(line == "col1,col2,col3", "header not duplicated");
		std::string extra;
		bool extra_got = static_cast<bool>(std::getline(f, extra));
		check(!extra_got, "no extra lines");
	}

	std::remove(path);
	std::cout << "  EnsureCsvHeader: OK\n";
}

/** @brief Test Percentile calculation. */
void test_percentile() {
	std::vector<double> v = {10.0, 20.0, 30.0, 40.0, 50.0};
	check(benchmark_runner::Percentile(v, 0.50) == 30.0, "p50 correct");
	check(benchmark_runner::Percentile(v, 0.0) == 10.0, "p0 correct");
	check(benchmark_runner::Percentile(v, 1.0) == 50.0, "p100 correct");

	std::vector<double> empty;
	check(benchmark_runner::Percentile(empty, 0.5) == 0.0, "empty returns 0");
	std::cout << "  Percentile: OK\n";
}

/** @brief Test PrefillSellBook populates the book correctly. */
void test_prefill_sell_book() {
	matching::OrderBook book(210);
	benchmark_runner::PrefillSellBook(book, 100, 10, 1000);
	const auto r = book.add_market_order(9999, matching::Side::Buy, 200, 9999);
	check(r.filled_quantity > 0, "prefilled orders exist and can be swept");
	std::cout << "  PrefillSellBook: OK\n";
}

/** @brief End-to-end test: run a trivial scenario through the full harness. */
void test_run_scenario_latency() {
	// Minimal scenario: every measured op simply increments ok.
	class SmokeScenario : public benchmark_runner::IBenchScenario {
	 public:
		const char* Name() const override { return "smoke"; }
		std::uint64_t max_batch_size() const override { return benchmark_runner::IBenchScenario::kUnlimitedBatch; }
		void Setup(const benchmark_runner::Args&, std::uint64_t) override {}
		bool RunOp(const benchmark_runner::Args&, std::uint64_t, std::uint64_t,
							 std::uint64_t& ok) override {
			ok++;
			return true;
		}
		void Teardown() override {}
	};

	std::remove("/tmp/bench_smoke_test_run.csv");

	char arg0[] = "bench_smoke";
	char arg1[] = "--metric";
	char arg2[] = "latency";
	char arg3[] = "--iters";
	char arg4[] = "10";
	char arg5[] = "--warmup-iters";
	char arg6[] = "2";
	char arg7[] = "--batch-size";
	char arg8[] = "1";
	char arg9[] = "--trial-id";
	char arg10[] = "99";
	char arg11[] = "--version-tag";
	char arg12[] = "test";
	char arg13[] = "--out";
	char arg14[] = "/tmp/bench_smoke_test_run.csv";
	char* argv[] = {
			arg0, arg1, arg2, arg3, arg4, arg5, arg6, arg7,
			arg8, arg9, arg10, arg11, arg12, arg13, arg14};
	int argc = sizeof(argv) / sizeof(argv[0]);

	SmokeScenario smokey;
	int rc = benchmark_runner::RunScenario(smokey, argc, argv);
	check(rc == 0, "RunScenario returns 0");

	std::ifstream f("/tmp/bench_smoke_test_run.csv");
	std::string header, data;
	bool h_ok = static_cast<bool>(std::getline(f, header));
	check(h_ok, "CSV has header");
	check(header.find("mode") != std::string::npos, "header contains mode");
	check(header.find("avg_ns") != std::string::npos, "header contains avg_ns");
	bool d_ok = static_cast<bool>(std::getline(f, data));
	check(d_ok, "CSV has data row");
	check(
			data.find("latency,smoke,test") != std::string::npos,
			"data row contains expected fields");
	check(data.find(",99,") != std::string::npos, "data row contains trial_id=99");

	// Run without --out to exercise the stdout-only path.
	char b0[] = "bench_smoke";
	char b1[] = "--metric"; char b2[] = "latency";
	char b3[] = "--iters"; char b4[] = "3";
	char b5[] = "--warmup-iters"; char b6[] = "1";
	char b7[] = "--batch-size"; char b8[] = "2";
	char* argv2[] = {b0, b1, b2, b3, b4, b5, b6, b7, b8};
	int argc2 = sizeof(argv2) / sizeof(argv2[0]);
	SmokeScenario smokey2;
	rc = benchmark_runner::RunScenario(smokey2, argc2, argv2);
	check(rc == 0, "RunScenario without --out returns 0");

	std::remove("/tmp/bench_smoke_test_run.csv");
	std::cout << "  RunScenario (latency): OK\n";
}

}  // namespace

int main() {
	std::cout << "Benchmark smoke tests:\n";
	test_ensure_csv_header();
	test_percentile();
	test_prefill_sell_book();
	test_run_scenario_latency();

	if (failures > 0) {
		std::cerr << "\n" << failures << " test(s) FAILED\n";
		return 1;
	}
	std::cout << "\nAll benchmark smoke tests passed\n";
	return 0;
}
