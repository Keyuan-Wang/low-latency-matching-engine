/**
 * @file bench_hft_cxl_miss.cpp
 * @brief HFT micro-benchmark: cancel non-existent order (miss path).
 *
 * Prefills a sell-side book with HFT-realistic depth decay, then attempts to
 * cancel an ID that does not exist.  Tests the hash-table miss path —
 * find() returns null, the engine records the pending cancel but no erase
 * occurs.  Non-destructive (no book state changes).
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>

namespace {

class BenchHftCxlMiss final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_cxl_miss"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override {
        return kUnlimitedBatch;
    }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        book_ = std::make_unique<matching::OrderBook>(args.orders + args.levels + 100);
        rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        miss_id_ = 9'000'000'000ULL;  // well outside any valid range

        benchmark_runner::PrefillHftBook(*book_, args.orders, args.levels,
                                         1000, 200'000'000ULL, rng_.next());
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t, std::uint64_t& ok) override {
        const auto code = book_->cancel_order(miss_id_);
        if (code == matching::ErrorCode::UnknownOrderId) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 rng_{42};
    std::uint64_t miss_id_ = 9'000'000'000ULL;
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftCxlMiss scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
