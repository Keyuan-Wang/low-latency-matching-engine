/**
 * @file bench_hft_add_far.cpp
 * @brief HFT micro-benchmark: cold add at sparse/deep level (far from best).
 *
 * Prefills a sell-side book with HFT-realistic depth decay, then submits buy
 * limit orders 10-50 ticks below the best ask.  These may create new price
 * levels or insert into very sparse levels (cold path).  Non-destructive.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class BenchHftAddFar final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_add_far"; }
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
        // Non-crossing buy far below best ask (10-50 ticks away)
        const std::int64_t price = best_price_ - 10 - static_cast<std::int64_t>(rng_.next() % 41);
        const std::uint64_t oid = id_counter_++;
        const auto res = book_->add_limit_order(oid, matching::Side::Buy, price, 1, oid);
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
    BenchHftAddFar scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
