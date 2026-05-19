/**
 * @file bench_mkt_sweep_deep.cpp
 * @brief Benchmark scenario: market order sweeping a deep resting book.
 *
 * Prefills the sell side, then submits a buy market order that consumes as
 * many resting sells as possible.  Each RunOp draws a fresh random ID.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class MktSweepDeepScenario : public benchmark_runner::IBenchScenario {
 public:
  const char* Name() const override { return "mkt_sweep_deep"; }

  void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
    book_ = std::make_unique<matching::OrderBook>();
    rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
    base_ = 300'000ULL;
    benchmark_runner::PrefillSellBook(*book_, args.orders, args.levels, base_);
    safe_base_ = base_ + args.orders + args.levels + 200;
  }

  bool RunOp(const benchmark_runner::Args& args, std::uint64_t,
             std::uint64_t, std::uint64_t& ok) override {
    const std::uint64_t mkt_id = safe_base_ + rng_.next();
    const auto res = book_->add_market_order(mkt_id, matching::Side::Buy,
                                             args.levels * 2, mkt_id);
    if (res.code == matching::ErrorCode::Success ||
        res.code == matching::ErrorCode::MarketRemainderCancelled) {
      ++ok;
    }
    return true;
  }

  void Teardown() override { book_.reset(); }

 private:
  std::unique_ptr<matching::OrderBook> book_;
  benchmark_runner::SplitMix64 rng_{42};
  std::uint64_t base_ = 0;
  std::uint64_t safe_base_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  MktSweepDeepScenario scen;
  return benchmark_runner::RunScenario(scen, argc, argv);
}
