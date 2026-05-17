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

namespace {

class DupRejectScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "dup_reject"; }

  bool PrepareAndRun(const benchmark_runner::Args& args, std::uint64_t op_idx,
                     std::uint64_t& ok) const override {
    matching::OrderBook book;
    const std::uint64_t base = 500'000ULL + op_idx * 10'000ULL;
    benchmark_runner::PrefillSellBook(book, args.orders, args.levels, base);
    (void)book.add_limit_order(7, matching::Side::Buy, 900, 10, base + 1);
    const auto res = book.add_limit_order(7, matching::Side::Sell, 2000, 10, base + 2);
    if (res.code == matching::ErrorCode::DuplicateOrderId) ++ok;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  return benchmark_runner::RunScenario(DupRejectScenario{}, argc, argv);
}
