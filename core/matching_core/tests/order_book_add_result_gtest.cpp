#include "matching/order_book.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <ostream>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

std::ostream& operator<<(std::ostream& os, matching::ErrorCode code) {
    switch (code) {
    case matching::ErrorCode::Success:
        return os << "Success";
    case matching::ErrorCode::InvalidQuantity:
        return os << "InvalidQuantity";
    case matching::ErrorCode::DuplicateOrderId:
        return os << "DuplicateOrderId";
    case matching::ErrorCode::UnknownOrderId:
        return os << "UnknownOrderId";
    case matching::ErrorCode::PendingCancelExists:
        return os << "PendingCancelExists";
    case matching::ErrorCode::MarketRemainderCancelled:
        return os << "MarketRemainderCancelled";
    }
    return os << "ErrorCode(" << static_cast<int>(code) << ")";
}

namespace {

struct RefOrder {
    std::uint64_t id = 0;
    std::int64_t price = 0;
    std::uint64_t quantity = 0;
};

class ReferenceOrderBook {
public:
    matching::AddResult add_limit_order(std::uint64_t order_id,
                                        matching::Side side,
                                        std::int64_t price,
                                        std::uint64_t quantity,
                                        std::uint64_t timestamp) {
        (void)timestamp;
        matching::AddResult out{};
        out.initial_quantity = quantity;
        if (quantity == 0) {
            out.code = matching::ErrorCode::InvalidQuantity;
            return out;
        }
        std::uint64_t remaining = quantity;
        if (side == matching::Side::Buy) {
            match_limit(order_id, side, price, remaining, asks_, out);
        } else {
            match_limit(order_id, side, price, remaining, bids_, out);
        }
        out.remaining_quantity = remaining;
        if (remaining == 0) {
            out.code = matching::ErrorCode::Success;
            return out;
        }
        rest_order(order_id, side, price, remaining);
        out.code = matching::ErrorCode::Success;
        return out;
    }

    matching::AddResult add_market_order(std::uint64_t order_id,
                                         matching::Side side,
                                         std::uint64_t quantity,
                                         std::uint64_t timestamp) {
        (void)timestamp;
        matching::AddResult out{};
        out.initial_quantity = quantity;
        if (quantity == 0) {
            out.code = matching::ErrorCode::InvalidQuantity;
            return out;
        }
        std::uint64_t remaining = quantity;
        if (side == matching::Side::Buy) {
            match_market(order_id, remaining, asks_, out);
        } else {
            match_market(order_id, remaining, bids_, out);
        }
        out.remaining_quantity = remaining;
        out.code = (remaining == 0)
                       ? matching::ErrorCode::Success
                       : matching::ErrorCode::MarketRemainderCancelled;
        return out;
    }

    matching::AddResult modify_order(std::uint64_t order_id,
                                     matching::Side side,
                                     std::int64_t price,
                                     std::uint64_t quantity,
                                     std::uint64_t timestamp) {
        (void)cancel_order(order_id);
        return add_limit_order(order_id, side, price, quantity, timestamp);
    }

    bool cancel_order(std::uint64_t order_id) {
        return cancel_from_book(order_id, bids_) || cancel_from_book(order_id, asks_);
    }

private:
    template <typename Book>
    static void match_limit(std::uint64_t order_id,
                            matching::Side side,
                            std::int64_t limit_price,
                            std::uint64_t& remaining,
                            Book& opposite_book,
                            matching::AddResult& out) {
        while (remaining > 0 && !opposite_book.empty()) {
            const std::int64_t best_price = opposite_book.begin()->first;
            const bool crosses =
                (side == matching::Side::Buy)
                    ? (limit_price >= best_price)
                    : (limit_price <= best_price);
            if (!crosses) break;
            match_level(order_id, remaining, opposite_book, out);
        }
    }

    template <typename Book>
    static void match_market(std::uint64_t order_id,
                             std::uint64_t& remaining,
                             Book& opposite_book,
                             matching::AddResult& out) {
        while (remaining > 0 && !opposite_book.empty()) {
            match_level(order_id, remaining, opposite_book, out);
        }
    }

