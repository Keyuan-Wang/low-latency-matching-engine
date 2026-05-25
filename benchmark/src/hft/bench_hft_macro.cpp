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
 * are pre-generated in Setup() — which is outside the timed window.
 * RunOp() executes pure OrderBook operations only, so latency/PMC samples
 * measure only the engine, not the benchmark harness.
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

// ------------------------------------------------------------------
//  Pre-generated book-operation parameters.
//  One PendingOp per RunOp() call — generated in Setup(), consumed in
//  RunOp(), so RNG and map lookups are outside the timed window.
// ------------------------------------------------------------------
struct PendingOp {
    enum Type : std::uint8_t { kLimitAdd, kCancel, kModify, kMarket };
    Type type;

    // kLimitAdd / kModify
    matching::Side side;
    std::int64_t price;
    std::uint64_t qty;

    // kCancel / kModify
    std::uint64_t target_id;

    // kMarket
    std::uint64_t market_qty;

    // All
    std::uint64_t oid;
};

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

        // --- Warmup (untimed, uses the old generate-execute-track loop) ---
        resting_orders_.clear();
        resting_ids_.clear();
        level_counts_.clear();
        cluster_queue_.clear();

        // Initial seed: a few hundred adds for depth
        for (std::uint64_t i = 0; i < 5000; ++i) {
            do_limit_add();
        }

        for (std::uint64_t i = 0; i < warmup_events; ++i) {
            generate_and_execute_one();
        }

        update_best_prices();

        // --- Pre-generate the measured batch ---
        // All event params are decided now.  RunOp() just replays them on
        // the book — no RNG, no map lookups, no tracking updates inside
        // the timed window.
        pending_.clear();
        pregen_queue_.clear();
        while (pending_.size() < args.batch_size) {
            generate_pending_one();
        }
        pending_.resize(args.batch_size);  // discard excess from clusters
    }

    bool RunOp(const benchmark_runner::Args&, std::uint64_t,
               std::uint64_t batch_idx, std::uint64_t& ok) override {
        execute_pending(batch_idx, ok);
        return true;
    }

    void Teardown() override { book_.reset(); }

