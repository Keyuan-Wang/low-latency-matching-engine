/**
	* @file bench_lmt_cross_shallow.cpp
	* @brief Benchmark scenario: limit order crossing a shallow portion of the book.
	*
	* Prefills the sell side, then submits a buy limit priced to cross only the
	* first 3 price levels.  Part of the quantity fills; the remainder rests.
	* Each RunOp uses a fresh random buyer ID.
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
	[[nodiscard]] std::uint64_t max_batch_size() const override { return 1; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
	book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
	rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
	base_ = 700'000ULL;
	benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base_);

	const std::uint64_t per_level =
		std::max<std::uint64_t>(1, args.orders / std::max<std::uint64_t>(1, args.levels));
	const std::uint64_t shallow_depth =
		std::min<std::uint64_t>(3, std::max<std::uint64_t>(1, args.levels));
	price_ = static_cast<std::int64_t>(1000 + shallow_depth - 1);
	qty_   = static_cast<std::uint32_t>(per_level * shallow_depth + 5);
	safe_base_ = base_ + args.orders + args.levels + 200;
	}

	bool RunOp(const benchmark_runner::Args&, std::uint64_t,
			 std::uint64_t, std::uint64_t& ok) override {
	const std::uint64_t buy_id = safe_base_ + rng_.next();
	const auto res = book_->add_limit_order(buy_id, matching::Side::Buy, price_, qty_, buy_id);
	if (res.code == matching::ErrorCode::Success) ++ok;
	return true;
	}

	void Teardown() override { book_.reset(); }

	private:
	std::unique_ptr<matching::OrderBook> book_;
	benchmark_runner::SplitMix64 rng_{42};
	std::uint64_t base_{};
	std::int64_t  price_{};
	std::uint32_t qty_{};
	std::uint64_t safe_base_{};
};

}  // namespace

int main(int argc, char** argv) {
	LmtCrossShallowScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