    template <typename Book>
    static void match_level(std::uint64_t order_id,
                            std::uint64_t& remaining,
                            Book& opposite_book,
                            matching::AddResult& out) {
        auto level_it = opposite_book.begin();
        auto& queue = level_it->second;
        while (remaining > 0 && !queue.empty()) {
            RefOrder& maker = queue.front();
            const std::uint64_t fill = std::min(remaining, maker.quantity);
            out.trades.push_back(
                matching::Trade{order_id, maker.id, maker.price, fill});
            maker.quantity -= fill;
            remaining -= fill;
            out.filled_quantity += fill;
            if (maker.quantity == 0) {
                queue.pop_front();
            }
        }
        if (queue.empty()) {
            opposite_book.erase(level_it);
        }
    }

    void rest_order(std::uint64_t order_id,
                    matching::Side side,
                    std::int64_t price,
                    std::uint64_t quantity) {
        if (side == matching::Side::Buy) {
            bids_[price].push_back(RefOrder{order_id, price, quantity});
        } else {
            asks_[price].push_back(RefOrder{order_id, price, quantity});
        }
    }

    template <typename Book>
    static bool cancel_from_book(std::uint64_t order_id, Book& book) {
        for (auto level_it = book.begin(); level_it != book.end(); ++level_it) {
            auto& queue = level_it->second;
            const auto order_it =
                std::find_if(queue.begin(), queue.end(), [&](const RefOrder& order) {
                    return order.id == order_id;
                });
            if (order_it == queue.end()) continue;
            queue.erase(order_it);
            if (queue.empty()) {
                book.erase(level_it);
            }
            return true;
        }
        return false;
    }

    std::map<std::int64_t, std::deque<RefOrder>, std::greater<>> bids_{};
    std::map<std::int64_t, std::deque<RefOrder>, std::less<>> asks_{};
};

void ExpectSameTrade(const matching::Trade& actual,
                     const matching::Trade& expected,
                     std::size_t index) {
    SCOPED_TRACE("trade index " + std::to_string(index));
    EXPECT_EQ(actual.taker_order_id, expected.taker_order_id);
    EXPECT_EQ(actual.maker_order_id, expected.maker_order_id);
    EXPECT_EQ(actual.price, expected.price);
    EXPECT_EQ(actual.quantity, expected.quantity);
}

void ExpectSameAddResult(const matching::AddResult& actual,
                         const matching::AddResult& expected) {
    EXPECT_EQ(actual.code, expected.code);
    EXPECT_EQ(actual.initial_quantity, expected.initial_quantity);
    EXPECT_EQ(actual.filled_quantity, expected.filled_quantity);
    EXPECT_EQ(actual.remaining_quantity, expected.remaining_quantity);
    ASSERT_EQ(actual.trades.size(), expected.trades.size());
    for (std::size_t i = 0; i < actual.trades.size(); ++i) {
        ExpectSameTrade(actual.trades[i], expected.trades[i], i);
    }
}

void apply_trades(const matching::AddResult& result,
                  std::unordered_map<std::uint64_t, std::uint64_t>& live_orders) {
    for (const auto& trade : result.trades) {
        auto it = live_orders.find(trade.maker_order_id);
        if (it == live_orders.end()) continue;
        if (trade.quantity >= it->second) {
            live_orders.erase(it);
        } else {
            it->second -= trade.quantity;
        }
    }
}

class ResultHarness {
public:
    explicit ResultHarness(std::size_t capacity = 200000)
        : actual_(capacity) {}

    void add_limit(std::uint64_t order_id,
                   matching::Side side,
                   std::int64_t price,
                   std::uint64_t quantity,
                   std::uint64_t timestamp) {
        const auto expected =
            expected_.add_limit_order(order_id, side, price, quantity, timestamp);
        const auto actual =
            actual_.add_limit_order(order_id, side, price, quantity, timestamp);
        ExpectSameAddResult(actual, expected);
        apply_result(order_id, side, actual);
    }

    void add_market(std::uint64_t order_id,
                    matching::Side side,
                    std::uint64_t quantity,
                    std::uint64_t timestamp) {
        const auto expected =
            expected_.add_market_order(order_id, side, quantity, timestamp);
        const auto actual =
            actual_.add_market_order(order_id, side, quantity, timestamp);
        ExpectSameAddResult(actual, expected);
        apply_trades(actual, live_orders_);
    }

    void modify(std::uint64_t order_id,
                matching::Side side,
                std::int64_t price,
                std::uint64_t quantity,
                std::uint64_t timestamp) {
        const auto expected =
            expected_.modify_order(order_id, side, price, quantity, timestamp);
        const auto actual =
            actual_.modify_order(order_id, side, price, quantity, timestamp);
        ExpectSameAddResult(actual, expected);
        apply_result(order_id, side, actual);
    }

