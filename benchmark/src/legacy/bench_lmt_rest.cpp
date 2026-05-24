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
	[[nodiscard]] std::uint64_t max_batch_size() const override { return benchmark_runner::IBenchScenario::kUnlimitedBatch; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
	book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
	rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
	safe_base_ = args.orders + args.levels + 200;
	}

	bool RunOp(const benchmark_runner::Args&, std::uint64_t,
			 std::uint64_t, std::uint64_t& ok) override {
	const std::uint64_t id = safe_base_ + rng_.next();
	const auto res = book_->add_limit_order(id, matching::Side::Buy, 900, 10, id);
	if (res.code == matching::ErrorCode::Success) ++ok;
	return true;
	}

	void Teardown() override { book_.reset(); }

	private:
	std::unique_ptr<matching::OrderBook> book_;
	benchmark_runner::SplitMix64 rng_{42};
	std::uint64_t safe_base_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
	LmtRestScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
