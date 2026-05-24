/**
 * @file bench_hft_modify_near.cpp
 * @brief HFT micro-benchmark: modify (cancel + re-add) at hot level.
 *
 * Prefills a sell-side book with HFT-realistic depth decay, then modifies
 * orders at the best ±1 tick.  modify_order performs a find → erase → insert
 * sequence, testing the combined code path.  Destructive so max_batch_size=1.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class BenchHftModifyNear final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_modify_near"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override { return 1; }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 5000);
        rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        id_counter_ = 1'000'000ULL + args.seed + iter_idx * 9973ULL;
        id_base_ = 200'000'000ULL;

        benchmark_runner::PrefillHftBook(*book_, args.orders, args.levels,
                                         1000, id_base_, rng_.next());

        // Hot zone: tick 0 (20%) + tick 1 (18%)
        std::uint64_t tick0 = static_cast<std::uint64_t>(0.20 * args.orders);
        std::uint64_t tick1 = static_cast<std::uint64_t>(0.18 * args.orders);
        if (tick0 == 0) tick0 = 1;
        if (tick1 == 0) tick1 = 1;
        hot_count_ = std::min(tick0 + tick1, args.orders);
        best_price_ = 1000;
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t, std::uint64_t& ok) override {
        const std::uint64_t mod_id = id_base_ + (rng_.next() % hot_count_);
        // New buy order near the best ask, qty 1-10
        const std::int64_t new_price = best_price_ - 1 - static_cast<std::int64_t>(rng_.next() % 2);
        const std::uint64_t new_qty = 1 + (rng_.next() % 10);
        const std::uint64_t ts = id_counter_++;
        const auto res = book_->modify_order(mod_id, matching::Side::Buy,
                                             new_price, new_qty, ts);
        if (res.code == matching::ErrorCode::Success) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 rng_{42};
    std::uint64_t id_counter_ = 0;
    std::uint64_t id_base_ = 0;
    std::uint64_t hot_count_ = 0;
    std::int64_t best_price_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftModifyNear scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
