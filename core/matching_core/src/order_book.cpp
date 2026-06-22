/**
 * @file order_book.cpp
 * @brief Implementation of @ref llmes::matching_core::OrderBook (Phase 1: map + list, FIFO per level).
 */

#include <algorithm>
#include <cassert>

#include "matching_core/order_book.hpp"
#include "matching_core/price_level.hpp"
#include "matching_core/order_pool.hpp"
#include "matching_core/types.hpp"

namespace llmes::matching_core {

namespace {

/**
 * @brief Whether a limit order crosses the opposite side's best quote.
 *
 * @param taker_side            Side of the incoming limit order.
 * @param limit_price           Limit price of the taker.
 * @param best_opposite_price   Best price on the opposite book (lowest ask or highest bid).
 * @return True if at least one share can match at @p best_opposite_price.
 */
template <Side S>
inline bool can_cross_limit(std::int64_t limit_price, std::int64_t best_opposite_price) {
    if constexpr (S == Side::Sell)  return limit_price <= best_opposite_price;  // ask book
    else                            return limit_price >= best_opposite_price;  // bid book
}

}  // namespace

/**
 * @copydoc OrderBook::cancel_order
 */
ErrorCode OrderBook::cancel_order(OrderHandle h) {
    Order* o = pool_.resolve(h);

    o->parent_level->erase(*o);
    pool_.release(o);

    return ErrorCode::Success;
}

/**
 * @copydoc OrderBook::modify_order
 */
AddResult OrderBook::modify_order(OrderHandle h, Side side, std::int64_t price,
                                  std::uint64_t quantity, std::uint64_t timestamp) {
    Order* o = pool_.resolve(h);

    const auto order_id = o->id;

    o->parent_level->erase(*o);
    pool_.release(o);
    
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


    if (side == Side::Buy) {
        out.remaining_quantity = matching_engine_limit<Side::Buy>(out, order_id, price, quantity);
    } else {
        out.remaining_quantity = matching_engine_limit<Side::Sell>(out, order_id, price, quantity);
    }

    if (out.remaining_quantity == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    // add remaining limit order to book
    OrderHandle h = pool_.acquire();

    assert(h != kInvalidHandle);

    Order* node = pool_.resolve(h);
    // TODO: WHAT IF POOL IS ALREADY EMPTY?
    assert(node != nullptr);

    // set node
    node->id = order_id;
    node->price = price;
    node->quantity = out.remaining_quantity;
    node->timestamp = timestamp;
    node->prev = nullptr;
    node->next = nullptr;

    PriceLevel* level = nullptr;
    if (side == Side::Buy) {
        level = bids_.get_or_create(price);
    } else {
        level = asks_.get_or_create(price);
    }

    level->push_back(*node);
    node->parent_level = level;

    // output
    out.code = ErrorCode::Success;
    out.handle = h;
    return out;
}

/**
 * @copydoc OrderBook::add_market_order
 */
AddResult OrderBook::add_market_order(std::uint64_t order_id, Side side, std::uint64_t quantity,
                                      std::uint64_t timestamp) {

    AddResult out{};
    out.initial_quantity = quantity;

    if (quantity == 0) {
        out.code = ErrorCode::InvalidQuantity;
        return out;
    }


    if (side == Side::Buy) {
        out.remaining_quantity = matching_engine_market<Side::Buy>(out, order_id, quantity);
    } else {
        out.remaining_quantity = matching_engine_market<Side::Sell>(out, order_id, quantity);
    }

    if (out.remaining_quantity == 0) {
        out.code = ErrorCode::Success;
        return out;
    }

    out.code = ErrorCode::MarketRemainderCancelled;
    return out;
}


template <Side S>
std::uint64_t OrderBook::matching_engine_limit(AddResult& out, std::uint64_t order_id, std::int64_t price, std::uint64_t quantity) {
    // Consume opposite-side liquidity while the limit price permits crossing.
    auto& oppo_book = opposite_book<S>();
    while (quantity > 0 && !oppo_book.empty()) {
        const std::int64_t best_price = oppo_book.best_price();
        if (!can_cross_limit<S>(price, best_price)) {
            break;
        }

        auto& price_level = oppo_book.best_level();

        while (quantity > 0 && !price_level.empty()) {
            Order& maker = price_level.front();

            const std::uint64_t fill = std::min(quantity, maker.quantity);

            out.trades.emplace_back(order_id, maker.id, maker.price, fill);

            maker.quantity -= fill;
            quantity -= fill;
            out.filled_quantity += fill;

            if (maker.quantity == 0) {
                Order* maker_ptr = &maker;
                price_level.erase(*maker_ptr);
                pool_.release(maker_ptr);
            }
        }
        
        if (price_level.empty())
            oppo_book.erase_best();
    }

    return quantity;
}


template <Side S>
std::uint64_t OrderBook::matching_engine_market(AddResult& out, std::uint64_t order_id, std::uint64_t quantity) {
    auto& oppo_book = opposite_book<S>();
    while (quantity > 0 && !oppo_book.empty()) {
        auto& price_level = oppo_book.best_level();

        while (quantity > 0 && !price_level.empty()) {
            Order& maker = price_level.front();

            const std::uint64_t fill = std::min(quantity, maker.quantity);

            out.trades.emplace_back(order_id, maker.id, maker.price, fill);

            maker.quantity -= fill;
            quantity -= fill;
            out.filled_quantity += fill;

            if (maker.quantity == 0) {
                Order* maker_ptr = &maker;
                price_level.erase(*maker_ptr);
                pool_.release(maker_ptr);
            }
        }

        if (price_level.empty())
            oppo_book.erase_best();
    }

    return quantity;
}

}  // namespace llmes::matching_core
