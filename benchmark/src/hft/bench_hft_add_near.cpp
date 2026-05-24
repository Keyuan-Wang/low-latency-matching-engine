/**
 * @file bench_hft_add_near.cpp
 * @brief HFT micro-benchmark: hot add at existing dense level (near best price).
 *
 * Prefills a sell-side book with HFT-realistic depth decay, then submits buy
 * limit orders at best ±1 tick.  These insert into already-existing price
 * levels (hot path — dense hash bucket or map node).  Non-destructive:
 * orders rest on the book without consuming levels.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class BenchHftAddNear final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_add_near"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override {
        return kUnlimitedBatch;
    }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        const std::uint64_t pool = args.orders + args.levels + args.batch_size + 5000;
        book_ = std::make_unique<matching::OrderBook>(pool);
        rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        id_counter_ = 1'000'000ULL + args.seed + iter_idx * 9973ULL;
        best_price_ = 1000;  // best ask after prefill

        benchmark_runner::PrefillHftBook(*book_, args.orders, args.levels,
                                         1000, 200'000'000ULL, rng_.next());
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t, std::uint64_t& ok) override {
        // Non-crossing buy: 1-2 ticks below best ask
        const std::int64_t price = best_price_ - 1 - static_cast<std::int64_t>(rng_.next() % 2);
        const std::uint64_t qty = 1 + (rng_.next() % 10);
        const std::uint64_t oid = id_counter_++;
        const auto res = book_->add_limit_order(oid, matching::Side::Buy, price, qty, oid);
        if (res.code == matching::ErrorCode::Success) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 rng_{42};
    std::uint64_t id_counter_ = 0;
    std::int64_t best_price_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftAddNear scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
