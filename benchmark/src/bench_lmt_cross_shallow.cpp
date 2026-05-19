/**
 * @file bench_lmt_cross_shallow.cpp
 * @brief Benchmark scenario: limit order crossing a shallow portion of the book.
 *
 * Prefills the sell side with @p orders spread across @p levels, then submits
 * a buy limit order priced to cross only the first 3 price levels. Part of the
 * quantity is filled against resting sells; the remainder rests on the bid
 * side. Measures the partial-match + remainder-insert path, complementing
 * lmt_rest (K=0) and lmt_cross_deep (K=all levels).
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>

namespace {

class LmtCrossShallowScenario : public benchmark_runner::IBenchScenario {
 public:
	const char* Name() const override { return "lmt_cross_shallow"; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t) override {
		book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
		base_ = 700'000ULL;
		benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base_);

		// Estimate book depth per level to size the partially crossing order.
		const std::uint64_t per_level =
				std::max<std::uint64_t>(1, args.orders / std::max<std::uint64_t>(1, args.levels));
		const std::uint64_t shallow_depth =
				std::min<std::uint64_t>(3, std::max<std::uint64_t>(1, args.levels));
		price_ = static_cast<std::int64_t>(1000 + shallow_depth - 1);
		// Keep qty slightly above the crossed depth so a bid remainder gets rested.
		qty_   = static_cast<std::uint32_t>(per_level * shallow_depth + 5);
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		const std::uint64_t buy_id = base_ + args.orders + args.levels + 100
																 + static_cast<std::uint64_t>(batch_idx);
		const auto res = book_->add_limit_order(
				buy_id, matching::Side::Buy, price_, qty_, buy_id);
		if (res.code == matching::ErrorCode::Success) ++ok;
		return true;
	}

	void Teardown() override { book_.reset(); }

 private:
	std::unique_ptr<matching::OrderBook> book_;
	std::uint64_t base_{};
	std::int64_t  price_{};
	std::uint32_t qty_{};
};

}  // namespace

int main(int argc, char** argv) {
	LmtCrossShallowScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
