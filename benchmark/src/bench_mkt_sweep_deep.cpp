/**
 * @file bench_mkt_sweep_deep.cpp
 * @brief Benchmark scenario: market order sweeping a deep resting book.
 *
 * Prefills the sell side with @p orders spread across @p levels, then
 * submits an aggressive buy market order for quantity = @p levels * 2.
 * The market order consumes as many resting sells as possible; any unfilled
 * remainder is cancelled. Measures the cost of sequential matching +
 * queue removal across multiple price levels.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class MktSweepDeepScenario : public benchmark_runner::IBenchScenario {
 public:
	const char* Name() const override { return "mkt_sweep_deep"; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t) override {
		// Build a deep ask side; the measured market order will sweep through it.
		book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
		base_ = 300'000ULL;
		benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base_);
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		// Deterministic ids avoid accidental duplicate-id rejections.
		const std::uint64_t mkt_id = base_ + args.orders + args.levels + 100
																 + static_cast<std::uint64_t>(batch_idx);
		const auto res = book_->add_market_order(
				mkt_id, matching::Side::Buy, args.levels * 2, mkt_id);
		if (res.code == matching::ErrorCode::Success ||
				res.code == matching::ErrorCode::MarketRemainderCancelled) {
			++ok;
		}
		return true;
	}

	void Teardown() override { book_.reset(); }

 private:
	std::unique_ptr<matching::OrderBook> book_;
	std::uint64_t base_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
	MktSweepDeepScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
