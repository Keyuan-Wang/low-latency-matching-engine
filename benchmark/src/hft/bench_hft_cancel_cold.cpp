/**
 * @file bench_hft_cancel_cold.cpp
 * @brief HFT micro-benchmark: cancel from sparse deep level (far from best).
 *
 * Prefills a sell-side book with HFT-realistic depth decay, then cancels
 * orders from tick 6+ (cold path — sparse price levels with few orders).
 * These stress hash-table erase on nearly-empty buckets.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class BenchHftCancelCold final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_cancel_cold"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override { return 1; }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
        rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        id_base_ = 200'000'000ULL;

        benchmark_runner::PrefillHftBook(*book_, args.orders, args.levels,
                                         1000, id_base_, rng_.next());

        // Ticks 0..5 = 83% of orders (hot zone).  The remaining 17% are the cold zone.
        const std::uint64_t hot_zone = static_cast<std::uint64_t>(0.83 * args.orders);
        cold_start_ = id_base_ + std::min(hot_zone, args.orders);
        cold_count_ = (args.orders > hot_zone) ? (args.orders - hot_zone) : 1;
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t, std::uint64_t& ok) override {
        const std::uint64_t cancel_id = cold_start_ + (rng_.next() % cold_count_);
        const auto code = book_->cancel_order(cancel_id);
        if (code == matching::ErrorCode::Success) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 rng_{42};
    std::uint64_t id_base_ = 0;
    std::uint64_t cold_start_ = 0;
    std::uint64_t cold_count_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftCancelCold scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
