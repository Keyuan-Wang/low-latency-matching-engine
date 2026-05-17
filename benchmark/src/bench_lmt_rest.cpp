/**
 * @file bench_lmt_rest.cpp
 * @brief Benchmark scenario: limit order resting (no fill).
 *
 * Submits a buy limit order at price 900 into an empty book where the
 * resting sell side starts at price 1000. The order never crosses the
 * spread, so it always rests — the simplest possible OrderBook operation
 * measuring pure insertion cost.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

namespace {

class LmtRestScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "lmt_rest"; }

  bool PrepareAndRun(const benchmark_runner::Args& /*args*/, std::uint64_t op_idx,
                     std::uint64_t& ok) const override {
    matching::OrderBook book;
    const std::uint64_t id = 100'000ULL + op_idx;
    const auto res = book.add_limit_order(id, matching::Side::Buy, 900, 10, id);
    if (res.code == matching::ErrorCode::Success) ++ok;
    return true;
  }
};

}  // namespace

int main(int argc, char** argv) {
  return benchmark_runner::RunScenario(LmtRestScenario{}, argc, argv);
}
