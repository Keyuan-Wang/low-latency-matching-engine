/**
 * @file bench_lmt_cross_deep.cpp
 * @brief Benchmark scenario: limit order crossing a deep resting book.
 *
 * Prefills the sell side with @p orders spread across @p levels, then
 * submits a buy limit order at price 5000 that crosses the entire spread
 * and fills against the resting sells. Measures the cost of matching
 * through a populated price-time priority queue.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class LmtCrossDeepScenario : public benchmark_runner::IBenchScenario {
 public:
	const char* Name() const override { return "lmt_cross_deep"; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t) override {
		// Prepare deep opposite liquidity to stress the crossing loop.
		book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
		base_ = 200'000ULL;
		benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base_);
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		const std::uint64_t buy_id =
				base_ + args.orders + args.levels + 100 +
				static_cast<std::uint64_t>(batch_idx);
		const auto res = book_->add_limit_order(
				buy_id, matching::Side::Buy, 5000, args.levels, buy_id);
		if (res.code == matching::ErrorCode::Success) ++ok;
		return true;
	}

	void Teardown() override { book_.reset(); }

 private:
	std::unique_ptr<matching::OrderBook> book_;
	std::uint64_t base_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
	LmtCrossDeepScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
