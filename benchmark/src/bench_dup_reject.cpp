/**
 * @file bench_dup_reject.cpp
 * @brief Benchmark scenario: duplicate order-ID rejection.
 *
 * Prefills the sell side with @p orders spread across @p levels, inserts a
 * resting buy order with ID=7, then attempts to insert a second order with
 * the same ID. The expected result is DuplicateOrderId. Measures the cost
 * of the duplicate-detection lookup path.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class DupRejectScenario : public benchmark_runner::IBenchScenario {
 public:
	const char* Name() const override { return "dup_reject"; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t) override {
		book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
		const std::uint64_t base = 500'000ULL;
		benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base);
		// Insert the first occurrence of id=7.
		(void)book_->add_limit_order(7, matching::Side::Buy, 900, 10, base + 1);
	}

	bool RunOp(const benchmark_runner::Args&, std::uint64_t,
						 std::uint64_t, std::uint64_t& ok) override {
		// Re-use id=7; engine should reject as duplicate before matching.
		const auto res =
				book_->add_limit_order(7, matching::Side::Sell, 2000, 10, 200);
		if (res.code == matching::ErrorCode::DuplicateOrderId) ++ok;
		return true;
	}

	void Teardown() override { book_.reset(); }

 private:
	std::unique_ptr<matching::OrderBook> book_;
};

}  // namespace

int main(int argc, char** argv) {
	DupRejectScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
