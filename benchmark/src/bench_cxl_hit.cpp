/**
 * @file bench_cxl_hit.cpp
 * @brief Benchmark scenario: cancel an existing order (successful cancel).
 *
 * Prefills the sell side with @p orders spread across @p levels, then cancels
 * the first order ID that was inserted — the order is guaranteed to exist.
 * Measures the successful cancel hot path. Paired with bench_cxl_miss.cpp
 * (cancel non-existent ID) for complete cancel-path coverage.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class CxlHitScenario : public benchmark_runner::IBenchScenario {
 public:
	const char* Name() const override { return "cxl_hit"; }

	void Setup(const benchmark_runner::Args& args, std::uint64_t) override {
		// Capacity cushion avoids reallocation noise during the measured path.
		book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
		id_base_ = 600'000ULL;
		benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, id_base_);
	}

	bool RunOp(const benchmark_runner::Args&, std::uint64_t,
						 std::uint64_t batch_idx, std::uint64_t& ok) override {
		// The id is guaranteed to exist in the prefilled set.
		const auto code =
				book_->cancel_order(id_base_ + static_cast<std::uint64_t>(batch_idx));
		if (code == matching::ErrorCode::Success) ++ok;
		return true;
	}

	void Teardown() override { book_.reset(); }

 private:
	std::unique_ptr<matching::OrderBook> book_;
	std::uint64_t id_base_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
	CxlHitScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