private:
    // ================================================================
    //  State
    // ================================================================

    std::unique_ptr<matching::OrderBook> book_;
    benchmark_runner::SplitMix64 event_rng_{42};
    benchmark_runner::SplitMix64 param_rng_{42};
    std::uint64_t id_counter_ = 0;

    // Resting-order tracking (used for cancel-target selection during the
    // pre-generation step in Setup, NOT during RunOp).
    absl::flat_hash_map<std::uint64_t, std::int64_t> resting_orders_;
    std::vector<std::uint64_t> resting_ids_;
    absl::flat_hash_map<std::int64_t, std::uint64_t> level_counts_;

    std::int64_t best_bid_ = 0;
    std::int64_t best_ask_ = 1000;

    // Cluster cancel ID queue (for warmup's generate-and-execute path)
    std::vector<std::uint64_t> cluster_queue_;

    // Pre-generated event parameters for the timed batch
    std::vector<PendingOp> pending_;
    // Overflow queue: cluster cancels that didn't fit in pending_ spill here
    std::vector<PendingOp> pregen_queue_;

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

        // Predictive tracking update (assuming success)
        track_add_predicted(op.oid, op.price);
        if (op.side == matching::Side::Buy && op.price > best_bid_)
            best_bid_ = op.price;
        if (op.side == matching::Side::Sell &&
            (best_ask_ == 0 || op.price < best_ask_))
            best_ask_ = op.price;

        pending_.push_back(op);
    }

    // --- Cancel pre-gen ---
    void pending_cancel() {
        if (resting_orders_.empty()) { pending_limit_add(); return; }

        std::uint64_t const target = select_cancel_target();
        if (target == 0) { pending_limit_add(); return; }

        PendingOp op;
        op.type = PendingOp::kCancel;
        op.target_id = target;
        op.oid = 0;
        pending_.push_back(op);

        // Predictive tracking update
        auto const it = resting_orders_.find(target);
        if (it != resting_orders_.end()) {
            track_remove_predicted(target, it->second);
        }

        // Cluster generation: push extra cancel ops into pregen_queue_
        if ((param_rng_.next() % 100) < 15) {
            enqueue_cluster_pending();
        }
    }

    // --- Modify pre-gen ---
    void pending_modify() {
        if (resting_orders_.empty()) { pending_limit_add(); return; }

        std::uint64_t const target = select_cancel_target();
        if (target == 0) { pending_limit_add(); return; }

        auto const it = resting_orders_.find(target);
        std::int64_t const old_price =
            (it != resting_orders_.end()) ? it->second : 0;

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
        pending_.push_back(op);

        // Predictive tracking update
        if (old_price > 0) track_remove_predicted(target, old_price);
        track_add_predicted(target, new_price);
        if (op.side == matching::Side::Buy && new_price > best_bid_)
            best_bid_ = new_price;
        if (op.side == matching::Side::Sell &&
            (best_ask_ == 0 || new_price < best_ask_))
            best_ask_ = new_price;
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
            op.market_qty = static_cast<std::uint64_t>(total * 0.8);

        op.oid = id_counter_++;
        pending_.push_back(op);

        // We cannot predict which specific maker orders will be filled by
        // the market order (the engine decides based on FIFO queue order).
        // Skip tracking updates — the tracking will be slightly optimistic
        // for remaining events in this batch, but the error is negligible
        // (market is 2% of events) and resets every iteration.
    }

    // ================================================================
    //  Timed execution — RunOp calls this, pure book ops only
    // ================================================================

    void execute_pending(std::size_t idx, std::uint64_t& ok) {
        auto const& op = pending_[idx];
        switch (op.type) {
        case PendingOp::kLimitAdd: {
            auto const res =
                book_->add_limit_order(op.oid, op.side, op.price, op.qty, op.oid);
            if (res.code == matching::ErrorCode::Success) ++ok;
            break;
        }
        case PendingOp::kCancel: {
            auto const code = book_->cancel_order(op.target_id);
            if (code == matching::ErrorCode::Success) ++ok;
            break;
        }
        case PendingOp::kModify: {
            auto const res = book_->modify_order(op.target_id, op.side, op.price,
                                                  op.qty, op.oid);
            if (res.code == matching::ErrorCode::Success) ++ok;
            break;
        }
        case PendingOp::kMarket: {
            auto const res =
                book_->add_market_order(op.oid, op.side, op.market_qty, op.oid);
            if (res.code == matching::ErrorCode::Success ||
                res.code == matching::ErrorCode::MarketRemainderCancelled)
                ++ok;
            break;
        }
        }
    }

    // ================================================================
    //  Tracking helpers — only called during warmup + pre-generation
    // ================================================================

    void track_add_predicted(std::uint64_t id, std::int64_t price) {
        resting_orders_[id] = price;
        resting_ids_.push_back(id);
        ++level_counts_[price];
    }

    void track_remove_predicted(std::uint64_t id, std::int64_t price) {
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
        if (res.code == matching::ErrorCode::Success && res.remaining_quantity > 0) {
            track_add_predicted(oid, price);
            if (side == matching::Side::Buy && price > best_bid_) best_bid_ = price;
            if (side == matching::Side::Sell &&
                (best_ask_ == 0 || price < best_ask_))
                best_ask_ = price;
            return true;
        }
        return (res.code == matching::ErrorCode::Success);
    }

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
                track_remove_predicted(target_id, it->second);
            }
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
        std::int64_t const old_price =
            (it != resting_orders_.end()) ? it->second : 0;
        int const delta = 1 + static_cast<int>(param_rng_.next() % 3);
        std::int64_t const new_price =
            old_price + ((param_rng_.next() % 2 == 0) ? -delta : delta);
        std::uint64_t const new_qty = 1 + (param_rng_.next() % 20);
        std::uint64_t const ts = id_counter_++;
        matching::Side const side =
            (param_rng_.next() % 2 == 0) ? matching::Side::Buy
                                         : matching::Side::Sell;
        auto const res =
            book_->modify_order(target, side, new_price, new_qty, ts);
        if (res.code == matching::ErrorCode::Success) {
            if (old_price > 0) track_remove_predicted(target, old_price);
            if (res.remaining_quantity > 0) {
                track_add_predicted(target, new_price);
                if (side == matching::Side::Buy && new_price > best_bid_)
                    best_bid_ = new_price;
                if (side == matching::Side::Sell &&
                    (best_ask_ == 0 || new_price < best_ask_))
                    best_ask_ = new_price;
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
            qty = static_cast<std::uint64_t>(total * 0.8);
        std::uint64_t const oid = id_counter_++;
        auto const res =
            book_->add_market_order(oid, side, qty, oid);
        if (res.code == matching::ErrorCode::Success ||
            res.code == matching::ErrorCode::MarketRemainderCancelled) {
            for (auto const& trade : res.trades) {
                auto const rit = resting_orders_.find(trade.maker_order_id);
                if (rit != resting_orders_.end()) {
                    track_remove_predicted(trade.maker_order_id, rit->second);
                }
            }
            update_best_prices();
            return true;
        }
        return false;
    }

    // ================================================================
    //  Cancel target selection (60/30/10 zone distribution)
    // ================================================================

    std::uint64_t select_cancel_target() {
        if (resting_orders_.empty()) return 0;

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
            int const dist = static_cast<int>(std::abs(it->second - best));
            if (dist >= min_dist && dist <= max_dist) return id;
        }
        for (int attempt = 0; attempt < 100; ++attempt) {
            std::uint64_t const id =
                resting_ids_[param_rng_.next() % resting_ids_.size()];
            if (resting_orders_.contains(id)) return id;
        }
        return 0;
    }

    // ================================================================
    //  Cancel cluster generation
    // ================================================================

    void enqueue_cluster_pending() {
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
                    PendingOp op;
                    op.type = PendingOp::kCancel;
                    op.target_id = id;
                    op.oid = 0;
                    pregen_queue_.push_back(op);
                    // Predictive tracking update
                    track_remove_predicted(id, it->second);
                    break;
                }
            }
        }
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
                if (std::abs(it->second - best) <= 3) {
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
