/**
 * @file bench_hft_macro.cpp
 * @brief HFT macro benchmark: Zero-Intelligence model of HFT order flow.
 *
 * Generates a continuous stream of empirically-grounded order-book events:
 *   45% limit add (near best price), 48% cancel, 5% modify, 2% market.
 * Spontaneous cancel clusters (power-law size, 15% trigger probability)
 * reproduce the temporal autocorrelation observed in real HFT markets.
 *
 * THE KEY DESIGN CHOICE:
 * All event parameters (RNG, cancel-target selection, tracking-map updates)
 * are pre-generated in Setup() -- which is outside the timed window. Pending
 * operations store stable business order IDs; each book instance maintains its
 * own local order_id -> handle map because handles are book-local locators.
 *
 * Reference: Gode & Sunder (1993), "Allocative Efficiency of Markets with
 * Zero-Intelligence Traders." JPE 101(1), 119-137.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#ifndef LLMES_PROFILE_HFT_MACRO_OPS
#define LLMES_PROFILE_HFT_MACRO_OPS 0
#endif

#ifndef LLMES_PROFILE_HFT_MACRO_OP_PMCS
#define LLMES_PROFILE_HFT_MACRO_OP_PMCS 0
#endif

#if LLMES_PROFILE_HFT_MACRO_OPS && (defined(__x86_64__) || defined(__i386__))
#include <x86intrin.h>
#endif

namespace {

// ------------------------------------------------------------------
//  Pre-generated book-operation parameters.
//  One PendingOp per RunOp() call -- generated in Setup(), consumed in
//  RunOp(), so RNG, ID-to-handle lookups, and tracking updates stay outside
//  the timed window.
// ------------------------------------------------------------------
struct PendingOp {
    enum Type : std::uint8_t { kLimitAdd, kCancel, kModify, kMarket };
    Type type = kLimitAdd;

    // kLimitAdd / kModify
    matching::Side side = matching::Side::Buy;
    std::int64_t price = 0;
    std::uint64_t qty = 0;

    // kCancel / kModify
    std::uint64_t target_id = 0;
    matching::OrderHandle target_handle{matching::kInvalidHandle};

    // kMarket
    std::uint64_t market_qty = 0;
    std::uint64_t market_levels_touched = 0;
    std::uint64_t market_filled_qty = 0;

    // Precomputed during setup for operation-profile reporting.
    bool add_rests = false;

    // All
    std::uint64_t oid = 0;
};

#if LLMES_PROFILE_HFT_MACRO_OPS
template <typename T>
double QuantileNearestRank(std::vector<T> values, double q) {
    if (values.empty()) return 0.0;
    const std::size_t idx = static_cast<std::size_t>(
        q * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + idx, values.end());
    return static_cast<double>(values[idx]);
}
#endif

// ------------------------------------------------------------------
class BenchHftMacro final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_macro"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override {
        return 1'000'000;
    }

    void Setup(const benchmark_runner::Args& args,
               std::uint64_t iter_idx) override {
        const std::uint64_t warmup_events = 500'000;
        const std::uint64_t pool = warmup_events + args.batch_size + 50000;

        book_ = std::make_unique<matching::OrderBook>(pool);
        event_rng_ =
            benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        param_rng_ = benchmark_runner::SplitMix64(
            args.seed * 1337ULL + iter_idx * 331ULL);
        id_counter_ = 1'000'000ULL;
        best_bid_ = 999;
        best_ask_ = 1000;

#if LLMES_PROFILE_HFT_MACRO_OPS
        current_iter_is_measured_ = (iter_idx >= args.warmup_iters);
        if (iter_idx == 0) {
            ResetProfileState(args);
        }
#endif

        // --- Warmup (untimed, uses the old generate-execute-track loop) ---
        resting_orders_.clear();
        book_handles_.clear();
        resting_ids_.clear();
        ask_level_counts_.clear();
        bid_level_counts_.clear();
        cluster_queue_.clear();

        // Initial seed: a few hundred adds for depth
        for (std::uint64_t i = 0; i < 5000; ++i) {
            do_limit_add();
        }

        for (std::uint64_t i = 0; i < warmup_events; ++i) {
            generate_and_execute_one();
        }

        update_best_prices();
        const auto base_resting_orders = resting_orders_;
        book_ = build_book_from_tracking(pool, book_handles_);

        // --- Pre-generate the measured batch ---
        // All event params and handles are decided now.  RunOp() just calls
        // the requested OrderBook operation.
        pending_.clear();
        pregen_queue_.clear();
        while (pending_.size() < args.batch_size) {
            generate_pending_one();
        }
        pending_.resize(args.batch_size);  // discard excess from clusters

        resting_orders_ = base_resting_orders;
        book_ = build_book_from_tracking(pool, book_handles_);
    }

    bool RunOp(const benchmark_runner::Args& args, std::uint64_t iter_idx,
               std::uint64_t batch_idx, std::uint64_t& ok) override {
#if LLMES_PROFILE_HFT_MACRO_OPS
        if (op_pmc_failed_) return false;
        if (iter_idx >= args.warmup_iters) {
#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
            if (op_pmc_enabled_ && !op_perf_.ResetEnable()) {
                std::cerr << "failed to enable hft_macro op PMC group\n";
                return false;
            }
#endif
            const std::uint64_t c0 = ReadCycles();
            const auto t0 = Clock::now();
            const ExecuteOutcome outcome = execute_pending(batch_idx, ok);
            const auto t1 = Clock::now();
            const std::uint64_t c1 = ReadCycles();

#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
            if (op_pmc_enabled_) {
                if (!op_perf_.Disable()) {
                    std::cerr << "failed to disable hft_macro op PMC group\n";
                    return false;
                }
                if (!op_perf_.ReadValues(op_pmc_values_)) {
                    std::cerr << "failed to read hft_macro op PMC group\n";
                    return false;
                }
            }
#endif

            const auto dt_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                t1 - t0).count();
            const std::uint64_t cycles = (c1 >= c0) ? (c1 - c0) : 0ULL;
            RecordProfileSample(outcome, static_cast<double>(dt_ns), cycles
#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
                                , op_pmc_enabled_ ? &op_pmc_values_ : nullptr
#endif
            );
            return true;
        }
#endif
        (void)execute_pending(batch_idx, ok);
        return true;
    }

    void Teardown() override {
        book_.reset();
#if LLMES_PROFILE_HFT_MACRO_OPS
        if (current_iter_is_measured_) {
            ++measured_iters_done_;
            if (!profile_emitted_ && measured_iters_done_ >= profile_args_.iters) {
                EmitProfile();
                profile_emitted_ = true;
            }
        }
#endif
    }

private:
    struct RestingMeta {
        matching::Side side = matching::Side::Buy;
        std::int64_t price = 0;
        std::uint64_t qty = 0;
    };

    struct HandleMeta {
        matching::OrderHandle handle = matching::kInvalidHandle;
        std::uint64_t qty = 0;
    };

    enum class OpBucket : std::uint8_t {
        kAddRest = 0,
        kAddCross,
        kCancelHit,
        kCancelMiss,
        kModifyHit,
        kModifyMiss,
        kMarket,
        kCount
    };

    struct ExecuteOutcome {
        OpBucket bucket = OpBucket::kMarket;
        std::uint64_t market_levels_touched = 0;
        std::uint64_t market_filled_qty = 0;
    };

#if LLMES_PROFILE_HFT_MACRO_OPS
    using Clock = std::chrono::steady_clock;

    struct OpProfile {
        const char* name = "";
        std::uint64_t count = 0;
        long double total_ns = 0.0L;
        std::uint64_t total_cycles = 0;
        std::vector<double> ns_samples;
        std::uint64_t market_levels_sum = 0;
        std::uint64_t market_filled_sum = 0;
        std::vector<std::uint64_t> market_levels_samples;
        std::vector<std::uint64_t> market_filled_samples;
#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
        std::vector<std::uint64_t> total_pmc;
#endif
    };
#endif

    // ================================================================
    //  State
    // ================================================================

    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 event_rng_{42};
    benchmark_runner::SplitMix64 param_rng_{42};
    std::uint64_t id_counter_ = 0;

    // Resting-order tracking (used for cancel-target selection during the
    // pre-generation step in Setup, NOT during RunOp).
    std::unordered_map<std::uint64_t, RestingMeta> resting_orders_;
    std::unordered_map<std::uint64_t, HandleMeta> book_handles_;
    std::vector<std::uint64_t> resting_ids_;
    std::map<std::int64_t, std::uint64_t, std::less<>> ask_level_counts_;
    std::map<std::int64_t, std::uint64_t, std::greater<>> bid_level_counts_;

    std::int64_t best_bid_ = 0;
    std::int64_t best_ask_ = 1000;

    // Cluster cancel ID queue (for warmup's generate-and-execute path)
    std::vector<std::uint64_t> cluster_queue_;

    // Pre-generated event parameters for the timed batch
    std::vector<PendingOp> pending_;
    // Overflow queue: cluster cancels that didn't fit in pending_ spill here
    std::vector<PendingOp> pregen_queue_;

#if LLMES_PROFILE_HFT_MACRO_OPS
    std::array<OpProfile, static_cast<std::size_t>(OpBucket::kCount)> op_profile_{};
    benchmark_runner::Args profile_args_{};
    std::uint64_t measured_iters_done_ = 0;
    bool current_iter_is_measured_ = false;
    bool profile_emitted_ = false;
#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
    benchmark_runner::PerfGroup op_perf_{};
    std::string op_pmc_group_name_{};
    std::vector<benchmark_runner::PerfEventSpec> op_pmc_specs_{};
    std::vector<std::uint64_t> op_pmc_values_{};
    bool op_pmc_enabled_ = false;
    bool op_pmc_failed_ = false;
#else
    bool op_pmc_failed_ = false;
#endif
#endif

    // ================================================================
    //  Warmup event generation and execution (legacy path, untimed)
    // ================================================================

    void generate_and_execute_one() {
        if (!cluster_queue_.empty()) {
            std::uint64_t target_id = cluster_queue_.back();
            cluster_queue_.pop_back();
            if (do_cancel(target_id)) return;
        }

        std::uint64_t const roll = event_rng_.next() % 100;
        if (roll < 45) { do_limit_add(); return; }
        if (roll < 93) { do_cancel_random(); return; }
        if (roll < 98) { do_modify(); return; }
        do_market();
    }

    // ================================================================
    //  Pre-generation for the timed batch (in Setup, untimed)
    // ================================================================

    void generate_pending_one() {
        // Drain pregen queue first
        if (!pregen_queue_.empty()) {
            pending_.push_back(pregen_queue_.back());
            pregen_queue_.pop_back();
            return;
        }

        std::uint64_t const roll = event_rng_.next() % 100;
        if (roll < 45) { pending_limit_add(); return; }
        if (roll < 93) { pending_cancel(); return; }
        if (roll < 98) { pending_modify(); return; }
        pending_market();
    }

    // --- Limit add pre-gen ---
    void pending_limit_add() {
        PendingOp op;
        op.type = PendingOp::kLimitAdd;
        op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                                : matching::Side::Sell;

        std::int64_t const ref = (best_ask_ > 0) ? best_ask_ : 1000;
        int offset = 0;
        std::uint64_t r = param_rng_.next();
        while ((r & 0xFF) < 161 && offset < 100) {
            ++offset; r >>= 8;
        }
        if (offset == 0) offset = 1;
        op.price = (op.side == matching::Side::Buy)
                       ? std::max<std::int64_t>(1, ref - offset - 1)
                       : ref + offset;

        static constexpr std::uint64_t kQtyTable[32] = {
            1,1,1,2,2,2,3,3,3,4,4,4,5,5,5,5,
            5,6,6,6,7,7,8,9,10,12,15,20,30,50,75,100};
        op.qty = kQtyTable[param_rng_.next() % 32];
        op.oid = id_counter_++;

        auto const res =
            book_->add_limit_order(op.oid, op.side, op.price, op.qty, op.oid);
        apply_trade_fills(res.trades, &book_handles_);
        if (res.code == matching::ErrorCode::Success && res.remaining_quantity > 0) {
            track_add_predicted(op.oid, op.side, op.price, res.remaining_quantity);
            book_handles_[op.oid] = HandleMeta{res.handle, res.remaining_quantity};
            op.add_rests = true;
        }

        pending_.push_back(op);
    }

    // --- Cancel pre-gen ---
    void pending_cancel() {
        if (resting_orders_.empty()) { pending_limit_add(); return; }

        for (int attempt = 0; attempt < 4; ++attempt) {
            std::uint64_t const target = select_cancel_target();
            if (target == 0) break;

            auto book_handle_it = book_handles_.find(target);
            if (book_handle_it == book_handles_.end()) {
                track_remove_predicted(target);
                continue;
            }

            auto const code = book_->cancel_order(book_handle_it->second.handle);
            if (code != matching::ErrorCode::Success) {
                book_handles_.erase(book_handle_it);
                track_remove_predicted(target);
                continue;
            }
            matching::OrderHandle const target_handle = book_handle_it->second.handle;
            book_handles_.erase(book_handle_it);

            PendingOp op;
            op.type = PendingOp::kCancel;
            op.target_id = target;
            op.target_handle = target_handle;
            op.oid = 0;
            pending_.push_back(op);
            track_remove_predicted(target);

            if ((param_rng_.next() % 100) < 15) {
                enqueue_cluster_pending();
            }
            return;
        }

        pending_limit_add();
    }

    // --- Modify pre-gen ---
    void pending_modify() {
        if (resting_orders_.empty()) { pending_limit_add(); return; }

        std::uint64_t const target = select_cancel_target();
        if (target == 0) { pending_limit_add(); return; }

        auto const it = resting_orders_.find(target);
        if (it == resting_orders_.end()) { pending_limit_add(); return; }
        std::int64_t const old_price = it->second.price;

        int const delta = 1 + static_cast<int>(param_rng_.next() % 3);
        std::int64_t const new_price =
            old_price + ((param_rng_.next() % 2 == 0) ? -delta : delta);
        std::uint64_t const new_qty = 1 + (param_rng_.next() % 20);

        PendingOp op;
        op.type = PendingOp::kModify;
        op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                                : matching::Side::Sell;
        op.price = new_price;
        op.qty = new_qty;
        op.target_id = target;
        op.oid = id_counter_++;

        auto book_handle_it = book_handles_.find(op.target_id);
        if (book_handle_it == book_handles_.end()) {
            track_remove_predicted(target);
            pending_limit_add();
            return;
        }
        op.target_handle = book_handle_it->second.handle;

        auto const res =
            book_->modify_order(book_handle_it->second.handle, op.side, op.price, op.qty, op.oid);
        if (res.code != matching::ErrorCode::Success) {
            book_handles_.erase(book_handle_it);
            track_remove_predicted(target);
            pending_limit_add();
            return;
        }

        book_handles_.erase(book_handle_it);
        track_remove_predicted(target);
        apply_trade_fills(res.trades, &book_handles_);
        if (res.remaining_quantity > 0) {
            track_add_predicted(target, op.side, op.price, res.remaining_quantity);
            book_handles_[target] = HandleMeta{res.handle, res.remaining_quantity};
        }
        pending_.push_back(op);
    }

    // --- Market order pre-gen ---
    void pending_market() {
        std::uint64_t const total = resting_orders_.size();
        if (total == 0) { pending_limit_add(); return; }

        PendingOp op;
        op.type = PendingOp::kMarket;
        op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                                : matching::Side::Sell;

        int const mroll = static_cast<int>(param_rng_.next() % 100);
        if (mroll < 85)
            op.market_qty = std::max<std::uint64_t>(1, total / 4);
        else if (mroll < 98)
            op.market_qty = std::max<std::uint64_t>(1, total / 2);
        else
            op.market_qty =
                std::max<std::uint64_t>(1, static_cast<std::uint64_t>(total * 0.8));

        op.oid = id_counter_++;
        auto const res =
            book_->add_market_order(op.oid, op.side, op.market_qty, op.oid);
        apply_trade_fills(res.trades, &book_handles_);
        op.market_filled_qty = res.filled_quantity;
        op.market_levels_touched = CountTouchedLevels(res.trades);
        pending_.push_back(op);
    }

    // ================================================================
    //  Timed execution -- RunOp calls this, pure OrderBook operation only
    // ================================================================

    ExecuteOutcome execute_pending(std::size_t idx, std::uint64_t& ok) {
        ExecuteOutcome outcome{};

        auto const& op = pending_[idx];
        switch (op.type) {
        case PendingOp::kLimitAdd: {
            auto const res =
                book_->add_limit_order(op.oid, op.side, op.price, op.qty, op.oid);
            if (res.code == matching::ErrorCode::Success) ++ok;
            outcome.bucket = op.add_rests ? OpBucket::kAddRest : OpBucket::kAddCross;
            break;
        }
        case PendingOp::kCancel: {
            auto const code = book_->cancel_order(op.target_handle);
            if (code == matching::ErrorCode::Success) {
                ++ok;
                outcome.bucket = OpBucket::kCancelHit;
            } else {
                outcome.bucket = OpBucket::kCancelMiss;
            }
            break;
        }
        case PendingOp::kModify: {
            auto const res = book_->modify_order(op.target_handle, op.side, op.price,
                                                 op.qty, op.oid);
            if (res.code == matching::ErrorCode::Success) ++ok;
            outcome.bucket = OpBucket::kModifyHit;
            break;
        }
        case PendingOp::kMarket: {
            auto const res =
                book_->add_market_order(op.oid, op.side, op.market_qty, op.oid);
            if (res.code == matching::ErrorCode::Success ||
                res.code == matching::ErrorCode::MarketRemainderCancelled)
                ++ok;

            outcome.bucket = OpBucket::kMarket;
            outcome.market_filled_qty = op.market_filled_qty;
            outcome.market_levels_touched = op.market_levels_touched;
            break;
        }
        }

        return outcome;
    }

    static std::uint64_t CountTouchedLevels(
        const std::vector<matching::Trade>& trades) noexcept {
        if (trades.empty()) return 0;

        std::uint64_t levels = 0;
        std::int64_t last_price = 0;
        bool has_last = false;
        for (const auto& trade : trades) {
            if (!has_last || trade.price != last_price) {
                ++levels;
                last_price = trade.price;
                has_last = true;
            }
        }
        return levels;
    }

#if LLMES_PROFILE_HFT_MACRO_OPS
    static std::uint64_t ReadCycles() noexcept {
#if defined(__x86_64__) || defined(__i386__)
        return __rdtsc();
#else
        return 0;
#endif
    }

    void ResetProfileState(const benchmark_runner::Args& args) {
        profile_args_ = args;
        measured_iters_done_ = 0;
        profile_emitted_ = false;

#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
        op_pmc_group_name_.clear();
        op_pmc_specs_.clear();
        op_pmc_values_.clear();
        op_pmc_enabled_ = false;
        op_pmc_failed_ = false;

        if (const char* group = std::getenv("LLMES_HFT_MACRO_OP_PMC_GROUP");
            group != nullptr && group[0] != '\0') {
            op_pmc_group_name_ = group;
            op_pmc_specs_ = benchmark_runner::HftMacroPerfGroupSpec(op_pmc_group_name_);
            if (op_pmc_specs_.empty()) {
                std::cerr << "unknown hft_macro op PMC group: "
                          << op_pmc_group_name_ << "\n";
                op_pmc_failed_ = true;
            } else if (!op_perf_.Open(op_pmc_specs_)) {
                std::cerr << "failed to open hft_macro op PMC group: "
                          << op_pmc_group_name_ << "\n";
                op_pmc_failed_ = true;
            } else {
                op_pmc_enabled_ = true;
                op_pmc_values_.assign(op_pmc_specs_.size(), 0);
            }
        }
#endif

        static constexpr std::array<const char*, static_cast<std::size_t>(OpBucket::kCount)>
            kNames = {
                "add_rest",
                "add_cross",
                "cancel_hit",
                "cancel_miss",
                "modify_hit",
                "modify_miss",
                "market"
            };

        const std::size_t samples = static_cast<std::size_t>(args.batch_size * args.iters);
        const std::size_t reserve_total = std::max<std::size_t>(samples, 64);
        const std::array<std::size_t, static_cast<std::size_t>(OpBucket::kCount)> weights = {
            30, 15, 40, 8, 4, 1, 2
        };

        for (std::size_t i = 0; i < op_profile_.size(); ++i) {
            auto& p = op_profile_[i];
            p.name = kNames[i];
            p.count = 0;
            p.total_ns = 0.0L;
            p.total_cycles = 0;
            p.market_levels_sum = 0;
            p.market_filled_sum = 0;
            p.ns_samples.clear();
            p.market_levels_samples.clear();
            p.market_filled_samples.clear();
#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
            p.total_pmc.assign(op_pmc_specs_.size(), 0);
#endif

            const std::size_t reserve =
                std::max<std::size_t>(32, (reserve_total * weights[i]) / 100);
            p.ns_samples.reserve(reserve);
            if (i == static_cast<std::size_t>(OpBucket::kMarket)) {
                p.market_levels_samples.reserve(reserve);
                p.market_filled_samples.reserve(reserve);
            }
        }
    }

    void RecordProfileSample(const ExecuteOutcome& outcome, double ns,
                             std::uint64_t cycles
#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
                             , const std::vector<std::uint64_t>* pmc_values
#endif
                             ) {
        auto& p = op_profile_[static_cast<std::size_t>(outcome.bucket)];
        ++p.count;
        p.total_ns += ns;
        p.total_cycles += cycles;
        p.ns_samples.push_back(ns);

#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
        if (pmc_values != nullptr && pmc_values->size() == p.total_pmc.size()) {
            for (std::size_t i = 0; i < pmc_values->size(); ++i) {
                p.total_pmc[i] += (*pmc_values)[i];
            }
        }
#endif

        if (outcome.bucket == OpBucket::kMarket) {
            p.market_levels_sum += outcome.market_levels_touched;
            p.market_filled_sum += outcome.market_filled_qty;
            p.market_levels_samples.push_back(outcome.market_levels_touched);
            p.market_filled_samples.push_back(outcome.market_filled_qty);
        }
    }

    void EmitProfile() const {
        std::uint64_t total_ops = 0;
        long double total_ns = 0.0L;
        long double total_cycles = 0.0L;
        for (const auto& p : op_profile_) {
            total_ops += p.count;
            total_ns += p.total_ns;
            total_cycles += static_cast<long double>(p.total_cycles);
        }

        if (total_ops == 0) return;

        const char* out_path = std::getenv("LLMES_HFT_MACRO_OP_PROFILE_OUT");
        const bool write_csv = (out_path != nullptr && out_path[0] != '\0');
        if (write_csv) {
            benchmark_runner::EnsureCsvHeader(
                out_path,
                "scenario,version_tag,commit_sha,trial_id,seed,orders,levels,batch_size,"
                "warmup_iters,iters,op_type,count,share,mean_ns,p50_ns,p95_ns,p99_ns,"
                "mean_cycles,weighted_ns_per_event,weighted_cycles_per_event,"
                "weighted_ns_share,weighted_cycles_share,market_levels_mean,"
                "market_levels_p95,market_levels_p99,market_filled_qty_mean,"
                "market_filled_qty_p95,market_filled_qty_p99");
        }

        std::cout << "macro_op_profile"
                  << " scenario=" << Name()
                  << " version_tag=" << profile_args_.version_tag
                  << " commit_sha=" << profile_args_.commit_sha
                  << " trial_id=" << profile_args_.trial_id
                  << " seed=" << profile_args_.seed
                  << " orders=" << profile_args_.orders
                  << " levels=" << profile_args_.levels
                  << " batch_size=" << profile_args_.batch_size
                  << " warmup_iters=" << profile_args_.warmup_iters
                  << " iters=" << profile_args_.iters
                  << " total_ops=" << total_ops
                  << "\n";

        for (const auto& p : op_profile_) {
            const double share = static_cast<double>(p.count) /
                                 static_cast<double>(total_ops);
            const double mean_ns = (p.count > 0)
                                       ? static_cast<double>(p.total_ns /
                                            static_cast<long double>(p.count))
                                       : 0.0;
            const double p50_ns = benchmark_runner::Percentile(p.ns_samples, 0.50);
            const double p95_ns = benchmark_runner::Percentile(p.ns_samples, 0.95);
            const double p99_ns = benchmark_runner::Percentile(p.ns_samples, 0.99);
            const double mean_cycles = (p.count > 0)
                                           ? static_cast<double>(p.total_cycles) /
                                                 static_cast<double>(p.count)
                                           : 0.0;

            const double weighted_ns_per_event = share * mean_ns;
            const double weighted_cycles_per_event = share * mean_cycles;
            const double weighted_ns_share =
                (total_ns > 0.0L)
                    ? static_cast<double>(p.total_ns / total_ns)
                    : 0.0;
            const double weighted_cycles_share =
                (total_cycles > 0.0L)
                    ? static_cast<double>(
                        static_cast<long double>(p.total_cycles) / total_cycles)
                    : 0.0;

            double market_levels_mean = 0.0;
            double market_levels_p95 = 0.0;
            double market_levels_p99 = 0.0;
            double market_filled_mean = 0.0;
            double market_filled_p95 = 0.0;
            double market_filled_p99 = 0.0;
            if (std::string(p.name) == "market" && p.count > 0) {
                market_levels_mean = static_cast<double>(p.market_levels_sum) /
                                     static_cast<double>(p.count);
                market_levels_p95 =
                    QuantileNearestRank(p.market_levels_samples, 0.95);
                market_levels_p99 =
                    QuantileNearestRank(p.market_levels_samples, 0.99);
                market_filled_mean = static_cast<double>(p.market_filled_sum) /
                                     static_cast<double>(p.count);
                market_filled_p95 =
                    QuantileNearestRank(p.market_filled_samples, 0.95);
                market_filled_p99 =
                    QuantileNearestRank(p.market_filled_samples, 0.99);
            }

            std::cout << "macro_op_profile"
                      << " op_type=" << p.name
                      << " count=" << p.count
                      << " share=" << share
                      << " mean_ns=" << mean_ns
                      << " p50_ns=" << p50_ns
                      << " p95_ns=" << p95_ns
                      << " p99_ns=" << p99_ns
                      << " mean_cycles=" << mean_cycles
                      << " weighted_ns_per_event=" << weighted_ns_per_event
                      << " weighted_cycles_per_event=" << weighted_cycles_per_event
                      << " weighted_ns_share=" << weighted_ns_share
                      << " weighted_cycles_share=" << weighted_cycles_share
                      << " market_levels_mean=" << market_levels_mean
                      << " market_levels_p95=" << market_levels_p95
                      << " market_levels_p99=" << market_levels_p99
                      << " market_filled_qty_mean=" << market_filled_mean
                      << " market_filled_qty_p95=" << market_filled_p95
                      << " market_filled_qty_p99=" << market_filled_p99
                      << "\n";

            if (write_csv) {
                std::ofstream f(out_path, std::ios::app);
                f << Name()
                  << ","
                  << profile_args_.version_tag
                  << ","
                  << profile_args_.commit_sha
                  << ","
                  << profile_args_.trial_id
                  << ","
                  << profile_args_.seed
                  << ","
                  << profile_args_.orders
                  << ","
                  << profile_args_.levels
                  << ","
                  << profile_args_.batch_size
                  << ","
                  << profile_args_.warmup_iters
                  << ","
                  << profile_args_.iters
                  << ","
                  << p.name
                  << ","
                  << p.count
                  << ","
                  << share
                  << ","
                  << mean_ns
                  << ","
                  << p50_ns
                  << ","
                  << p95_ns
                  << ","
                  << p99_ns
                  << ","
                  << mean_cycles
                  << ","
                  << weighted_ns_per_event
                  << ","
                  << weighted_cycles_per_event
                  << ","
                  << weighted_ns_share
                  << ","
                  << weighted_cycles_share
                  << ","
                  << market_levels_mean
                  << ","
                  << market_levels_p95
                  << ","
                  << market_levels_p99
                  << ","
                  << market_filled_mean
                  << ","
                  << market_filled_p95
                  << ","
                  << market_filled_p99
                  << "\n";
            }
        }

#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
        EmitPmcProfile(total_ops);
#endif
    }

#if LLMES_PROFILE_HFT_MACRO_OP_PMCS
    void EmitPmcProfile(std::uint64_t total_ops) const {
        if (!op_pmc_enabled_ || total_ops == 0) return;

        const char* out_path = std::getenv("LLMES_HFT_MACRO_OP_PMC_OUT");
        const bool write_csv = (out_path != nullptr && out_path[0] != '\0');
        if (write_csv) {
            benchmark_runner::EnsureCsvHeader(
                out_path,
                "scenario,version_tag,commit_sha,trial_id,seed,orders,levels,batch_size,"
                "warmup_iters,iters,pmc_group,op_type,count,share,event_name,"
                "event_total,event_per_op,weighted_event_per_macro_op");
        }

        const auto& event_names = op_perf_.EventNames();
        std::cout << "macro_op_pmc_profile"
                  << " scenario=" << Name()
                  << " version_tag=" << profile_args_.version_tag
                  << " commit_sha=" << profile_args_.commit_sha
                  << " trial_id=" << profile_args_.trial_id
                  << " seed=" << profile_args_.seed
                  << " orders=" << profile_args_.orders
                  << " levels=" << profile_args_.levels
                  << " batch_size=" << profile_args_.batch_size
                  << " warmup_iters=" << profile_args_.warmup_iters
                  << " iters=" << profile_args_.iters
                  << " pmc_group=" << op_pmc_group_name_
                  << " total_ops=" << total_ops
                  << "\n";

        for (const auto& p : op_profile_) {
            const double share = static_cast<double>(p.count) /
                                 static_cast<double>(total_ops);
            for (std::size_t i = 0; i < p.total_pmc.size(); ++i) {
                const char* event_name =
                    (i < event_names.size()) ? event_names[i].c_str() : "unknown";
                const double event_per_op =
                    (p.count > 0)
                        ? static_cast<double>(p.total_pmc[i]) /
                              static_cast<double>(p.count)
                        : 0.0;
                const double weighted_event_per_macro_op = share * event_per_op;

                std::cout << "macro_op_pmc_profile"
                          << " op_type=" << p.name
                          << " pmc_group=" << op_pmc_group_name_
                          << " event_name=" << event_name
                          << " count=" << p.count
                          << " share=" << share
                          << " event_total=" << p.total_pmc[i]
                          << " event_per_op=" << event_per_op
                          << " weighted_event_per_macro_op="
                          << weighted_event_per_macro_op
                          << "\n";

                if (write_csv) {
                    std::ofstream f(out_path, std::ios::app);
                    f << Name()
                      << ","
                      << profile_args_.version_tag
                      << ","
                      << profile_args_.commit_sha
                      << ","
                      << profile_args_.trial_id
                      << ","
                      << profile_args_.seed
                      << ","
                      << profile_args_.orders
                      << ","
                      << profile_args_.levels
                      << ","
                      << profile_args_.batch_size
                      << ","
                      << profile_args_.warmup_iters
                      << ","
                      << profile_args_.iters
                      << ","
                      << op_pmc_group_name_
                      << ","
                      << p.name
                      << ","
                      << p.count
                      << ","
                      << share
                      << ","
                      << event_name
                      << ","
                      << p.total_pmc[i]
                      << ","
                      << event_per_op
                      << ","
                      << weighted_event_per_macro_op
                      << "\n";
                }
            }
        }
    }
#endif
#endif

    // ================================================================
    //  Tracking helpers -- only called during warmup + pre-generation
    // ================================================================

    void track_add_predicted(std::uint64_t id, matching::Side side, std::int64_t price,
                             std::uint64_t qty) {
        track_remove_predicted(id);
        resting_orders_[id] = RestingMeta{side, price, qty};
        resting_ids_.push_back(id);
        if (side == matching::Side::Buy) {
            ++bid_level_counts_[price];
        } else {
            ++ask_level_counts_[price];
        }
        update_best_prices();
    }

    void track_remove_predicted(std::uint64_t id) {
        auto it = resting_orders_.find(id);
        if (it == resting_orders_.end()) return;

        if (it->second.side == matching::Side::Buy) {
            auto lc = bid_level_counts_.find(it->second.price);
            if (lc != bid_level_counts_.end()) {
                if (lc->second > 0) --lc->second;
                if (lc->second == 0) bid_level_counts_.erase(lc);
            }
        } else {
            auto lc = ask_level_counts_.find(it->second.price);
            if (lc != ask_level_counts_.end()) {
                if (lc->second > 0) --lc->second;
                if (lc->second == 0) ask_level_counts_.erase(lc);
            }
        }
        resting_orders_.erase(it);
        update_best_prices();
    }

    void apply_trade_fills(const std::vector<matching::Trade>& trades,
                           std::unordered_map<std::uint64_t, HandleMeta>* handles = nullptr) {
        for (const auto& trade : trades) {
            auto it = resting_orders_.find(trade.maker_order_id);
            if (it == resting_orders_.end()) continue;

            if (trade.quantity >= it->second.qty) {
                if (handles != nullptr) handles->erase(trade.maker_order_id);
                track_remove_predicted(trade.maker_order_id);
                continue;
            }

            it->second.qty -= trade.quantity;
            if (handles != nullptr) {
                auto hit = handles->find(trade.maker_order_id);
                if (hit != handles->end()) {
                    hit->second.qty -= std::min(hit->second.qty, trade.quantity);
                }
            }
        }
    }

    void compact_resting_ids() {
        resting_ids_.clear();
        resting_ids_.reserve(resting_orders_.size());
        for (const auto& [id, _] : resting_orders_) resting_ids_.push_back(id);
    }

    std::unique_ptr<matching::OrderBook>
    build_book_from_tracking(
        std::size_t pool_capacity,
        std::unordered_map<std::uint64_t, HandleMeta>& handles) {
        auto book = std::make_unique<matching::OrderBook>(pool_capacity);
        handles.clear();
        std::vector<std::uint64_t> ids;
        ids.reserve(resting_orders_.size());
        for (const auto& [id, _] : resting_orders_) ids.push_back(id);
        std::sort(ids.begin(), ids.end());

        for (std::uint64_t id : ids) {
            auto it = resting_orders_.find(id);
            if (it == resting_orders_.end()) continue;
            auto const& meta = it->second;
            const auto res =
                book->add_limit_order(id, meta.side, meta.price, meta.qty, id);
            if (res.code == matching::ErrorCode::Success &&
                res.remaining_quantity > 0) {
                handles[id] = HandleMeta{res.handle, res.remaining_quantity};
            }
        }
        return book;
    }

    void update_best_prices() {
        best_bid_ = bid_level_counts_.empty() ? 0 : bid_level_counts_.begin()->first;
        best_ask_ = ask_level_counts_.empty() ? 1000 : ask_level_counts_.begin()->first;
    }

    // ================================================================
    //  Legacy warmup do_* methods (generate + execute + track inline)
    // ================================================================

    bool do_limit_add() {
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                         : matching::Side::Sell;
        std::int64_t const ref = (best_ask_ > 0) ? best_ask_ : 1000;
        int offset = 0;
        std::uint64_t r = param_rng_.next();
        while ((r & 0xFF) < 161 && offset < 100) {
            ++offset; r >>= 8;
        }
        if (offset == 0) offset = 1;
        std::int64_t const price = (side == matching::Side::Buy)
                                       ? std::max<std::int64_t>(1, ref - offset - 1)
                                       : ref + offset;
        static constexpr std::uint64_t kQtyTable[32] = {
            1,1,1,2,2,2,3,3,3,4,4,4,5,5,5,5,
            5,6,6,6,7,7,8,9,10,12,15,20,30,50,75,100};
        std::uint64_t const qty = kQtyTable[param_rng_.next() % 32];
        std::uint64_t const oid = id_counter_++;
        auto const res =
            book_->add_limit_order(oid, side, price, qty, oid);
        apply_trade_fills(res.trades, &book_handles_);
        if (res.code == matching::ErrorCode::Success) {
            if (res.remaining_quantity > 0) {
                track_add_predicted(oid, side, price, res.remaining_quantity);
                book_handles_[oid] = HandleMeta{res.handle, res.remaining_quantity};
            }
            return true;
        }
        return false;
    }

    bool do_cancel_random() {
        if (resting_orders_.empty()) return do_limit_add();
        std::uint64_t const target = select_cancel_target();
        if (target == 0) return do_limit_add();
        return do_cancel(target);
    }

    bool do_cancel(std::uint64_t target_id) {
        auto handle_it = book_handles_.find(target_id);
        if (handle_it == book_handles_.end()) return false;
        auto const code = book_->cancel_order(handle_it->second.handle);
        if (code == matching::ErrorCode::Success) {
            book_handles_.erase(handle_it);
            track_remove_predicted(target_id);
            if ((param_rng_.next() % 100) < 15) {
                enqueue_cluster_legacy();
            }
            return true;
        }
        return false;
    }

    bool do_modify() {
        if (resting_orders_.empty()) return do_limit_add();
        std::uint64_t const target = select_cancel_target();
        if (target == 0) return do_limit_add();
        auto const it = resting_orders_.find(target);
        if (it == resting_orders_.end()) return do_limit_add();
        std::int64_t const old_price = it->second.price;
        int const delta = 1 + static_cast<int>(param_rng_.next() % 3);
        std::int64_t const new_price =
            old_price + ((param_rng_.next() % 2 == 0) ? -delta : delta);
        std::uint64_t const new_qty = 1 + (param_rng_.next() % 20);
        std::uint64_t const ts = id_counter_++;
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                         : matching::Side::Sell;
        auto handle_it = book_handles_.find(target);
        if (handle_it == book_handles_.end()) return do_limit_add();
        auto const res =
            book_->modify_order(handle_it->second.handle, side, new_price, new_qty, ts);
        if (res.code == matching::ErrorCode::Success) {
            book_handles_.erase(handle_it);
            track_remove_predicted(target);
            apply_trade_fills(res.trades, &book_handles_);
            if (res.remaining_quantity > 0) {
                track_add_predicted(target, side, new_price, res.remaining_quantity);
                book_handles_[target] = HandleMeta{res.handle, res.remaining_quantity};
            }
            return true;
        }
        return false;
    }

    bool do_market() {
        std::uint64_t const total = resting_orders_.size();
        if (total == 0) return do_limit_add();
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                         : matching::Side::Sell;
        int const mroll = static_cast<int>(param_rng_.next() % 100);
        std::uint64_t qty;
        if (mroll < 85)
            qty = std::max<std::uint64_t>(1, total / 4);
        else if (mroll < 98)
            qty = std::max<std::uint64_t>(1, total / 2);
        else
            qty = std::max<std::uint64_t>(1,
                                          static_cast<std::uint64_t>(total * 0.8));
        std::uint64_t const oid = id_counter_++;
        auto const res =
            book_->add_market_order(oid, side, qty, oid);
        if (res.code == matching::ErrorCode::Success ||
            res.code == matching::ErrorCode::MarketRemainderCancelled) {
            apply_trade_fills(res.trades, &book_handles_);
            return true;
        }
        return false;
    }

    // ================================================================
    //  Cancel target selection (60/30/10 zone distribution)
    // ================================================================

    std::uint64_t select_cancel_target() {
        if (resting_orders_.empty()) return 0;
        if (resting_ids_.empty() || resting_ids_.size() > resting_orders_.size() * 8) {
            compact_resting_ids();
        }

        int const zroll = static_cast<int>(param_rng_.next() % 100);
        int min_dist, max_dist;
        if (zroll < 60) {
            min_dist = 0; max_dist = 2;
        } else if (zroll < 90) {
            min_dist = 3; max_dist = 5;
        } else {
            min_dist = 6; max_dist = 1000000;
        }

        std::int64_t const best = (best_ask_ > 0) ? best_ask_ : 1000;
        for (int attempt = 0; attempt < 200; ++attempt) {
            std::uint64_t const id =
                resting_ids_[param_rng_.next() % resting_ids_.size()];
            auto const it = resting_orders_.find(id);
            if (it == resting_orders_.end()) continue;
            int const dist = static_cast<int>(std::abs(it->second.price - best));
            if (dist >= min_dist && dist <= max_dist) return id;
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::uint64_t const id =
                resting_ids_[param_rng_.next() % resting_ids_.size()];
            if (resting_orders_.contains(id)) return id;
        }
        std::size_t offset = static_cast<std::size_t>(param_rng_.next() % resting_orders_.size());
        auto it = resting_orders_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(offset));
        return it->first;
    }

    // ================================================================
    //  Cancel cluster generation
    // ================================================================

    void enqueue_cluster_pending() {
        int const n = draw_cluster_size();
        if (n <= 1) return;

        std::vector<PendingOp> cluster_ops;
        cluster_ops.reserve(static_cast<std::size_t>(n - 1));

        std::int64_t const best = (best_ask_ > 0) ? best_ask_ : 1000;
        for (int i = 0; i < n && !resting_orders_.empty(); ++i) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                std::uint64_t const id =
                    resting_ids_[param_rng_.next() % resting_ids_.size()];
                auto const it = resting_orders_.find(id);
                if (it == resting_orders_.end()) continue;
                if (std::abs(it->second.price - best) <= 3) {
                    auto book_handle_it = book_handles_.find(id);
                    if (book_handle_it == book_handles_.end()) {
                        track_remove_predicted(id);
                        continue;
                    }
                    auto const code = book_->cancel_order(book_handle_it->second.handle);
                    if (code != matching::ErrorCode::Success) {
                        book_handles_.erase(book_handle_it);
                        track_remove_predicted(id);
                        continue;
                    }
                    matching::OrderHandle const target_handle =
                        book_handle_it->second.handle;
                    book_handles_.erase(book_handle_it);
                    PendingOp op;
                    op.type = PendingOp::kCancel;
                    op.target_id = id;
                    op.target_handle = target_handle;
                    op.oid = 0;
                    cluster_ops.push_back(op);
                    track_remove_predicted(id);
                    break;
                }
            }
        }

        // pregen_queue_ is drained with pop_back(); reverse append preserves
        // the same cancel order already applied to the dry-run book.
        pregen_queue_.insert(pregen_queue_.end(),
                             cluster_ops.rbegin(), cluster_ops.rend());
    }

    void enqueue_cluster_legacy() {
        int const n = draw_cluster_size();
        if (n <= 1) return;
        std::int64_t const best = (best_ask_ > 0) ? best_ask_ : 1000;
        for (int i = 0; i < n && !resting_orders_.empty(); ++i) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                std::uint64_t const id =
                    resting_ids_[param_rng_.next() % resting_ids_.size()];
                auto const it = resting_orders_.find(id);
                if (it == resting_orders_.end()) continue;
                if (std::abs(it->second.price - best) <= 3) {
                    cluster_queue_.push_back(id);
                    break;
                }
            }
        }
    }

    int draw_cluster_size() {
        static constexpr int kSizes[] = {
            2,2,2,2,2,2,2,2,2,2,
            2,2,2,2,2,2,2,2,2,2,
            3,3,3,3,3,3,3,3,3,3,
            4,4,4,4,4,
            5,5,5,
            6,6,
            7,8,9,10,
            12,15,30,50,100,200};
        static constexpr std::size_t kSize =
            sizeof(kSizes) / sizeof(kSizes[0]);
        return kSizes[param_rng_.next() % kSize];
    }
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftMacro scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
