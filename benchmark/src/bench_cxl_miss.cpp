/**
 * @file bench_cxl_miss.cpp
 * @brief Benchmark scenario: cancel order with non-existent ID.
 *
 * Prefills the sell side with @p orders spread across @p levels, then
 * attempts to cancel an order ID that is guaranteed not to exist in the
 * book. The expected result is UnknownOrderId. Measures the worst-case
 * lookup path for the cancel code path (hash-table miss or full scan).
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

namespace {

class CxlMissScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "cxl_miss"; }

  bool PrepareAndRun(const benchmark_runner::Args& args, std::uint64_t op_idx,
                     std::uint64_t& ok) const override {
    matching::OrderBook book;
    const std::uint64_t base = 400'000ULL + op_idx * 10'000ULL;
    benchmark_runner::PrefillSellBook(book, args.orders, args.levels, base);
    const auto code = book.cancel_order(9'000'000'000ULL + op_idx);
    if (code == matching::ErrorCode::UnknownOrderId) ++ok;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  return benchmark_runner::RunScenario(CxlMissScenario{}, argc, argv);
}
