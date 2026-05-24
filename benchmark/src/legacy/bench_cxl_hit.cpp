/**
	* @file bench_cxl_hit.cpp
	* @brief Benchmark scenario: cancel an existing order (successful cancel).
	*
	* Prefills the sell side with @p orders spread across @p levels, then
	* cancels a randomly chosen order ID from the prefilled range. Measures
	* the successful cancel hot path. Paired with bench_cxl_miss.cpp.
	*/

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class CxlHitScenario : public benchmark_runner::IBenchScenario {
	public:
	const char* Name() const override { return "cxl_hit"; }
	[[nodiscard]] std::uint64_t max_batch_size() const override { return 1; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
	book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
	rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
	id_base_ = 600'000ULL;
	benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, id_base_);

	const std::uint64_t per_level =
		std::max<std::uint64_t>(1, args.orders / std::max<std::uint64_t>(1, args.levels));
	total_ = per_level * args.levels;
	}

	bool RunOp(const benchmark_runner::Args&, std::uint64_t,
			 std::uint64_t, std::uint64_t& ok) override {
	const std::uint64_t cancel_id = id_base_ + (rng_.next() % total_);
	const auto code = book_->cancel_order(cancel_id);
	if (code == matching::ErrorCode::Success) ++ok;
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
	CxlHitScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
