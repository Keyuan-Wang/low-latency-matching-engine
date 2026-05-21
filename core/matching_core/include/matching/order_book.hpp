/**
 * @file order_book.hpp
 * @brief Central limit order book (Phase 1): declarations for @ref matching::OrderBook and related types.
 */

#pragma once

#include <cstdint>
#include <unordered_set>
#include <map>


#include "matching/flat_hash_map.hpp"
#include "types.hpp"
#include "intrusive_list.hpp"
#include "order_pool.hpp"

namespace matching {


using PriceLevel = IntrusiveList;

/**
 * @brief Ask side: ascending price map; @c begin() is best (lowest) ask.
 */
using AskBook = std::map<std::int64_t, PriceLevel, std::less<>>;

/**
 * @brief Bid side: descending price map; @c begin() is best (highest) bid.
 */
using BidBook = std::map<std::int64_t, PriceLevel, std::greater<>>;

/**
 * @brief Phase-1 central limit order book (two-sided, price–time priority at each level).
 *
 * @details
 * - Bids and asks are stored in separate @ref BidBook / @ref AskBook maps.
 * - Each price maps to a @ref PriceLevel (`std::list`) for FIFO per level.
 * - @ref active_ids_ tracks resting order ids for duplicate detection in O(1) average time.
 * - @ref pending_cancel_ids_ supports cancel-before-insert: unknown cancels are queued
 *   until an insert with the same id is rejected with @ref ErrorCode::PendingCancelExists.
 *
 * @note Cancel path scans books (Phase 1); later phases may add O(1) index by id.
 */
class OrderBook {
public:
    /** @brief Constructs an empty book. */
    explicit OrderBook(std::size_t pool_capacity = 100000)
        : pool_(pool_capacity) ,
          id_to_order_(pool_capacity) {};

    /**
     * @brief Submit a limit order: match against the opposite side, rest remainder on book.
     *
     * @param order_id   Unique order id (must not duplicate a resting id or pending cancel).
     * @param side       @ref Side::Buy consumes asks; @ref Side::Sell consumes bids.
     * @param price      Limit price; used for crossing check and for resting level.
     * @param quantity   Desired quantity (> 0).
     * @param timestamp  Opaque event time (stored on resting portion).
     * @return @ref AddResult with @ref AddResult::trades and fill/rest fields set.
     *
     * @retval ErrorCode::Success Resting portion (if any) posted; or fully filled.
     * @retval ErrorCode::InvalidQuantity @p quantity == 0.
     * @retval ErrorCode::PendingCancelExists @p order_id in @ref pending_cancel_ids_.
     * @retval ErrorCode::DuplicateOrderId @p order_id already in @ref active_ids_.
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
     *        then add a fresh limit order.  If no order with that id is on the book, behaves
     *        as a plain add (skips duplicate-id checks since the id isn't active).
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
     * @param order_id Id to cancel.
     * @return @ref ErrorCode::Success if removed from book;
     *         @ref ErrorCode::UnknownOrderId if not found (id added to pending cancel set).
     */
    ErrorCode cancel_order(std::uint64_t order_id);

    /**
     * @brief Number of ids waiting for a later insert after an early cancel.
     * @return Size of @ref pending_cancel_ids_.
     */
    [[nodiscard]] std::size_t pending_cancel_count() const noexcept {
        return pending_cancel_ids_.size();
    }

private:
    BidBook bids_{};   ///< Bid price levels (best bid at @c begin()).
    AskBook asks_{};   ///< Ask price levels (best ask at @c begin()).

    OrderPool pool_;

    std::unordered_set<std::uint64_t> active_ids_{};         ///< Ids currently resting on book.
    std::unordered_set<std::uint64_t> pending_cancel_ids_{}; ///< Early cancel ids not yet seen on insert.
    
    // open-addressing hash table
    HashTable id_to_order_;
};

}  // namespace matching
