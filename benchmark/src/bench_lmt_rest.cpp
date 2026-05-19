/**
 * @file bench_lmt_rest.cpp
 * @brief Benchmark scenario: limit order resting (no fill).
 *
 * Submits a buy limit order at price 900 into an empty book where the
 * resting sell side starts at price 1000. The order never crosses the
 * spread, so it always rests — the simplest possible OrderBook operation
 * measuring pure insertion cost.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class LmtRestScenario : public benchmark_runner::IBenchScenario {
 public:
	const char* Name() const override { return "lmt_rest"; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t) override {
		// Empty-book setup isolates pure insert cost for a non-crossing order.
		book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t iter_idx,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		// args.orders/levels are unused by this scenario, but batch_size is used
		// to derive deterministic ids across iterations.
		const std::uint64_t id = 100'000ULL + iter_idx * args.batch_size + batch_idx;
		const auto res = book_->add_limit_order(id, matching::Side::Buy, 900, 10, id);
		if (res.code == matching::ErrorCode::Success) ++ok;
		return true;
	}

	void Teardown() override { book_.reset(); }

 private:
	std::unique_ptr<matching::OrderBook> book_;
};

}  // namespace

int main(int argc, char** argv) {
	LmtRestScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
