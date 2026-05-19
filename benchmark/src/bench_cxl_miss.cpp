/**
 * @file bench_cxl_miss.cpp
 * @brief Benchmark scenario: cancel order with non-existent ID.
 *
 * Prefills the sell side with @p orders spread across @p levels, then
 * attempts to cancel an order ID that is guaranteed not to exist in the
 * book. Measures the worst-case lookup path for the cancel code path.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class CxlMissScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "cxl_miss"; }

  void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
    book_ = std::make_unique<matching::OrderBook>();
    rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
    const std::uint64_t base = 400'000ULL;
    benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base);
  }

  bool RunOp(const benchmark_runner::Args&, std::uint64_t,
             std::uint64_t, std::uint64_t& ok) override {
    const std::uint64_t miss_id = 9'000'000'000ULL + (rng_.next() & 0xFFFFFFULL);
    const auto code = book_->cancel_order(miss_id);
    if (code == matching::ErrorCode::UnknownOrderId) ++ok;
    return true;
  }

  void Teardown() override { book_.reset(); }

 private:
  std::unique_ptr<matching::OrderBook> book_;
  benchmark_runner::SplitMix64 rng_{42};
};

}  // namespace

int main(int argc, char** argv) {
  CxlMissScenario scen;
  return benchmark_runner::RunScenario(scen, argc, argv);
}
