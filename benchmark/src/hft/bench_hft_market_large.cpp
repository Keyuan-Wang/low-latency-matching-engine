/**
 * @file bench_hft_market_large.cpp
 * @brief HFT micro-benchmark: large market sweep (5+ levels).
 *
 * Prefills a sell-side book with HFT-realistic depth decay, then submits a
 * market buy order sized to consume 5+ price levels.  Tests the matching
 * loop through many dense levels — worst-case market sweep.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class BenchHftMarketLarge final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_market_large"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override { return 1; }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        const std::uint64_t pool = args.orders + args.levels + 5000;
        book_ = std::make_unique<matching::OrderBook>(pool);
        rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        id_counter_ = 1'000'000ULL + args.seed + iter_idx * 9973ULL;

        benchmark_runner::PrefillHftBook(*book_, args.orders, args.levels,
                                         1000, 200'000'000ULL, rng_.next());

        // Size to eat ~5+ levels: cumulative through tick 4 is ~75% of orders
        qty_ = std::max<std::uint64_t>(1,
                static_cast<std::uint64_t>(args.orders * 0.70));
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t, std::uint64_t& ok) override {
        const std::uint64_t oid = id_counter_++;
        const auto res = book_->add_market_order(oid, matching::Side::Buy,
                                                 qty_, oid);
        if (res.code == matching::ErrorCode::Success ||
            res.code == matching::ErrorCode::MarketRemainderCancelled) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 rng_{42};
    std::uint64_t id_counter_ = 0;
    std::uint64_t qty_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftMarketLarge scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
