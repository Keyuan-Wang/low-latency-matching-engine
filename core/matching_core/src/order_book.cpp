/**
 * @file order_book.cpp
 * @brief Implementation of @ref matching::OrderBook (Phase 1: map + list, FIFO per level).
 */

#include <algorithm>
#include <cassert>

#include "matching/order_book.hpp"
#include "matching/types.hpp"

namespace matching {

namespace {

/**
 * @brief Whether a limit order crosses the opposite side's best quote.
 *
 * @param taker_side            Side of the incoming limit order.
 * @param limit_price           Limit price of the taker.
 * @param best_opposite_price   Best price on the opposite book (lowest ask or highest bid).
 * @return True if at least one share can match at @p best_opposite_price.
 */
bool can_cross_limit(Side taker_side, std::int64_t limit_price, std::int64_t best_opposite_price) {
    if (taker_side == Side::Buy) {
        return limit_price >= best_opposite_price;
    }
    return limit_price <= best_opposite_price;
}

}  // namespace

/**
 * @copydoc OrderBook::cancel_order
 */
ErrorCode OrderBook::cancel_order(std::uint64_t order_id) {
    auto it = id_to_order_.find(order_id);

    if (it != id_to_order_.end()) {
        // The id index points directly to the live Order. The Order carries
        // its parent PriceLevel, so cancel does not need to search by price.
        Order* o = it->second;
        o->parent_level->remove(*o);
        id_to_order_.erase(order_id);
        return ErrorCode::Success;
    }

    pending_cancel_ids_.insert(order_id);
    return ErrorCode::UnknownOrderId;
}

/**
 * @copydoc OrderBook::modify_order
 */
AddResult OrderBook::modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                                  std::uint64_t quantity, std::uint64_t timestamp) {
    // Remove existing order with this id if present (no side effects on pending_cancel_ids_).
    auto it = id_to_order_.find(order_id);
    if (it != id_to_order_.end()) {
        Order* o = it->second;
        o->parent_level->remove(*o);
        id_to_order_.erase(order_id);
    }

    // A prior cancel that landed in pending_cancel_ids_ is overridden by modify.
    pending_cancel_ids_.erase(order_id);

    // Delegate to add_limit_order — both duplicate and pending-cancel checks
    // will pass since we just cleaned up state for this id.
    return add_limit_order(order_id, side, price, quantity, timestamp);
}

/**
 * @copydoc OrderBook::add_limit_order
 */
AddResult OrderBook::add_limit_order(std::uint64_t order_id, Side side, std::int64_t price,
                                     std::uint64_t quantity, std::uint64_t timestamp) {
    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }

    if (pending_cancel_ids_.contains(order_id)) {
        out.code = ErrorCode::PendingCancelExists;
        out.remaining_quantity = quantity;
        return out;
    }

    if (id_to_order_.contains(order_id)) {
        out.code = ErrorCode::DuplicateOrderId;
        out.remaining_quantity = quantity;
        return out;
    }

    std::uint64_t remaining = quantity;

    // Consume opposite-side liquidity while the limit price permits crossing.
    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            const std::int64_t best_price = opposite_book.best_price();
            if (!can_cross_limit(side, price, best_price)) {
                break;
            }

            auto& price_level = opposite_book.best_level();

            // Within one price level, orders are consumed FIFO through the
            // intrusive queue maintained by PriceLevel.
            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    id_to_order_.erase(maker.id);

                    // Save the address before remove(): remove() returns the
                    // slot to its chunk and clears the order links.
                    Order* maker_ptr = &maker;
                    price_level.remove(*maker_ptr);
                }
            }

            // Best-level erasure is deliberately lazy: only the current best
            // can affect matching progress in this ordered-map implementation.
            if (price_level.empty())
                opposite_book.erase_best();
        }
    };

    if (side == Side::Buy) {
        match_against(asks_);
    } else {
        match_against(bids_);
    }

    out.remaining_quantity = remaining;

    if (remaining == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    Order* node;

    if (side == Side::Buy) {
        auto& level = bids_.get_or_create(price);

        node = level.allocate();

        // Benchmark configuration assumes the fixed chunk pool is large enough.
        assert(node != nullptr);

        // Aggregate assignment overwrites allocator-side links, including
        // parent_level, so parent_level is restored immediately afterward.
        *node = {order_id, price, remaining, timestamp};

        node->parent_level = &level;
        level.push_back(*node);
    } else {
        auto& level = asks_.get_or_create(price);

        node = level.allocate();

        // Benchmark configuration assumes the fixed chunk pool is large enough.
        assert(node != nullptr);

        // Aggregate assignment overwrites allocator-side links, including
        // parent_level, so parent_level is restored immediately afterward.
        *node = {order_id, price, remaining, timestamp};

        node->parent_level = &level;
        level.push_back(*node);
    }

    id_to_order_.emplace(order_id, node);

    // output
    out.code = ErrorCode::Success;
    return out;
}

/**
 * @copydoc OrderBook::add_market_order
 */
AddResult OrderBook::add_market_order(std::uint64_t order_id, Side side, std::uint64_t quantity,
                                      std::uint64_t timestamp) {
    (void)timestamp;

    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }

    if (pending_cancel_ids_.contains(order_id)) {
        out.code = ErrorCode::PendingCancelExists;
        out.remaining_quantity = quantity;
        return out;
    }

    if (id_to_order_.contains(order_id)) {
        out.code = ErrorCode::DuplicateOrderId;
        out.remaining_quantity = quantity;
        return out;
    }

    std::uint64_t remaining = quantity;

    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            auto& price_level = opposite_book.best_level();

            // Market orders ignore price limits and sweep the opposite best
            // levels until filled or until the book is empty.
            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    id_to_order_.erase(maker.id);

                    // Save the pointer before releasing the order slot back to
                    // its owning chunk.
                    Order* maker_ptr = &maker;
                    price_level.remove(*maker_ptr);
                }
            }

            if (price_level.empty())
                opposite_book.erase_best();
        }
    };

    if (side == Side::Buy) {
        match_against(asks_);
    } else {
        match_against(bids_);
    }

    out.remaining_quantity = remaining;

    if (remaining == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    out.code = ErrorCode::MarketRemainderCancelled;
    return out;
}

}  // namespace matching
