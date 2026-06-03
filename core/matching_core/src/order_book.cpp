/**
 * @file order_book.cpp
 * @brief Implementation of @ref matching::OrderBook (Phase 1: map + list, FIFO per level).
 */

#include <algorithm>
#include <cassert>

#include "matching/order_book.hpp"
#include "matching/intrusive_list.hpp"
#include "matching/order_pool.hpp"
#include "matching/types.hpp"

namespace matching {

namespace {

/**
 * @brief Whether a limit order crosses the opposite side's best quote.
 */
bool can_cross_limit(Side taker_side, std::int64_t limit_price, std::int64_t best_opposite_price) {
    // Buy crosses when limit_price >= best_ask; sell crosses when limit_price <= best_bid.
    if (taker_side == Side::Buy) {
        return limit_price >= best_opposite_price;
    }
    return limit_price <= best_opposite_price;
}

}  // namespace

ErrorCode OrderBook::cancel_order(std::uint64_t order_id) {
    // Look up the order via the id-to-order hash table (O(1) expected).
    auto order_ptr = id_to_order_.find(order_id);

    if (order_ptr != nullptr) {
        Order* o = static_cast<Order*>(order_ptr);

        // Remove from its price-level intrusive list.
        o->parent_level->erase(*o);
        pool_.release(o);                   // Return memory to the object pool.
        id_to_order_.erase(order_id);       // Remove from hash table.
        return ErrorCode::Success;
    }

    // Order not found: record as pending cancel so a subsequent insert
    // with the same id is rejected (matching real-world exchange behaviour).
    return ErrorCode::UnknownOrderId;
}

AddResult OrderBook::modify_order(std::uint64_t order_id, Side side, std::int64_t price,
                                  std::uint64_t quantity, std::uint64_t timestamp) {
    // If the order already exists, remove it first (same logic as cancel_order).
    auto order_ptr = id_to_order_.find(order_id);

    if (order_ptr != nullptr) {
        Order* o = static_cast<Order*>(order_ptr);

        o->parent_level->erase(*o);
        pool_.release(o);
        id_to_order_.erase(order_id);
    }



    // Now delegate to add_limit_order with a clean state for this id.
    return add_limit_order(order_id, side, price, quantity, timestamp);
}

AddResult OrderBook::add_limit_order(std::uint64_t order_id, Side side, std::int64_t price,
                                     std::uint64_t quantity, std::uint64_t timestamp) {
    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }



    std::uint64_t remaining = quantity;

    // Consume opposite-side liquidity while the limit price permits crossing.
    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            const std::int64_t best_price = opposite_book.begin()->first;
            if (!can_cross_limit(side, price, best_price)) {
                break;  // Price no longer crosses — remainder will rest on the book.
            }

            auto level_it = opposite_book.begin();

            auto& price_level = level_it->second;   // Intrusive list for this price.

            // Fill against the best level until it is exhausted or the order is filled.
            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    // Fully consumed: remove maker from book.
                    id_to_order_.erase(maker.id);

                    Order* maker_ptr = &maker;
                    price_level.erase(*maker_ptr);
                    pool_.release(maker_ptr);
                }
            }

            // Level is empty: remove the price point from the book.
            if (price_level.empty())
                opposite_book.erase(level_it);
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

    // Add remaining quantity as a new resting order.
    Order* node = pool_.acquire();
    assert(node != nullptr);              // Pool capacity must be sized correctly upfront.

    *node = {order_id, price, remaining, timestamp};
    if (side == Side::Buy) {
        auto& level = bids_[price];
        level.push_back(*node);
        node->parent_level = &level;
    } else {
        auto& level = asks_[price];
        level.push_back(*node);
        node->parent_level = &level;
    }
    id_to_order_.insert(order_id, node);

    out.code = ErrorCode::Success;
    return out;
}

AddResult OrderBook::add_market_order(std::uint64_t order_id, Side side, std::uint64_t quantity,
                                      std::uint64_t timestamp) {
    (void)timestamp;

    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }



    std::uint64_t remaining = quantity;

    // Market order: sweep the entire opposite book until filled or exhausted.
    // Unlike limit orders, there is no crossing check — market orders always
    // take the best available price.
    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            auto level_it = opposite_book.begin();
            auto& price_level = level_it->second;

            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    id_to_order_.erase(maker.id);

                    Order* maker_ptr = &maker;
                    price_level.erase(*maker_ptr);
                    pool_.release(maker_ptr);
                }
            }

            if (price_level.empty())
                opposite_book.erase(level_it);
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

    // Market orders never post to the book: any remainder is discarded.
    out.code = ErrorCode::MarketRemainderCancelled;
    return out;
}

}  // namespace matching