    void cancel(std::uint64_t order_id) {
        (void)expected_.cancel_order(order_id);
        (void)actual_.cancel_order(order_id);
        live_orders_.erase(order_id);
    }

    void random_test(std::uint64_t seed = 42, std::size_t ops = 10000) {
        std::mt19937 rng(seed);
        std::uint64_t next_id = 1;
        std::unordered_map<std::uint64_t, matching::Side> known_orders;

        for (std::size_t i = 0; i < ops; ++i) {
            if (known_orders.empty() || rng() % 4 != 0) {
                // Add limit order
                const matching::Side side =
                    (rng() % 2 == 0) ? matching::Side::Buy : matching::Side::Sell;
                const std::int64_t price = 1000 + static_cast<std::int64_t>(rng() % 100) * (side == matching::Side::Buy ? -1 : 1);
                const std::uint64_t qty = 1 + (rng() % 10);
                const std::uint64_t oid = next_id++;
                add_limit(oid, side, price, qty, oid);
                known_orders[oid] = side;
            } else {
                // Cancel a random known order
                auto it = known_orders.begin();
                std::advance(it, rng() % known_orders.size());
                cancel(it->first);
                known_orders.erase(it);
            }
        }
    }

private:
    matching::OrderBook actual_;
    ReferenceOrderBook expected_;
    std::unordered_map<std::uint64_t, std::uint64_t> live_orders_;
    std::uint64_t last_handle_ = 0;

    void apply_result(std::uint64_t order_id, matching::Side side,
                      const matching::AddResult& result) {
        apply_trades(result, live_orders_);
        if (result.code == matching::ErrorCode::Success &&
            result.remaining_quantity > 0) {
            live_orders_[order_id] = result.remaining_quantity;
        }
    }
};

TEST(OrderBookAddResultTest, add_limit_rest) {
    ResultHarness h;
    h.add_limit(1, matching::Side::Buy, 100, 10, 1);
    h.add_limit(2, matching::Side::Sell, 200, 5, 2);
}

TEST(OrderBookAddResultTest, add_limit_cross_partial) {
    ResultHarness h;
    h.add_limit(10, matching::Side::Sell, 100, 5, 1);
    h.add_limit(11, matching::Side::Buy, 100, 12, 2);
}

TEST(OrderBookAddResultTest, add_limit_cross_full) {
    ResultHarness h;
    h.add_limit(10, matching::Side::Sell, 100, 5, 1);
    h.add_limit(11, matching::Side::Buy, 105, 5, 2);
}

TEST(OrderBookAddResultTest, add_limit_cross_multi_level) {
    ResultHarness h;
    h.add_limit(10, matching::Side::Sell, 100, 5, 1);
    h.add_limit(11, matching::Side::Sell, 101, 5, 2);
    h.add_limit(12, matching::Side::Buy, 101, 10, 3);
}

TEST(OrderBookAddResultTest, add_market_sweep) {
    ResultHarness h;
    h.add_limit(101, matching::Side::Sell, 100, 5, 1);
    h.add_limit(102, matching::Side::Sell, 101, 5, 2);
    h.add_market(500, matching::Side::Buy, 10, 3);
}

TEST(OrderBookAddResultTest, add_market_remainder_cancelled) {
    ResultHarness h;
    h.add_limit(101, matching::Side::Sell, 100, 5, 1);
    h.add_market(500, matching::Side::Buy, 10, 2);
}

TEST(OrderBookAddResultTest, modify_resting) {
    ResultHarness h;
    h.add_limit(1, matching::Side::Buy, 100, 10, 1);
    h.modify(1, matching::Side::Buy, 99, 5, 2);
}

TEST(OrderBookAddResultTest, cancel_resting) {
    ResultHarness h;
    h.add_limit(1, matching::Side::Buy, 100, 10, 1);
    h.cancel(1);
}

TEST(OrderBookAddResultTest, duplicate_order_id_allowed) {
    ResultHarness h;
    h.add_limit(1, matching::Side::Buy, 100, 10, 1);
    h.add_limit(1, matching::Side::Sell, 101, 5, 2);
}

TEST(OrderBookAddResultTest, random_workload) {
    ResultHarness h;
    h.random_test(42, 5000);
}

}  // namespace
