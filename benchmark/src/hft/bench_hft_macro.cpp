/**
 * @file bench_hft_macro.cpp
 * @brief HFT macro benchmark: Zero-Intelligence model of HFT order flow.
 *
 * Generates a continuous stream of empirically-grounded order-book events:
 *   45% limit add (near best price), 48% cancel, 5% modify, 2% market.
 * Spontaneous cancel clusters (power-law size, 15% trigger probability)
 * reproduce the temporal autocorrelation observed in real HFT markets.
 *
 * THE KEY DESIGN CHOICE:
 * All event parameters (RNG, cancel-target selection, tracking-map updates)
 * are pre-generated in Setup() -- which is outside the timed window. Pending
 * operations store stable business order IDs; the OrderBook resolves those IDs
 * through its own live-order index.
 *
 * Reference: Gode & Sunder (1993), "Allocative Efficiency of Markets with
 * Zero-Intelligence Traders." JPE 101(1), 119-137.
 */

#include "benchmark_runner.hpp"
#include "hft_macro_workload.hpp"
#include "match_result_buffer.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace {

enum class ResultMode {
	Book,
	OverallVector,
};

[[nodiscard]] ResultMode ParseResultMode(int argc, char** argv) {
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg != "--result-mode") continue;
		if (i + 1 >= argc) {
			std::cerr << "missing value for --result-mode\n";
			std::exit(2);
		}
		const std::string mode = argv[++i];
		if (mode == "book" || mode == "book_latency") {
			return ResultMode::Book;
		}
		if (mode == "overall_vector" || mode == "vector") {
			return ResultMode::OverallVector;
		}
		std::cerr << "unknown --result-mode: " << mode << "\n";
		std::exit(2);
	}
	return ResultMode::Book;
}

template <typename Sink, bool CollectResults>
class BenchHftMacro final : public benchmark_runner::IBenchScenario {
public:
	explicit BenchHftMacro(const char* name) : name_(name) {}

	const char* Name() const override { return name_; }

	[[nodiscard]] std::uint64_t max_batch_size() const override {
		return 1'000'000;
	}

	void Setup(const benchmark_runner::Args& args,
						 std::uint64_t iter_idx) override {
		if constexpr (CollectResults) {
			results_.reserve(args.batch_size, args.batch_size + 550'000);
			results_.clear();
		}
		workload_.Setup(args, iter_idx);
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t iter_idx,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		(void)args;
		(void)iter_idx;
		if constexpr (CollectResults) {
			return workload_.ExecuteAndCollect(
					static_cast<std::size_t>(batch_idx), ok, results_);
		}
		return workload_.Execute(static_cast<std::size_t>(batch_idx), ok);
	}

	void Teardown() override {
		workload_.Teardown();
	}

private:
	const char* name_;
	benchmark_runner::hft::HftMacroWorkload<false, Sink> workload_;
	benchmark_runner::MatchResultBuffer results_;
};

}  // namespace

int main(int argc, char** argv) {
	switch (ParseResultMode(argc, argv)) {
	case ResultMode::Book: {
		auto scen = std::make_unique<
				BenchHftMacro<llmes::matching_core::NullTradeSink, false>>(
				"hft_macro_book_latency");
		return benchmark_runner::RunScenario(*scen, argc, argv);
	}
	case ResultMode::OverallVector: {
		auto scen = std::make_unique<
				BenchHftMacro<llmes::matching_core::VectorTradeSink, true>>(
				"hft_macro_overall_vector");
		return benchmark_runner::RunScenario(*scen, argc, argv);
	}
	}
	return 2;
}
