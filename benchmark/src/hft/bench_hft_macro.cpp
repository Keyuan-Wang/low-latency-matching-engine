/**
 * @file bench_hft_macro.cpp
 * @brief HFT macro benchmark: Zero-Intelligence model of HFT order flow.
 *
 * Generates a continuous stream of empirically-grounded order-book events:
 *   45% limit add (near best price), 48% cancel, 5% modify, 2% market.
 * Spontaneous cancel clusters (power-law size, 15% trigger probability)
 * reproduce the temporal autocorrelation observed in real HFT markets.
 *
 * The book evolves naturally — depth decay, spread formation, and price
 * movement emerge from the model without being hardcoded.
 *
 * Reference: Gode & Sunder (1993), "Allocative Efficiency of Markets with
 * Zero-Intelligence Traders." JPE 101(1), 119-137.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include "absl/container/flat_hash_map.h"

#include <cstdlib>
#include <memory>
#include <vector>

namespace {

class BenchHftMacro final : public benchmark_runner::IBenchScenario {
public:
    const char* Name() const override { return "hft_macro"; }
    [[nodiscard]] std::uint64_t max_batch_size() const override {
        return 1'000'000;
    }

    void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
        const std::uint64_t warmup_events = 500'000;
        const std::uint64_t pool = warmup_events + args.batch_size + 50000;

        book_ = std::make_unique<matching::OrderBook>(pool);
        event_rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
        param_rng_ = benchmark_runner::SplitMix64(args.seed * 1337ULL + iter_idx * 331ULL);
        id_counter_ = 1'000'000ULL;
        best_bid_ = 999;
        best_ask_ = 1000;

        // Warmup: run 500K ZI events to reach steady-state book
        resting_orders_.clear();
        resting_ids_.clear();
        level_counts_.clear();
        cluster_queue_.clear();

        // Initial seed: a few hundred adds to create initial depth
        for (std::uint64_t i = 0; i < 5000; ++i) {
            do_limit_add();
        }

        // Main warmup
        for (std::uint64_t i = 0; i < warmup_events; ++i) {
            generate_event();
        }

        update_best_prices();
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t, std::uint64_t& ok) override {
        if (generate_event()) ++ok;
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    // --- core state ---
    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 event_rng_{42};
    benchmark_runner::SplitMix64 param_rng_{42};
    std::uint64_t id_counter_ = 0;

    // --- resting order tracking ---
    absl::flat_hash_map<std::uint64_t, std::int64_t> resting_orders_;
    std::vector<std::uint64_t> resting_ids_;
    absl::flat_hash_map<std::int64_t, std::uint64_t> level_counts_;

    // --- book state ---
    std::int64_t best_bid_ = 0;
    std::int64_t best_ask_ = 1000;

    // --- cancel cluster queue ---
    std::vector<std::uint64_t> cluster_queue_;

    // ----------------------------------------------------------------
    //  Event generation
    // ----------------------------------------------------------------
    bool generate_event() {
        // 1. Drain one cluster entry (if any) — each RunOp = one event
        if (!cluster_queue_.empty()) {
            std::uint64_t target_id = cluster_queue_.back();
            cluster_queue_.pop_back();
            if (do_cancel(target_id)) return true;
            // Cluster cancel failed; fall through to generate a regular event.
        }

        // 2. Draw event type
        std::uint64_t const roll = event_rng_.next() % 100;
        if (roll < 45) return do_limit_add();       // 45%
        if (roll < 93) return do_cancel_random();   // 48%
        if (roll < 98) return do_modify();           // 5%
        return do_market();                          // 2%
    }

    // ----------------------------------------------------------------
    //  Limit add (45%)
    // ----------------------------------------------------------------
    bool do_limit_add() {
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy : matching::Side::Sell;

        // Use best_ask as the reference for both sides so bids stay near the spread
        std::int64_t const ref = (best_ask_ > 0) ? best_ask_ : 1000;

        // Exponential price offset: P(>k) = 0.63^k → ~90% within 5 ticks
        int offset = 0;
        std::uint64_t r = param_rng_.next();
        while ((r & 0xFF) < 161 && offset < 100) {
            ++offset;
            r >>= 8;
        }
        if (offset == 0) offset = 1;

        std::int64_t const price = (side == matching::Side::Buy)
                                       ? std::max<std::int64_t>(1, ref - offset - 1)
                                       : ref + offset;

        // Truncated power-law qty, range [1, 100], mode ≈ 5
        static constexpr std::uint64_t kQtyTable[32] = {
            1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4, 4, 5, 5, 5, 5,
            5, 6, 6, 6, 7, 7, 8, 9, 10, 12, 15, 20, 30, 50, 75, 100};
        std::uint64_t const qty = kQtyTable[param_rng_.next() % 32];

        std::uint64_t const oid = id_counter_++;
        auto const res = book_->add_limit_order(oid, side, price, qty, oid);
        if (res.code == matching::ErrorCode::Success && res.remaining_quantity > 0) {
            track_add(oid, price);
            if (side == matching::Side::Buy && price > best_bid_) best_bid_ = price;
            if (side == matching::Side::Sell && (best_ask_ == 0 || price < best_ask_)) best_ask_ = price;
            return true;
        }
        return (res.code == matching::ErrorCode::Success);
    }

    // ----------------------------------------------------------------
    //  Cancel (48%)
    // ----------------------------------------------------------------
    bool do_cancel_random() {
        if (resting_orders_.empty()) return do_limit_add();
        std::uint64_t const target = select_cancel_target();
        if (target == 0) return do_limit_add();
        return do_cancel(target);
    }

    bool do_cancel(std::uint64_t target_id) {
        auto const code = book_->cancel_order(target_id);
        if (code == matching::ErrorCode::Success) {
            auto const it = resting_orders_.find(target_id);
            if (it != resting_orders_.end()) {
                track_remove(target_id, it->second);
            }
            // 15% chance: enqueue cancel cluster
            if ((param_rng_.next() % 100) < 15) {
                enqueue_cluster();
            }
            return true;
        }
        return false;
    }

    // ----------------------------------------------------------------
    //  Modify (5%)
    // ----------------------------------------------------------------
    bool do_modify() {
        if (resting_orders_.empty()) return do_limit_add();
        std::uint64_t const target = select_cancel_target();
        if (target == 0) return do_limit_add();

        auto const it = resting_orders_.find(target);
        std::int64_t const old_price = (it != resting_orders_.end()) ? it->second : 0;

        // New price at a nearby offset
        int const delta = 1 + static_cast<int>(param_rng_.next() % 3);
        std::int64_t const new_price = old_price + ((param_rng_.next() % 2 == 0) ? -delta : delta);
        std::uint64_t const new_qty = 1 + (param_rng_.next() % 20);
        std::uint64_t const ts = id_counter_++;
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy : matching::Side::Sell;

        auto const res = book_->modify_order(target, side, new_price, new_qty, ts);
        if (res.code == matching::ErrorCode::Success) {
            if (old_price > 0) track_remove(target, old_price);
            if (res.remaining_quantity > 0) {
                track_add(target, new_price);
                if (side == matching::Side::Buy && new_price > best_bid_) best_bid_ = new_price;
                if (side == matching::Side::Sell && (best_ask_ == 0 || new_price < best_ask_)) best_ask_ = new_price;
            }
            return true;
        }
        return false;
    }

    // ----------------------------------------------------------------
    //  Market order (2%)
    // ----------------------------------------------------------------
    bool do_market() {
        std::uint64_t const total = resting_orders_.size();
        if (total == 0) return do_limit_add();

        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy : matching::Side::Sell;

        int const mroll = static_cast<int>(param_rng_.next() % 100);
        std::uint64_t qty;
        if (mroll < 85)
            qty = std::max<std::uint64_t>(1, total / 4);   // ~1-2 levels
        else if (mroll < 98)
            qty = std::max<std::uint64_t>(1, total / 2);   // ~3-5 levels
        else
            qty = static_cast<std::uint64_t>(total * 0.8);  // 5+ levels

        std::uint64_t const oid = id_counter_++;
        auto const res = book_->add_market_order(oid, side, qty, oid);
        if (res.code == matching::ErrorCode::Success ||
            res.code == matching::ErrorCode::MarketRemainderCancelled) {
            // Remove filled orders from tracking
            for (auto const& trade : res.trades) {
                auto const rit = resting_orders_.find(trade.maker_order_id);
                if (rit != resting_orders_.end()) {
                    track_remove(trade.maker_order_id, rit->second);
                }
            }
            update_best_prices();
            return true;
        }
        return false;
    }

    // ----------------------------------------------------------------
    //  Tracking helpers
    // ----------------------------------------------------------------
    void track_add(std::uint64_t id, std::int64_t price) {
        resting_orders_[id] = price;
        resting_ids_.push_back(id);
        ++level_counts_[price];
    }

    void track_remove(std::uint64_t id, std::int64_t price) {
        resting_orders_.erase(id);
        auto lc = level_counts_.find(price);
        if (lc != level_counts_.end() && lc->second > 0) {
            --lc->second;
            if (lc->second == 0) level_counts_.erase(lc);
        }
    }

    void update_best_prices() {
        best_bid_ = 0;
        best_ask_ = 0;
        for (auto const& [price, count] : level_counts_) {
            if (count == 0) continue;
            if (price >= 1000) {
                if (best_ask_ == 0 || price < best_ask_) best_ask_ = price;
            } else {
                if (best_bid_ == 0 || price > best_bid_) best_bid_ = price;
            }
        }
        if (best_ask_ == 0) best_ask_ = 1000;
    }

    // ----------------------------------------------------------------
    //  Cancel target selection (60/30/10 zone distribution)
    // ----------------------------------------------------------------
    std::uint64_t select_cancel_target() {
        if (resting_orders_.empty()) return 0;

        int const zroll = static_cast<int>(param_rng_.next() % 100);
        int min_dist, max_dist;
        if (zroll < 60) {
            min_dist = 0; max_dist = 2;       // hot zone
        } else if (zroll < 90) {
            min_dist = 3; max_dist = 5;       // warm zone
        } else {
            min_dist = 6; max_dist = 1000000; // cold zone
        }

        std::int64_t const best = (best_ask_ > 0) ? best_ask_ : 1000;
        for (int attempt = 0; attempt < 200; ++attempt) {
            std::uint64_t const id =
                resting_ids_[param_rng_.next() % resting_ids_.size()];
            auto const it = resting_orders_.find(id);
            if (it == resting_orders_.end()) continue;
            int const dist = static_cast<int>(std::abs(it->second - best));
            if (dist >= min_dist && dist <= max_dist) return id;
        }

        // Fallback: any valid ID
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::uint64_t const id =
                resting_ids_[param_rng_.next() % resting_ids_.size()];
            if (resting_orders_.contains(id)) return id;
        }
        return 0;
    }

    // ----------------------------------------------------------------
    //  Cancel cluster
    // ----------------------------------------------------------------
    void enqueue_cluster() {
        int const n = draw_cluster_size();
        if (n <= 1) return;

        std::int64_t const best = (best_ask_ > 0) ? best_ask_ : 1000;
        for (int i = 0; i < n && !resting_orders_.empty(); ++i) {
            for (int attempt = 0; attempt < 50; ++attempt) {
                std::uint64_t const id =
                    resting_ids_[param_rng_.next() % resting_ids_.size()];
                auto const it = resting_orders_.find(id);
                if (it == resting_orders_.end()) continue;
                if (std::abs(it->second - best) <= 3) {
                    cluster_queue_.push_back(id);
                    break;
                }
            }
        }
    }

    int draw_cluster_size() {
        // Truncated power-law (α ≈ 2.5), range [2, 200], 50 entries
        static constexpr int kSizes[] = {
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  //
            2, 2, 2, 2, 2, 2, 2, 2, 2, 2,  // 40%
            3, 3, 3, 3, 3, 3, 3, 3, 3, 3,  // 20%
            4, 4, 4, 4, 4,                   // 10%
            5, 5, 5,                         //  6%
            6, 6,                            //  4%
            7, 8, 9, 10,                     //  8%
            12, 15, 30, 50, 100, 200};       // 12%
        static constexpr std::size_t kSize = sizeof(kSizes) / sizeof(kSizes[0]);
        return kSizes[param_rng_.next() % kSize];
    }
};

}  // namespace

int main(int argc, char** argv) {
    BenchHftMacro scen;
    return benchmark_runner::RunScenario(scen, argc, argv);
}
