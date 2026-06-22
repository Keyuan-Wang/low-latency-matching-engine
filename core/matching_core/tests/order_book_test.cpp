#include "matching_core/order_book.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void expect(bool ok, const char* msg) {
    if (!ok) {
        std::cerr << "FAIL : " << msg << "\n";
        std::exit(1);
    }
}
}

int main() {
    llmes::matching_core::OrderBook book;

    // 1. push limit order to an empty book: only insert the orders but no matching
    {
        const auto r = book.add_limit_order(1, llmes::matching_core::Side::Buy, 100, 10, 1);
        expect(r.code == llmes::matching_core::ErrorCode::Success, "Limite rest success");
        expect(r.filled_quantity == 0, "no fill");
        expect(r.remaining_quantity == 10, "full qty rests");
        expect(r.trades.empty(), "no trade");
        expect(r.handle != llmes::matching_core::kInvalidHandle, "resting order returns handle");
    }

    // 2. matching + push remaining orders in the book
    {
        llmes::matching_core::OrderBook b;
        (void)b.add_limit_order(10, llmes::matching_core::Side::Sell, 100, 5, 1);
        const auto r = b.add_limit_order(11, llmes::matching_core::Side::Buy, 100, 12, 2);
        expect(r.code == llmes::matching_core::ErrorCode::Success, "cross Success");
        expect(r.filled_quantity == 5, "filled 5");
        expect(r.remaining_quantity == 7, "rest 7");
        expect(r.trades.size() == 1, "one trade");
        expect(r.trades[0].maker_order_id == 10, "maker id");
        expect(r.trades[0].quantity == 5, "trade qty");
      }

    // 3. market orders wipe all orders, reamining orders are canceled
    {
        llmes::matching_core::OrderBook b;
        (void)b.add_limit_order(101, llmes::matching_core::Side::Sell, 100, 5, 1);
        (void)b.add_limit_order(102, llmes::matching_core::Side::Sell, 101, 5, 2);
        const auto r = b.add_market_order(500, llmes::matching_core::Side::Buy, 15, 3);
        expect(r.filled_quantity == 10, "market fill 10");
        expect(r.remaining_quantity == 5, "remainder 5");
        expect(r.code == llmes::matching_core::ErrorCode::MarketRemainderCancelled, "remainder cancelled");
    }

    // 4. handle-based cancel
    {
        llmes::matching_core::OrderBook b;
        const auto r = b.add_limit_order(42, llmes::matching_core::Side::Buy, 100, 1, 1);
        expect(r.code == llmes::matching_core::ErrorCode::Success, "insert before cancel");
        expect(r.handle != llmes::matching_core::kInvalidHandle, "cancel target handle");
        expect(b.cancel_order(r.handle) == llmes::matching_core::ErrorCode::Success, "handle cancel");
    }

    // 5. business order ids are not used for matching-core uniqueness checks
    {
        llmes::matching_core::OrderBook b;
        const auto first = b.add_limit_order(7, llmes::matching_core::Side::Buy, 100, 10, 1);
        expect(first.code == llmes::matching_core::ErrorCode::Success, "first insert ok");

        const auto second = b.add_limit_order(7, llmes::matching_core::Side::Buy, 99, 5, 2);
        expect(second.code == llmes::matching_core::ErrorCode::Success, "duplicate business id allowed");
        expect(second.handle != first.handle, "each resting order has its own handle");
    }

    std::cout << "order_book tests passed\n";
    return 0;
}