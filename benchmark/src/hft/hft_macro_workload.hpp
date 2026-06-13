#pragma once

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace benchmark_runner::hft {

enum class MacroScenario : std::uint8_t {
	AddRestExistingLevel = 0,
	AddRestNewLevel = 1,
	CancelOrder = 2,
	Unmeasured = 3,
	Count = 4,
};

inline constexpr std::size_t kMacroScenarioCount =
		static_cast<std::size_t>(MacroScenario::Count);

inline std::size_t ScenarioIndex(MacroScenario scenario) noexcept {
	return static_cast<std::size_t>(scenario);
}

inline const char* ScenarioName(MacroScenario scenario) noexcept {
	switch (scenario) {
	case MacroScenario::AddRestExistingLevel:
		return "add_rest_existing_level";
	case MacroScenario::AddRestNewLevel:
		return "add_rest_new_level";
	case MacroScenario::CancelOrder:
		return "cancel_order";
	case MacroScenario::Unmeasured:
		return "unmeasured";
	case MacroScenario::Count:
		break;
	}
	return "unknown";
}

inline bool IsMeasuredScenario(MacroScenario scenario) noexcept {
	return scenario == MacroScenario::AddRestExistingLevel ||
				 scenario == MacroScenario::AddRestNewLevel ||
				 scenario == MacroScenario::CancelOrder;
}

enum class OccupancySetPath : std::uint8_t {
	NotApplicable = 0,
	TargetAlreadySet = 1,
	L1Only = 2,
	ReachedL2 = 3,
};

inline const char* OccupancySetPathName(OccupancySetPath path) noexcept {
	switch (path) {
	case OccupancySetPath::NotApplicable:
		return "not_applicable";
	case OccupancySetPath::TargetAlreadySet:
		return "target_already_set";
	case OccupancySetPath::L1Only:
		return "l1_only";
	case OccupancySetPath::ReachedL2:
		return "reached_l2";
	}
	return "unknown";
}

inline constexpr std::uint64_t kNoPreviousLevelTouch =
		std::numeric_limits<std::uint64_t>::max();

struct PendingAttribution {
	OccupancySetPath occupancy_set_path = OccupancySetPath::NotApplicable;
	std::uint16_t occupancy_l1_popcount_before = 0;
	std::uint8_t price_mod8 = 0;
	std::uint64_t level_reuse_distance_ops = kNoPreviousLevelTouch;
};

// ------------------------------------------------------------------
//  Pre-generated book-operation parameters.
//  One PendingOp per measured book operation.  Event generation,
//  cancel-target selection, tracking-map updates, and scenario tagging all
//  happen in Setup(); Execute() only replays the selected OrderBook call.
// ------------------------------------------------------------------
struct PendingOp {
	enum Type : std::uint8_t { kLimitAdd, kCancel, kModify, kMarket };
	Type type = kLimitAdd;
	MacroScenario scenario = MacroScenario::Unmeasured;

	// kLimitAdd / kModify
	matching::Side side = matching::Side::Buy;
	std::int64_t price = 0;
	std::uint64_t qty = 0;

	// kCancel / kModify
	std::uint64_t target_id = 0;
	matching::OrderHandle target_handle{matching::kInvalidHandle};

	// kMarket
	std::uint64_t market_qty = 0;

	// All
	std::uint64_t oid = 0;
};

template <bool EnableAttribution = false>
class HftMacroWorkload final {
public:
	void Setup(const Args& args, std::uint64_t iter_idx) {
		const std::uint64_t warmup_events = 500'000;
		const std::uint64_t pool = warmup_events + args.batch_size + 50000;

		book_ = std::make_unique<matching::OrderBook>(pool);
		event_rng_ = SplitMix64(args.seed + args.trial_id * 1000003ULL +
														iter_idx * 9973ULL);
		param_rng_ = SplitMix64(args.seed * 1337ULL + args.trial_id * 500009ULL +
														iter_idx * 331ULL);
		id_counter_ = 1'000'000ULL;
		best_bid_ = 999;
		best_ask_ = 1000;

		// --- Warmup (untimed, uses the old generate-execute-track loop) ---
		resting_orders_.clear();
		book_handles_.clear();
		resting_ids_.clear();
		ask_level_counts_.clear();
		bid_level_counts_.clear();
		cluster_queue_.clear();

		// Initial seed: a few thousand adds for depth.
		for (std::uint64_t i = 0; i < 5000; ++i) {
			do_limit_add();
		}

		for (std::uint64_t i = 0; i < warmup_events; ++i) {
			generate_and_execute_one();
		}

		update_best_prices();
		const auto base_resting_orders = resting_orders_;
		book_ = build_book_from_tracking(pool, book_handles_);
		if constexpr (EnableAttribution) reset_attribution_state();

		// --- Pre-generate the measured batch ---
		// All event params and handles are decided now. Execute() just calls
		// the requested OrderBook operation.
		pending_.clear();
		pregen_queue_.clear();
		if constexpr (EnableAttribution) {
			pending_attribution_.clear();
			pregen_attribution_queue_.clear();
		}
		while (pending_.size() < args.batch_size) {
			generate_pending_one();
		}
		pending_.resize(args.batch_size);  // discard excess from clusters
		if constexpr (EnableAttribution) {
			pending_attribution_.resize(args.batch_size);
		}

		resting_orders_ = base_resting_orders;
		book_ = build_book_from_tracking(pool, book_handles_);
	}

	bool Execute(std::size_t idx, std::uint64_t& ok) {
		execute_pending(idx, ok);
		return true;
	}

	void Teardown() {
		book_.reset();
	}

	[[nodiscard]] std::size_t size() const noexcept { return pending_.size(); }

	[[nodiscard]] const PendingOp& pending(std::size_t idx) const noexcept {
		return pending_[idx];
	}

	[[nodiscard]] const PendingAttribution& attribution(
			std::size_t idx) const noexcept {
		static_assert(EnableAttribution);
		return pending_attribution_[idx];
	}

	[[nodiscard]] MacroScenario scenario(std::size_t idx) const noexcept {
		return pending_[idx].scenario;
	}

private:
	class ShadowOccupancyTree {
	public:
		static constexpr std::size_t kBitCount = matching::OccupancyTree::kBitCount;
		static constexpr std::size_t kL1WordCount = kBitCount / 64;

		void reset() noexcept {
			l1_.fill(0);
			l2_ = 0;
		}

		[[nodiscard]] OccupancySetPath classify_set(std::size_t bit) const noexcept {
			const std::size_t l1_idx = bit / 64;
			const std::uint64_t l1_mask = 1ULL << (bit & 63);
			const std::uint64_t l1_word = l1_[l1_idx];
			if ((l1_word & l1_mask) != 0) {
				return OccupancySetPath::TargetAlreadySet;
			}
			if (l1_word != 0) return OccupancySetPath::L1Only;
			return OccupancySetPath::ReachedL2;
		}

		[[nodiscard]] std::uint16_t l1_popcount(std::size_t bit) const noexcept {
			return static_cast<std::uint16_t>(std::popcount(l1_[bit / 64]));
		}

		void set(std::size_t bit) noexcept {
			const std::size_t l1_idx = bit / 64;
			const std::uint64_t l1_mask = 1ULL << (bit & 63);
			if ((l1_[l1_idx] & l1_mask) != 0) return;

			const bool l1_was_empty = l1_[l1_idx] == 0;
			l1_[l1_idx] |= l1_mask;
			if (!l1_was_empty) return;

			l2_ |= 1ULL << l1_idx;
		}

		void clear(std::size_t bit) noexcept {
			const std::size_t l1_idx = bit / 64;
			const std::uint64_t l1_mask = 1ULL << (bit & 63);
			if ((l1_[l1_idx] & l1_mask) == 0) return;
			l1_[l1_idx] &= ~l1_mask;
			if (l1_[l1_idx] != 0) return;

			l2_ &= ~(1ULL << l1_idx);
		}

		template <bool IsAsk>
		[[nodiscard]] std::optional<std::size_t> best() const noexcept {
			if (l2_ == 0) return std::nullopt;
			if constexpr (IsAsk) {
				const std::size_t l1_idx = std::countr_zero(l2_);
				return l1_idx * 64 + std::countr_zero(l1_[l1_idx]);
			} else {
				const std::size_t l1_idx = 63 - std::countl_zero(l2_);
				return l1_idx * 64 + (63 - std::countl_zero(l1_[l1_idx]));
			}
		}

	private:
		std::array<std::uint64_t, kL1WordCount> l1_{};
		std::uint64_t l2_ = 0;
	};

	struct RestingMeta {
		matching::Side side = matching::Side::Buy;
		std::int64_t price = 0;
		std::uint64_t qty = 0;
	};

	struct HandleMeta {
		matching::OrderHandle handle = matching::kInvalidHandle;
		std::uint64_t qty = 0;
	};

	std::unique_ptr<matching::OrderBook> book_;
	SplitMix64 event_rng_{42};
	SplitMix64 param_rng_{42};
	std::uint64_t id_counter_ = 0;

	// Resting-order tracking, used only for cancel-target selection during
	// warmup and pre-generation. It is not touched by the measured replay.
	std::unordered_map<std::uint64_t, RestingMeta> resting_orders_;
	std::unordered_map<std::uint64_t, HandleMeta> book_handles_;
	std::vector<std::uint64_t> resting_ids_;
	std::map<std::int64_t, std::uint64_t, std::less<>> ask_level_counts_;
	std::map<std::int64_t, std::uint64_t, std::greater<>> bid_level_counts_;

	std::int64_t best_bid_ = 0;
	std::int64_t best_ask_ = 1000;

	std::vector<std::uint64_t> cluster_queue_;
	std::vector<PendingOp> pending_;
	std::vector<PendingAttribution> pending_attribution_;
	std::vector<PendingOp> pregen_queue_;
	std::vector<PendingAttribution> pregen_attribution_queue_;
	ShadowOccupancyTree bid_shadow_tree_;
	ShadowOccupancyTree ask_shadow_tree_;
	std::unordered_map<std::int64_t, std::int64_t> bid_last_level_touch_;
	std::unordered_map<std::int64_t, std::int64_t> ask_last_level_touch_;
	std::uint64_t attribution_op_index_ = 0;

	[[nodiscard]] ShadowOccupancyTree& shadow_tree(matching::Side side) noexcept {
		return side == matching::Side::Buy ? bid_shadow_tree_ : ask_shadow_tree_;
	}

	[[nodiscard]] std::unordered_map<std::int64_t, std::int64_t>&
	last_level_touch(matching::Side side) noexcept {
		return side == matching::Side::Buy ? bid_last_level_touch_
																	 : ask_last_level_touch_;
	}

	[[nodiscard]] static std::size_t price_index(std::int64_t price) noexcept {
		return static_cast<std::size_t>(price);
	}

	void reset_attribution_state() {
		bid_shadow_tree_.reset();
		ask_shadow_tree_.reset();
		bid_last_level_touch_.clear();
		ask_last_level_touch_.clear();
		attribution_op_index_ = 0;

		for (const auto& [price, _] : bid_level_counts_) {
			bid_shadow_tree_.set(price_index(price));
			bid_last_level_touch_[price] = -1;
		}
		for (const auto& [price, _] : ask_level_counts_) {
			ask_shadow_tree_.set(price_index(price));
			ask_last_level_touch_[price] = -1;
		}
	}

	void annotate_level_reuse(PendingAttribution& attribution,
			matching::Side side, std::int64_t price) {
		if constexpr (!EnableAttribution) return;
		attribution.price_mod8 = static_cast<std::uint8_t>(
				(static_cast<std::uint64_t>(price) & 7ULL));
		auto& touches = last_level_touch(side);
		const auto it = touches.find(price);
		if (it == touches.end()) {
			attribution.level_reuse_distance_ops = kNoPreviousLevelTouch;
			return;
		}
		const std::int64_t current =
				static_cast<std::int64_t>(attribution_op_index_);
		attribution.level_reuse_distance_ops =
				static_cast<std::uint64_t>(current - it->second);
	}

	void annotate_add(PendingAttribution& attribution,
			matching::Side side, std::int64_t price) {
		if constexpr (!EnableAttribution) return;
		annotate_level_reuse(attribution, side, price);
		auto& tree = shadow_tree(side);
		const std::size_t idx = price_index(price);
		attribution.occupancy_set_path = tree.classify_set(idx);
		attribution.occupancy_l1_popcount_before = tree.l1_popcount(idx);
	}

	void touch_level(matching::Side side, std::int64_t price) {
		if constexpr (!EnableAttribution) return;
		last_level_touch(side)[price] =
				static_cast<std::int64_t>(attribution_op_index_);
	}

	void set_shadow_level(matching::Side side, std::int64_t price) noexcept {
		if constexpr (!EnableAttribution) return;
		shadow_tree(side).set(price_index(price));
	}

	void clear_shadow_level(matching::Side side, std::int64_t price) noexcept {
		if constexpr (!EnableAttribution) return;
		shadow_tree(side).clear(price_index(price));
	}

	[[nodiscard]] std::optional<std::int64_t> shadow_best_price(
			matching::Side side) const noexcept {
		if (side == matching::Side::Buy) {
			const auto best = bid_shadow_tree_.template best<false>();
			return best ? std::optional<std::int64_t>(*best) : std::nullopt;
		}
		const auto best = ask_shadow_tree_.template best<true>();
		return best ? std::optional<std::int64_t>(*best) : std::nullopt;
	}

	[[nodiscard]] static bool shadow_can_cross(
			matching::Side taker_side,
			std::int64_t limit_price,
			std::int64_t best_opposite_price) noexcept {
		return taker_side == matching::Side::Buy
						 ? limit_price >= best_opposite_price
						 : limit_price <= best_opposite_price;
	}

	void apply_matching_attribution(
			matching::Side taker_side,
			std::optional<std::int64_t> limit_price,
			const std::vector<matching::Trade>& trades) {
		if constexpr (!EnableAttribution) return;
		const matching::Side maker_side = taker_side == matching::Side::Buy
																	 ? matching::Side::Sell
																	 : matching::Side::Buy;
		for (const auto& trade : trades) {
			touch_level(maker_side, trade.price);
		}

		// A matching loop touches and removes leading ghost/emptied levels until
		// it reaches a live level, a non-crossing limit, or an empty book.
		bool cleared_any = false;
		while (true) {
			const auto best = shadow_best_price(maker_side);
			if (!best) return;
			if (has_price_level(maker_side, *best)) {
				// clear_ghost_best_level() probes the first live level before it stops.
				if (cleared_any) touch_level(maker_side, *best);
				return;
			}
			if (limit_price &&
					!shadow_can_cross(taker_side, *limit_price, *best)) {
				return;
			}
			touch_level(maker_side, *best);
			clear_shadow_level(maker_side, *best);
			cleared_any = true;
		}
	}

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

	void generate_pending_one() {
		if (!pregen_queue_.empty()) {
			pending_.push_back(pregen_queue_.back());
			if constexpr (EnableAttribution) {
				pending_attribution_.push_back(pregen_attribution_queue_.back());
			}
			pregen_queue_.pop_back();
			if constexpr (EnableAttribution) {
				pregen_attribution_queue_.pop_back();
			}
			return;
		}

		std::uint64_t const roll = event_rng_.next() % 100;
		if (roll < 45) { pending_limit_add(); return; }
		if (roll < 93) { pending_cancel(); return; }
		if (roll < 98) { pending_modify(); return; }
		pending_market();
	}

	void pending_limit_add() {
		PendingOp op;
		op.type = PendingOp::kLimitAdd;
		op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
																					 : matching::Side::Sell;

		std::int64_t const ref = (best_ask_ > 0) ? best_ask_ : 1000;
		int offset = 0;
		std::uint64_t r = param_rng_.next();
		while ((r & 0xFF) < 161 && offset < 100) {
			++offset;
			r >>= 8;
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
		const bool level_existed_before = has_price_level(op.side, op.price);
		PendingAttribution attribution;
		annotate_add(attribution, op.side, op.price);

		auto const res =
				book_->add_limit_order(op.oid, op.side, op.price, op.qty, op.oid);
		if (res.code == matching::ErrorCode::Success && res.trades.empty() &&
				res.remaining_quantity > 0) {
			op.scenario = level_existed_before
												? MacroScenario::AddRestExistingLevel
												: MacroScenario::AddRestNewLevel;
		} else {
			op.scenario = MacroScenario::Unmeasured;
		}
		apply_trade_fills(res.trades, &book_handles_);
		apply_matching_attribution(op.side, op.price, res.trades);
		if (res.code == matching::ErrorCode::Success &&
				res.remaining_quantity > 0) {
			track_add_predicted(op.oid, op.side, op.price, res.remaining_quantity);
			book_handles_[op.oid] = HandleMeta{res.handle, res.remaining_quantity};
			set_shadow_level(op.side, op.price);
			touch_level(op.side, op.price);
		}

		pending_.push_back(op);
		if constexpr (EnableAttribution) {
			pending_attribution_.push_back(attribution);
		}
		++attribution_op_index_;
	}

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
			PendingAttribution attribution;
			op.type = PendingOp::kCancel;
			op.scenario = MacroScenario::CancelOrder;
			if constexpr (EnableAttribution) {
				const auto resting_it = resting_orders_.find(target);
				if (resting_it != resting_orders_.end()) {
					op.side = resting_it->second.side;
					op.price = resting_it->second.price;
					op.qty = resting_it->second.qty;
				}
			}
			op.target_id = target;
			op.target_handle = target_handle;
			op.oid = 0;
			annotate_level_reuse(attribution, op.side, op.price);
			pending_.push_back(op);
			if constexpr (EnableAttribution) {
				pending_attribution_.push_back(attribution);
			}
			touch_level(op.side, op.price);
			track_remove_predicted(target);
			++attribution_op_index_;

			if ((param_rng_.next() % 100) < 15) {
				enqueue_cluster_pending();
			}
			return;
		}

		pending_limit_add();
	}

	void pending_modify() {
		if (resting_orders_.empty()) { pending_limit_add(); return; }

		std::uint64_t const target = select_cancel_target();
		if (target == 0) { pending_limit_add(); return; }

		auto const it = resting_orders_.find(target);
		if (it == resting_orders_.end()) { pending_limit_add(); return; }
		const RestingMeta old_meta = it->second;
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

		auto const res = book_->modify_order(book_handle_it->second.handle, op.side,
																				 op.price, op.qty, op.oid);
		if (res.code != matching::ErrorCode::Success) {
			book_handles_.erase(book_handle_it);
			track_remove_predicted(target);
			pending_limit_add();
			return;
		}

		op.scenario = MacroScenario::Unmeasured;
		touch_level(old_meta.side, old_meta.price);
		book_handles_.erase(book_handle_it);
		track_remove_predicted(target);
		apply_trade_fills(res.trades, &book_handles_);
		apply_matching_attribution(op.side, op.price, res.trades);
		if (res.remaining_quantity > 0) {
			track_add_predicted(target, op.side, op.price, res.remaining_quantity);
			book_handles_[target] = HandleMeta{res.handle, res.remaining_quantity};
			set_shadow_level(op.side, op.price);
			touch_level(op.side, op.price);
		}
		pending_.push_back(op);
		if constexpr (EnableAttribution) {
			pending_attribution_.push_back(PendingAttribution{});
		}
		++attribution_op_index_;
	}

	void pending_market() {
		std::uint64_t const total = resting_orders_.size();
		if (total == 0) { pending_limit_add(); return; }

		PendingOp op;
		op.type = PendingOp::kMarket;
		op.scenario = MacroScenario::Unmeasured;
		op.side = (param_rng_.next() % 2 == 0) ? matching::Side::Buy
																					 : matching::Side::Sell;

		int const mroll = static_cast<int>(param_rng_.next() % 100);
		if (mroll < 85) {
			op.market_qty = std::max<std::uint64_t>(1, total / 4);
		} else if (mroll < 98) {
			op.market_qty = std::max<std::uint64_t>(1, total / 2);
		} else {
			op.market_qty =
					std::max<std::uint64_t>(1, static_cast<std::uint64_t>(total * 0.8));
		}

		op.oid = id_counter_++;
		auto const res =
				book_->add_market_order(op.oid, op.side, op.market_qty, op.oid);
		apply_trade_fills(res.trades, &book_handles_);
		apply_matching_attribution(op.side, std::nullopt, res.trades);
		pending_.push_back(op);
		if constexpr (EnableAttribution) {
			pending_attribution_.push_back(PendingAttribution{});
		}
		++attribution_op_index_;
	}

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
			auto const code = book_->cancel_order(op.target_handle);
			if (code == matching::ErrorCode::Success) ++ok;
			break;
		}
		case PendingOp::kModify: {
			auto const res = book_->modify_order(op.target_handle, op.side, op.price,
																					 op.qty, op.oid);
			if (res.code == matching::ErrorCode::Success) ++ok;
			break;
		}
		case PendingOp::kMarket: {
			auto const res =
					book_->add_market_order(op.oid, op.side, op.market_qty, op.oid);
			if (res.code == matching::ErrorCode::Success ||
					res.code == matching::ErrorCode::MarketRemainderCancelled) {
				++ok;
			}
			break;
		}
		}
	}

	void track_add_predicted(std::uint64_t id, matching::Side side,
													 std::int64_t price, std::uint64_t qty) {
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
												 std::unordered_map<std::uint64_t, HandleMeta>* handles =
														 nullptr) {
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

	std::unique_ptr<matching::OrderBook> build_book_from_tracking(
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

	[[nodiscard]] bool has_price_level(matching::Side side,
																		 std::int64_t price) const {
		if (side == matching::Side::Buy) {
			return bid_level_counts_.contains(price);
		}
		return ask_level_counts_.contains(price);
	}

	bool do_limit_add() {
		matching::Side const side =
				(param_rng_.next() % 2 == 0) ? matching::Side::Buy
																		 : matching::Side::Sell;
		std::int64_t const ref = (best_ask_ > 0) ? best_ask_ : 1000;
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
		static constexpr std::uint64_t kQtyTable[32] = {
				1,1,1,2,2,2,3,3,3,4,4,4,5,5,5,5,
				5,6,6,6,7,7,8,9,10,12,15,20,30,50,75,100};
		std::uint64_t const qty = kQtyTable[param_rng_.next() % 32];
		std::uint64_t const oid = id_counter_++;
		auto const res = book_->add_limit_order(oid, side, price, qty, oid);
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
		if (mroll < 85) {
			qty = std::max<std::uint64_t>(1, total / 4);
		} else if (mroll < 98) {
			qty = std::max<std::uint64_t>(1, total / 2);
		} else {
			qty = std::max<std::uint64_t>(
					1, static_cast<std::uint64_t>(total * 0.8));
		}
		std::uint64_t const oid = id_counter_++;
		auto const res = book_->add_market_order(oid, side, qty, oid);
		if (res.code == matching::ErrorCode::Success ||
				res.code == matching::ErrorCode::MarketRemainderCancelled) {
			apply_trade_fills(res.trades, &book_handles_);
			return true;
		}
		return false;
	}

	std::uint64_t select_cancel_target() {
		if (resting_orders_.empty()) return 0;
		if (resting_ids_.empty() ||
				resting_ids_.size() > resting_orders_.size() * 8) {
			compact_resting_ids();
		}

		int const zroll = static_cast<int>(param_rng_.next() % 100);
		int min_dist;
		int max_dist;
		if (zroll < 60) {
			min_dist = 0;
			max_dist = 2;
		} else if (zroll < 90) {
			min_dist = 3;
			max_dist = 5;
		} else {
			min_dist = 6;
			max_dist = 1000000;
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
		std::size_t offset =
				static_cast<std::size_t>(param_rng_.next() % resting_orders_.size());
		auto it = resting_orders_.begin();
		std::advance(it, static_cast<std::ptrdiff_t>(offset));
		return it->first;
	}

	void enqueue_cluster_pending() {
		int const n = draw_cluster_size();
		if (n <= 1) return;

		std::vector<PendingOp> cluster_ops;
		std::vector<PendingAttribution> cluster_attributions;
		cluster_ops.reserve(static_cast<std::size_t>(n - 1));
		if constexpr (EnableAttribution) {
			cluster_attributions.reserve(static_cast<std::size_t>(n - 1));
		}

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
					PendingAttribution attribution;
					op.type = PendingOp::kCancel;
					op.scenario = MacroScenario::CancelOrder;
					if constexpr (EnableAttribution) {
						op.side = it->second.side;
						op.price = it->second.price;
						op.qty = it->second.qty;
					}
					op.target_id = id;
					op.target_handle = target_handle;
					op.oid = 0;
					annotate_level_reuse(attribution, op.side, op.price);
					cluster_ops.push_back(op);
					if constexpr (EnableAttribution) {
						cluster_attributions.push_back(attribution);
					}
					touch_level(op.side, op.price);
					track_remove_predicted(id);
					++attribution_op_index_;
					break;
				}
			}
		}

		// pregen_queue_ is drained with pop_back(); reverse append preserves
		// the same cancel order already applied to the dry-run book.
		pregen_queue_.insert(pregen_queue_.end(), cluster_ops.rbegin(),
										 cluster_ops.rend());
		if constexpr (EnableAttribution) {
			pregen_attribution_queue_.insert(pregen_attribution_queue_.end(),
													 cluster_attributions.rbegin(),
													 cluster_attributions.rend());
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
		static constexpr std::size_t kSize = sizeof(kSizes) / sizeof(kSizes[0]);
		return kSizes[param_rng_.next() % kSize];
	}
};

}  // namespace benchmark_runner::hft
