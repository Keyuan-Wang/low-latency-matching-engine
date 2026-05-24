/**
 * @file bench_dup_reject.cpp
 * @brief Benchmark scenario: duplicate order-ID rejection.
 *
 * Prefills the sell side, then each measured operation picks a random ID
 * from inside the prefilled range and attempts to insert a second order
 * with that ID.  The ID changes every call so the compiler and branch
 * predictor cannot lock onto a fixed constant.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class DupRejectScenario : public benchmark_runner::IBenchScenario {
	public:
	const char* Name() const override { return "dup_reject"; }
	[[nodiscard]] std::uint64_t max_batch_size() const override { return benchmark_runner::IBenchScenario::kUnlimitedBatch; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
		book_ = std::make_unique<matching::OrderBook>();
		rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
		id_base_ = 500'000ULL;
		benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, id_base_);

		const std::uint64_t per_level =
			std::max<std::uint64_t>(1, args.orders / std::max<std::uint64_t>(1, args.levels));
		total_ = per_level * args.levels;
	}

	bool RunOp(const benchmark_runner::Args&, std::uint64_t,
			   std::uint64_t, std::uint64_t& ok) override {
		const std::uint64_t dup_id = id_base_ + (rng_.next() % total_);
		const auto res = book_->add_limit_order(dup_id, matching::Side::Buy, 900, 10, dup_id);
		if (res.code == matching::ErrorCode::DuplicateOrderId) ++ok;
		return true;
	}

	void Teardown() override { book_.reset(); }

	private:
	std::unique_ptr<matching::OrderBook> book_;
	benchmark_runner::SplitMix64 rng_{42};
	std::uint64_t id_base_ = 0;
	std::uint64_t total_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
	DupRejectScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
