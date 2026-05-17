/**
 * @file bench_lmt_cross_deep.cpp
 * @brief Benchmark scenario: limit order crossing a deep resting book.
 *
 * Prefills the sell side with @p orders spread across @p levels, then
 * submits a buy limit order at price 5000 that crosses the entire spread
 * and fills against the resting sells. Measures the cost of matching
 * through a populated price-time priority queue.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

namespace {

class LmtCrossDeepScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "lmt_cross_deep"; }

  bool PrepareAndRun(const benchmark_runner::Args& args, std::uint64_t op_idx,
                     std::uint64_t& ok) const override {
    matching::OrderBook book;
    const std::uint64_t base = 200'000ULL + op_idx * 10'000ULL;
    benchmark_runner::PrefillSellBook(book, args.orders, args.levels, base);
    const auto res = book.add_limit_order(base + 1, matching::Side::Buy, 5000,
                                          static_cast<std::uint32_t>(args.levels),
                                          base + 1);
    if (res.code == matching::ErrorCode::Success) ++ok;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  return benchmark_runner::RunScenario(LmtCrossDeepScenario{}, argc, argv);
}
