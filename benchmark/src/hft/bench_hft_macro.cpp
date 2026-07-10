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
 * operations store stable business order IDs; each book instance maintains its
 * own local order_id -> handle map because handles are book-local locators.
 *
 * Reference: Gode & Sunder (1993), "Allocative Efficiency of Markets with
 * Zero-Intelligence Traders." JPE 101(1), 119-137.
 */

#include "benchmark_runner.hpp"
#include "hft_macro_workload.hpp"

#include <cstdint>

namespace {

class BenchHftMacro final : public benchmark_runner::IBenchScenario {
public:
	const char* Name() const override { return "hft_macro"; }

	[[nodiscard]] std::uint64_t max_batch_size() const override {
		return 1'000'000;
	}

	void Setup(const benchmark_runner::Args& args,
						 std::uint64_t iter_idx) override {
		workload_.Setup(args, iter_idx);
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t iter_idx,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		(void)args;
		(void)iter_idx;
		return workload_.Execute(static_cast<std::size_t>(batch_idx), ok);
	}

	void Teardown() override {
		workload_.Teardown();
	}

private:
	benchmark_runner::hft::HftMacroWorkload<false> workload_;
};

}  // namespace

int main(int argc, char** argv) {
	BenchHftMacro scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
