/**
 * @file order_book.hpp
 * @brief Central limit order book (Phase 1): declarations for @ref matching::OrderBook and related types.
 */

#pragma once

#include <cstdint>
#include <map>
#include <utility>
#include <cstddef>  // std::byte, std::size_t


#include "types.hpp"
#include "price_level.hpp"
#include "order_pool.hpp"
#include "cached_order_book.hpp"

namespace matching {
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
    [[nodiscard]] [[gnu::always_inline]] bool empty() const noexcept {
        return levels_.empty();
    }

    [[nodiscard]] [[gnu::always_inline]] std::int64_t best_price() const {
        return levels_.begin()->first;
    }

    [[nodiscard]] [[gnu::always_inline]] PriceLevel& best_level() {
        return levels_.begin()->second;
    }

    [[nodiscard]] [[gnu::always_inline]] std::pair<PriceLevel*, bool> get_or_create(std::int64_t price) {
        auto [it, inserted] = levels_.try_emplace(price);
        return {&it->second, inserted};
    }

    [[gnu::always_inline]] void erase_best() {
        levels_.erase(levels_.begin());
    }
};

using AskBook = SideBook<true>;
using BidBook = SideBook<false>;

/**
 * @brief Phase-1 central limit order book (two-sided, price–time priority at each level).
 *
 * @details
 * - Bids and asks are stored in separate @ref BidBook / @ref AskBook maps.
 * - Each price maps to a @ref PriceLevel (`std::list`) for FIFO per level.
 * - Resting orders are addressed by an engine handle returned from add.
 *
 * @note The matching core assumes the gateway has validated business order ids.
 */
class OrderBook {
public:
    /** @brief Constructs an empty book. */
    explicit OrderBook(std::size_t pool_capacity = 100000) : pool_(pool_capacity) {};

    /**
     * @brief Submit a limit order: match against the opposite side, rest remainder on book.
     *
     * @param order_id   Business/reporting order id; not used for hot-path lookup.
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
     * @brief Atomically replace a resting order addressed by handle.
     *
     * @param h         Engine handle returned by a previous resting add.
     * @param side      Side for the replacement order.
     * @param price     Limit price for the replacement order.
     * @param quantity  Quantity for the replacement order (> 0).
     * @param timestamp Opaque event time for the replacement order.
     * @return @ref AddResult from the replacement add, or @ref ErrorCode::InvalidQuantity.
     */
    AddResult modify_order(OrderHandle h, Side side, std::int64_t price,
                           std::uint64_t quantity, std::uint64_t timestamp);

    /**
     * @brief Remove a resting order by engine handle.
     *
     * @param h Engine handle returned by a previous resting add.
     * @return @ref ErrorCode::Success if removed from book.
     */
    ErrorCode cancel_order(OrderHandle h);

private:
    CachedSideBook<false> bids_;   ///< Bid price levels (best bid at @c begin()).
    CachedSideBook<true> asks_;   ///< Ask price levels (best ask at @c begin()).

    OrderPool pool_;
};

}  // namespace matching
