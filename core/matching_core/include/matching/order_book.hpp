/**
 * @file order_book.hpp
 * @brief Central limit order book (Phase 1): declarations for @ref matching::OrderBook and related types.
 */

#pragma once

#include <cstdint>
#include <unordered_set>
#include <map>

#include "absl/container/flat_hash_map.h"


#include "types.hpp"
#include "intrusive_list.hpp"
#include "order_pool.hpp"

namespace matching {


using PriceLevel = IntrusiveList;

template <bool IsAsk>
struct PriceCompare;

template <>
struct PriceCompare<true> {
    bool operator()(std::int64_t lhs, std::int64_t rhs) const noexcept {
        return lhs < rhs;
    }
};

template <>
struct PriceCompare<false> {
    bool operator()(std::int64_t lhs, std::int64_t rhs) const noexcept {
        return lhs > rhs;
    }
};

/**
 * @brief Price-level container for one side of the book.
 *
 * @details Phase 4 V1 deliberately keeps the storage as an ordered map.  The
 * wrapper isolates the operations OrderBook needs so later phases can replace
 * the backing container without rewriting matching logic.
 */
template <bool IsAsk>
class SideBook {
private:
    std::map<std::int64_t, PriceLevel, PriceCompare<IsAsk>> levels_{};
public:
    [[nodiscard]] bool empty() const noexcept {
        return levels_.empty();
    }

    [[nodiscard]] std::int64_t best_price() const {
        return levels_.begin()->first;
    }

    [[nodiscard]] PriceLevel& best_level() {
        return levels_.begin()->second;
    }

    PriceLevel& get_or_create(std::int64_t price) {
        return levels_[price];
    }

    void erase_best() {
        levels_.erase(levels_.begin());
    }
};

using AskBook = SideBook<true>;
using BidBook = SideBook<false>;

/**
 * @brief Central limit order book (two-sided, price–time priority at each level).
 *
 * @details
 * - Bids and asks are stored in separate @ref BidBook / @ref AskBook maps.
 * - Each price maps to a @ref PriceLevel (IntrusiveList) for FIFO per level.
 * - The exchange gateway owns all order-id validation (duplicate, pending-cancel).
 * - id_to_order_ is retained as the functional cancel/modify lookup mechanism.
 */
class OrderBook {
public:
    /** @brief Constructs an empty book. */
    explicit OrderBook(std::size_t pool_capacity = 100000) : pool_(pool_capacity) {};

    /**
     * @brief Submit a limit order: match against the opposite side, rest remainder on book.
     *
     * @param order_id   Business/reporting order id.
     * @param side       @ref Side::Buy consumes asks; @ref Side::Sell consumes bids.
     * @param price      Limit price; used for crossing check and for resting level.
     * @param quantity   Desired quantity (> 0).
     * @param timestamp  Opaque event time (stored on resting portion).
     * @return @ref AddResult with @ref AddResult::trades and fill/rest fields set.
     *
     * @retval ErrorCode::Success Resting portion (if any) posted; or fully filled.
     * @retval ErrorCode::InvalidQuantity @p quantity == 0.
     */
    AddResult add_limit_order(std::uint64_t order_id, Side side, std::int64_t price,
                              std::uint64_t quantity, std::uint64_t timestamp);

    /**
     * @brief Submit a market order: match aggressively; do not post remainder to the book.
     *
     * @param order_id   Unique order id for this aggressive order.
     * @param side       Buy sweeps asks; sell sweeps bids.
     * @param quantity   Desired quantity (> 0).
     * @param timestamp  Unused in current implementation (reserved).
     * @return @ref AddResult; leftover sets @ref ErrorCode::MarketRemainderCancelled.
     *
     * @retval ErrorCode::Success Fully filled.
     * @retval ErrorCode::MarketRemainderCancelled Partially filled; remainder discarded.
     */
    AddResult add_market_order(std::uint64_t order_id, Side side, std::uint64_t quantity,
                               std::uint64_t timestamp);

    /**
     * @brief Atomically replace a resting order: remove any existing order with the same id,
     *        then add a fresh limit order.
     *
     * @param order_id  Target order id (used for remove if present, then for the new insert).
     * @param side      Side for the replacement order.
     * @param price     Limit price for the replacement order.
     * @param quantity  Quantity for the replacement order (> 0).
     * @param timestamp Opaque event time for the replacement order.
     * @return @ref AddResult from the replacement add, or @ref ErrorCode::InvalidQuantity.
     */
    AddResult modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                           std::uint64_t quantity, std::uint64_t timestamp);

    /**
     * @brief Remove a resting order by id from either side.
     *
     * @param order_id Id to cancel (assumed valid — gateway-owned).
     * @return @ref ErrorCode::Success if removed from book;
     *         @ref ErrorCode::UnknownOrderId if not found.
     */
    ErrorCode cancel_order(std::uint64_t order_id);

private:
    BidBook bids_{};   ///< Bid price levels (best bid at @c begin()).
    AskBook asks_{};   ///< Ask price levels (best ask at @c begin()).

    OrderPool pool_;

    absl::flat_hash_map<std::uint64_t, Order*> id_to_order_{};
};

}  // namespace matching
