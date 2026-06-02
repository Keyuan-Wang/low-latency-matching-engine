/**
 * @file bench_overall.cpp
 * @brief Overall throughput benchmark: mixed operations on a single book.
 *
 * Prefills both sides of the book, then runs a fixed mix of operation types
 * (35% cancel, 30% modify, 25% limit rest, 5% limit cross, 5% market) in a single long
 * run. Uses iters=1 + a large batch_size so the book evolves realistically
 * throughout the measurement window.  Reports ops_s as total_ops / total_seconds
 * — a single global throughput metric for comparing versions.
 *
 * Randomness is fully deterministic via SplitMix64 seeded from Args::seed +
 * iter_idx, so runs are reproducible across versions and machines.
 */

#include "benchmark_runner.hpp"
#include "bench_common.hpp"

#include <memory>
#include <unordered_map>
#include <vector>

namespace {

class OverallScenario : public benchmark_runner::IBenchScenario {
public:
	const char* Name() const override { return "overall"; }
	[[nodiscard]] std::uint64_t max_batch_size() const override {
		return 1'000'000;
	}

	void Setup(const benchmark_runner::Args& args, std::uint64_t iter_idx) override {
		// Pool must hold: prefilled + new ops during run
		const std::uint64_t pool_size =
			2 * args.orders + 2 * args.levels + args.batch_size + 5000;
		book_ = std::make_unique<matching::OrderBook>(pool_size);
		rng_ = benchmark_runner::SplitMix64(args.seed + iter_idx * 9973ULL);
		op_rng_ = benchmark_runner::SplitMix64(args.seed * 1337ULL + iter_idx * 331ULL);
		live_.clear();
		live_ids_.clear();

		const std::uint64_t per_level =
			std::max<std::uint64_t>(1, args.orders / std::max<std::uint64_t>(1, args.levels));

		// Prefill asks at prices 1000+
		std::uint64_t id = 100'000ULL;
		for (std::uint64_t lvl = 0; lvl < args.levels; ++lvl) {
			const std::int64_t ask_price = 1000 + static_cast<std::int64_t>(lvl);
			for (std::uint64_t j = 0; j < per_level; ++j) {
				const auto res =
					book_->add_limit_order(id, matching::Side::Sell, ask_price, 1, id);
				track_add(id, res);
				++id;
			}
		}

		// Prefill bids at prices below 1000 (will not cross with asks)
		for (std::uint64_t lvl = 0; lvl < args.levels; ++lvl) {
			const std::int64_t bid_price = 999 - static_cast<std::int64_t>(lvl);
			for (std::uint64_t j = 0; j < per_level; ++j) {
				const auto res =
					book_->add_limit_order(id, matching::Side::Buy, bid_price, 1, id);
				track_add(id, res);
				++id;
			}
		}

		id_counter_ = 1'000'000ULL;
	}

