/**
 * @file bench_hft_cancel_hot.cpp
 * @brief HFT micro-benchmark: cancel from dense near-best level.
 *
 * Prefills a sell-side book with HFT-realistic depth decay, then cancels
 * orders from the best ±1 tick levels (hot path — dense price levels where
 * most HFT cancellation activity occurs).  Each cancel consumes one order,
 * so max_batch_size is 1.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>
#include <vector>

namespace {

class BenchHftCancelHot final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_cancel_hot"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override { return 1; }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
        rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        id_base_ = 200'000'000ULL;
        handles_.clear();

        benchmark_runner::PrefillHftBook(*book_, args.orders, args.levels,
                                         1000, id_base_, rng_.next(),
                                         matching::Side::Sell, &handles_);

        // Compute how many orders land at tick 0 (20%) and tick 1 (18%).
        // These form the hot zone for cancellation.
        std::uint64_t tick0 = static_cast<std::uint64_t>(0.20 * args.orders);
        std::uint64_t tick1 = static_cast<std::uint64_t>(0.18 * args.orders);
        if (tick0 == 0) tick0 = 1;
        if (tick1 == 0) tick1 = 1;
        hot_count_ = std::min(tick0 + tick1, args.orders);
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t, std::uint64_t& ok) override {
        const auto cancel_handle = handles_[rng_.next() % hot_count_];
        const auto code = book_->cancel_order(cancel_handle);
        if (code == matching::ErrorCode::Success) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 rng_{42};
    std::uint64_t id_base_ = 0;
    std::uint64_t hot_count_ = 0;
    std::vector<matching::OrderHandle> handles_;
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftCancelHot scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
