/**
 * @file order_book.hpp
 * @brief Central limit order book (Phase 1): declarations for @ref matching::OrderBook and related types.
 */

#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <unordered_set>
#include <vector>

namespace matching {

/**
 * @brief Side of an order in the central limit order book.
 */
enum class Side {
    Buy,   ///< Bid side (buy book).
    Sell,  ///< Ask side (sell book).
};

/**
 * @brief One execution against a resting (maker) order.
 *
 * @details Price is the maker's resting price. The taker is the aggressor
 *          (the order that triggered this match in the current request).
 */
struct Trade {
    std::uint64_t taker_order_id;  ///< Aggressor order identifier.
    std::uint64_t maker_order_id;  ///< Resting order identifier.
    std::int64_t price;            ///< Execution price (maker level price).
    std::uint32_t quantity;        ///< Traded quantity.
};

/**
 * @brief Result or error code for an operation on the order book.
 */
enum class ErrorCode {
    Success,                   ///< Operation completed as requested.
    InvalidQuantity,           ///< Non-positive quantity (e.g. zero).
    DuplicateOrderId,          ///< Order id already present on the book.
    UnknownOrderId,            ///< Cancel: id not on book (recorded as pending cancel).
    MarketRemainderCancelled,  ///< Market order: leftover quantity not posted to book.
};

/**
 * @brief Aggregated outcome of a single @ref OrderBook::add_limit_order or
 *        @ref OrderBook::add_market_order call.
 */
struct AddResult {
    ErrorCode code{ErrorCode::Success};  ///< Primary status of the request.

    std::uint32_t initial_quantity{0};   ///< Requested quantity at entry.
    std::uint32_t filled_quantity{0};      ///< Total matched quantity.
    std::uint32_t remaining_quantity{0};   ///< Unfilled quantity after matching / rest.

    std::vector<Trade> trades{};           ///< Individual fills, in chronological order.
};

/**
 * @brief Resting order stored at a price level (FIFO queue element).
 */
struct Order {
    std::uint64_t id;         ///< Unique order identifier.
    std::int64_t price;       ///< Limit price while resting on the book.
    std::uint64_t quantity;   ///< Remaining quantity.
    std::uint64_t timestamp;  ///< Application timestamp (ordering / audit).
};

/** @brief FIFO queue of orders at one price on the ask or bid side. */
using PriceLevel = std::list<Order>;

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
 *
 * @note Cancel path scans books (Phase 1); later phases may add O(1) index by id.
 * @note The matching core assumes the gateway has validated business order ids.
 */
class OrderBook {
public:
    /** @brief Constructs an empty book. */
    OrderBook() = default;

    /** @brief Benchmark-compatible constructor; Phase 1 ignores pool sizing. */
    explicit OrderBook(std::size_t /*pool_capacity*/) {}

    /**
     * @brief Submit a limit order: match against the opposite side, rest remainder on book.
     *
     * @param order_id   Business/reporting order id; not validated for duplicates in-core.
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
                              std::uint32_t quantity, std::uint64_t timestamp);

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
    AddResult add_market_order(std::uint64_t order_id, Side side, std::uint32_t quantity,
                               std::uint64_t timestamp);

    /**
     * @brief Atomically replace a resting order: remove any existing order with the same id,
     *        then add a fresh limit order. If no order with that id is on the book, behaves
     *        as a plain add.
     *
     * @param order_id  Target order id (used for remove if present, then for the new insert).
     * @param side      Side for the replacement order.
     * @param price     Limit price for the replacement order.
     * @param quantity  Quantity for the replacement order (> 0).
     * @param timestamp Opaque event time for the replacement order.
     * @return @ref AddResult from the replacement add, or @ref ErrorCode::InvalidQuantity.
     */
    AddResult modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                           std::uint32_t quantity, std::uint64_t timestamp);

    /**
     * @brief Remove a resting order by id from either side.
     *
     * @param order_id Id to cancel.
     * @return @ref ErrorCode::Success if removed from book;
     *         @ref ErrorCode::UnknownOrderId if not found.
     */
    ErrorCode cancel_order(std::uint64_t order_id);

private:
    BidBook bids_{};   ///< Bid price levels (best bid at @c begin()).
    AskBook asks_{};   ///< Ask price levels (best ask at @c begin()).

};

}  // namespace matching
