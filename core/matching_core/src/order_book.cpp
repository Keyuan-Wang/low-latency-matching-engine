/**
 * @file order_book.cpp
 * @brief Implementation of @ref matching::OrderBook (Phase 1: map + list, FIFO per level).
 */

#include <algorithm>
#include <cassert>

#include "matching/order_book.hpp"
#include "matching/order_pool.hpp"

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
    auto try_remove = [&](auto& book) -> bool {
        for (auto level_it = book.begin(); level_it != book.end(); ++level_it) {

            auto& price_level = level_it->second;

            for (auto* order = price_level.begin(); order != nullptr; ) {

                auto* temp = order->next;
                
                if (order->id == order_id) {
                    // remove order from linked list
                    price_level.erase(*order);
                    // remove order from active id table
                    active_ids_.erase(order_id);
                    // free the space occupied by order
                    pool_.release(order);
                    
                    // remove empty price level from the ask/bid book
                    if (price_level.empty()) {
                        book.erase(level_it);
                    }
                    return true;
                }

                order = temp;
            }
        }
        return false;
    };

    if (try_remove(bids_) || try_remove(asks_)) {
        return ErrorCode::Success;
    }

    pending_cancel_ids_.insert(order_id);
    return ErrorCode::UnknownOrderId;
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

    if (active_ids_.contains(order_id)) {
        out.code = ErrorCode::DuplicateOrderId;
        out.remaining_quantity = quantity;
        return out;
    }

    std::uint64_t remaining = quantity;

    // Consume opposite-side liquidity while the limit price permits crossing.
    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            const std::int64_t best_price = opposite_book.begin()->first;
            if (!can_cross_limit(side, price, best_price)) {
                break;
            }

            auto level_it = opposite_book.begin();

            auto& price_level = level_it->second;   // an intrusive list

            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    active_ids_.erase(maker.id);

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

    // add remaining limit order to book
    Order* node = pool_.acquire();
    // TODO: WHAT IF POOL IS ALREADY EMPTY?
    assert(node != nullptr);

    *node = {order_id, price, remaining, timestamp};
    if (side == Side::Buy)  bids_[price].push_back(*node);
    else                    asks_[price].push_back(*node);
    active_ids_.insert(order_id);

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

    if (active_ids_.contains(order_id)) {
        out.code = ErrorCode::DuplicateOrderId;
        out.remaining_quantity = quantity;
        return out;
    }

    std::uint64_t remaining = quantity;

    auto match_against = [&](auto& opposite_book) {
        while (remaining > 0 && !opposite_book.empty()) {
            auto level_it = opposite_book.begin();
            auto& price_level = level_it->second;   // an intrusive list

            while (remaining > 0 && !price_level.empty()) {
                Order& maker = price_level.front();

                const std::uint64_t fill = std::min(remaining, maker.quantity);

                out.trades.emplace_back(order_id, maker.id, maker.price, fill);

                maker.quantity -= fill;
                remaining -= fill;
                out.filled_quantity += fill;

                if (maker.quantity == 0) {
                    active_ids_.erase(maker.id);

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

    out.code = ErrorCode::MarketRemainderCancelled;
    return out;
}

}  // namespace matching