	bool RunOp(const benchmark_runner::Args& args, std::uint64_t,
			   std::uint64_t, std::uint64_t& ok) override {
		const std::uint64_t roll = op_rng_.next() % 100;

		if (roll < 35) {
			// 35%: Cancel a random live resting order.
			const std::uint64_t cancel_id = select_live_id();
			if (cancel_id == 0) {
				run_resting_limit(ok);
				return true;
			}
			const auto it = live_.find(cancel_id);
			if (it == live_.end()) {
				run_resting_limit(ok);
				return true;
			}
			const auto code = book_->cancel_order(it->second.handle);
			live_.erase(it);
			if (code == matching::ErrorCode::Success) ++ok;

		} else if (roll < 65) {
			// 30%: Modify a random live resting order.
			const std::uint64_t mod_id = select_live_id();
			if (mod_id == 0) {
				run_resting_limit(ok);
				return true;
			}
			const std::int64_t price = 1 + static_cast<std::int64_t>(rng_.next() % 998);
			const std::uint64_t ts = id_counter_++;
			const auto it = live_.find(mod_id);
			if (it == live_.end()) {
				run_resting_limit(ok);
				return true;
			}
			const auto res =
				book_->modify_order(it->second.handle, matching::Side::Buy, price, 1, ts);
			live_.erase(it);
			track_result(mod_id, res);
			if (res.code == matching::ErrorCode::Success) ++ok;

		} else if (roll < 90) {
			// 25%: Limit order — resting (non-crossing price)
			run_resting_limit(ok);

		} else if (roll < 95) {
			// 5%: Limit order — crosses the spread
			const std::uint64_t oid = id_counter_++;
			const std::int64_t price = 1000 + static_cast<std::int64_t>(rng_.next() % 10);
			const auto res = book_->add_limit_order(oid, matching::Side::Buy, price, 5, oid);
			track_result(oid, res);
			if (res.code == matching::ErrorCode::Success ||
				res.code == matching::ErrorCode::MarketRemainderCancelled) ++ok;

		} else {
			// 5%: Market order
			const std::uint64_t oid = id_counter_++;
			const auto res = book_->add_market_order(oid, matching::Side::Buy, 10, oid);
			track_result(oid, res);
			if (res.code == matching::ErrorCode::Success ||
				res.code == matching::ErrorCode::MarketRemainderCancelled) ++ok;
		}

		return true;
	}

	void Teardown() override { book_.reset(); }

private:
	struct LiveOrder {
		matching::OrderHandle handle = matching::kInvalidHandle;
		std::uint64_t quantity = 0;
	};

	void track_add(std::uint64_t id, const matching::AddResult& res) {
		if (res.code == matching::ErrorCode::Success &&
			res.remaining_quantity > 0 &&
			res.handle != matching::kInvalidHandle) {
			live_[id] = LiveOrder{res.handle, res.remaining_quantity};
			live_ids_.push_back(id);
		}
	}

	void track_result(std::uint64_t id, const matching::AddResult& res) {
		for (const auto& trade : res.trades) {
			auto it = live_.find(trade.maker_order_id);
			if (it == live_.end()) continue;
			if (trade.quantity >= it->second.quantity) {
				live_.erase(it);
			} else {
				it->second.quantity -= trade.quantity;
			}
		}
		track_add(id, res);
	}

	void run_resting_limit(std::uint64_t& ok) {
		const std::uint64_t oid = id_counter_++;
		const std::int64_t price = 1 + static_cast<std::int64_t>(rng_.next() % 998);
		const auto res = book_->add_limit_order(oid, matching::Side::Buy, price, 1, oid);
		track_result(oid, res);
		if (res.code == matching::ErrorCode::Success) ++ok;
	}

	std::uint64_t select_live_id() {
		if (live_.empty()) return 0;
		if (live_ids_.empty() || live_ids_.size() > live_.size() * 8) {
			live_ids_.clear();
			live_ids_.reserve(live_.size());
			for (const auto& [id, _] : live_) live_ids_.push_back(id);
		}
		for (int attempt = 0; attempt < 16 && !live_ids_.empty(); ++attempt) {
			const auto id = live_ids_[rng_.next() % live_ids_.size()];
			if (live_.contains(id)) return id;
		}
		live_ids_.clear();
		live_ids_.reserve(live_.size());
		for (const auto& [id, _] : live_) live_ids_.push_back(id);
		if (live_ids_.empty()) return 0;
		return live_ids_[rng_.next() % live_ids_.size()];
	}

	std::unique_ptr<matching::OrderBook> book_;
	benchmark_runner::SplitMix64 rng_{42};
	benchmark_runner::SplitMix64 op_rng_{42};
	std::uint64_t id_counter_ = 0;
	std::unordered_map<std::uint64_t, LiveOrder> live_;
	std::vector<std::uint64_t> live_ids_;
};

}  // namespace

int main(int argc, char** argv) {
	OverallScenario scen;
	return benchmark_runner::RunScenario(scen, argc, argv);
}
