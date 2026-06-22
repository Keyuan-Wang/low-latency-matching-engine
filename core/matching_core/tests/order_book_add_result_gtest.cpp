#include "matching_core/order_book.hpp"

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

std::ostream& operator<<(std::ostream& os, llmes::matching_core::ErrorCode code) {
    switch (code) {
    case llmes::matching_core::ErrorCode::Success:
        return os << "Success";
    case llmes::matching_core::ErrorCode::InvalidQuantity:
        return os << "InvalidQuantity";
    case llmes::matching_core::ErrorCode::DuplicateOrderId:
        return os << "DuplicateOrderId";
    case llmes::matching_core::ErrorCode::UnknownOrderId:
        return os << "UnknownOrderId";
    case llmes::matching_core::ErrorCode::PendingCancelExists:
        return os << "PendingCancelExists";
    case llmes::matching_core::ErrorCode::MarketRemainderCancelled:
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
    llmes::matching_core::AddResult add_limit_order(std::uint64_t order_id,
                                        llmes::matching_core::Side side,
                                        std::int64_t price,
                                        std::uint64_t quantity,
                                        std::uint64_t timestamp) {
        (void)timestamp;

        llmes::matching_core::AddResult out{};
        out.initial_quantity = quantity;

        if (quantity == 0) {
            out.code = llmes::matching_core::ErrorCode::InvalidQuantity;
            return out;
        }

        std::uint64_t remaining = quantity;
        if (side == llmes::matching_core::Side::Buy) {
            match_limit(order_id, side, price, remaining, asks_, out);
        } else {
            match_limit(order_id, side, price, remaining, bids_, out);
        }

        out.remaining_quantity = remaining;
        if (remaining == 0) {
            out.code = llmes::matching_core::ErrorCode::Success;
            return out;
        }

        rest_order(order_id, side, price, remaining);
        out.code = llmes::matching_core::ErrorCode::Success;
        out.handle = 0;  // Fake valid handle. Tests compare validity only.
        return out;
    }

    llmes::matching_core::AddResult add_market_order(std::uint64_t order_id,
                                         llmes::matching_core::Side side,
                                         std::uint64_t quantity,
                                         std::uint64_t timestamp) {
        (void)timestamp;

        llmes::matching_core::AddResult out{};
        out.initial_quantity = quantity;

        if (quantity == 0) {
            out.code = llmes::matching_core::ErrorCode::InvalidQuantity;
            return out;
        }

        std::uint64_t remaining = quantity;
        if (side == llmes::matching_core::Side::Buy) {
            match_market(order_id, remaining, asks_, out);
        } else {
            match_market(order_id, remaining, bids_, out);
        }

        out.remaining_quantity = remaining;
        out.code = (remaining == 0)
                       ? llmes::matching_core::ErrorCode::Success
                       : llmes::matching_core::ErrorCode::MarketRemainderCancelled;
        return out;
    }

    llmes::matching_core::AddResult modify_order(std::uint64_t order_id,
                                     llmes::matching_core::Side side,
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
                            llmes::matching_core::Side side,
                            std::int64_t limit_price,
                            std::uint64_t& remaining,
                            Book& opposite_book,
                            llmes::matching_core::AddResult& out) {
        while (remaining > 0 && !opposite_book.empty()) {
            const std::int64_t best_price = opposite_book.begin()->first;
            const bool crosses =
                (side == llmes::matching_core::Side::Buy)
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
                             llmes::matching_core::AddResult& out) {
        while (remaining > 0 && !opposite_book.empty()) {
            match_level(order_id, remaining, opposite_book, out);
        }
    }

    template <typename Book>
    static void match_level(std::uint64_t order_id,
                            std::uint64_t& remaining,
                            Book& opposite_book,
                            llmes::matching_core::AddResult& out) {
        auto level_it = opposite_book.begin();
        auto& queue = level_it->second;

        while (remaining > 0 && !queue.empty()) {
            RefOrder& maker = queue.front();
            const std::uint64_t fill = std::min(remaining, maker.quantity);

            out.trades.push_back(
                llmes::matching_core::Trade{order_id, maker.id, maker.price, fill});
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
                    llmes::matching_core::Side side,
                    std::int64_t price,
                    std::uint64_t quantity) {
        if (side == llmes::matching_core::Side::Buy) {
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

struct LiveOrder {
    llmes::matching_core::OrderHandle handle = llmes::matching_core::kInvalidHandle;
    llmes::matching_core::Side side = llmes::matching_core::Side::Buy;
    std::int64_t price = 0;
    std::uint64_t quantity = 0;
};

void ExpectSameTrade(const llmes::matching_core::Trade& actual,
                     const llmes::matching_core::Trade& expected,
                     std::size_t index) {
    SCOPED_TRACE("trade index " + std::to_string(index));
    EXPECT_EQ(actual.taker_order_id, expected.taker_order_id);
    EXPECT_EQ(actual.maker_order_id, expected.maker_order_id);
    EXPECT_EQ(actual.price, expected.price);
    EXPECT_EQ(actual.quantity, expected.quantity);
}

void ExpectSameAddResult(const llmes::matching_core::AddResult& actual,
                         const llmes::matching_core::AddResult& expected) {
    EXPECT_EQ(actual.code, expected.code);
    EXPECT_EQ(actual.initial_quantity, expected.initial_quantity);
    EXPECT_EQ(actual.filled_quantity, expected.filled_quantity);
    EXPECT_EQ(actual.remaining_quantity, expected.remaining_quantity);
    ASSERT_EQ(actual.trades.size(), expected.trades.size());
    for (std::size_t i = 0; i < actual.trades.size(); ++i) {
        ExpectSameTrade(actual.trades[i], expected.trades[i], i);
    }

    const bool actual_has_handle = actual.handle != llmes::matching_core::kInvalidHandle;
    const bool expected_has_handle = expected.handle != llmes::matching_core::kInvalidHandle;
    EXPECT_EQ(actual_has_handle, expected_has_handle);
}

class ResultHarness {
public:
    explicit ResultHarness(std::size_t capacity = 200000)
        : actual_(capacity) {}

    void add_limit(std::uint64_t order_id,
                   llmes::matching_core::Side side,
                   std::int64_t price,
                   std::uint64_t quantity,
                   std::uint64_t timestamp) {
        const auto expected =
            expected_.add_limit_order(order_id, side, price, quantity, timestamp);
        const auto actual =
            actual_.add_limit_order(order_id, side, price, quantity, timestamp);
        ExpectSameAddResult(actual, expected);
        apply_result(order_id, side, price, actual);
    }

    void add_market(std::uint64_t order_id,
                    llmes::matching_core::Side side,
                    std::uint64_t quantity,
                    std::uint64_t timestamp) {
        const auto expected =
            expected_.add_market_order(order_id, side, quantity, timestamp);
        const auto actual =
            actual_.add_market_order(order_id, side, quantity, timestamp);
        ExpectSameAddResult(actual, expected);
        apply_trades(actual);
    }

    void modify(std::uint64_t order_id,
                llmes::matching_core::Side side,
                std::int64_t price,
                std::uint64_t quantity,
                std::uint64_t timestamp) {
        auto live_it = live_.find(order_id);
        ASSERT_NE(live_it, live_.end());

        const llmes::matching_core::OrderHandle handle = live_it->second.handle;
        live_.erase(live_it);

        const auto expected =
            expected_.modify_order(order_id, side, price, quantity, timestamp);
        const auto actual =
            actual_.modify_order(handle, side, price, quantity, timestamp);
        ExpectSameAddResult(actual, expected);
        apply_result(order_id, side, price, actual);
    }

    void cancel(std::uint64_t order_id) {
        auto live_it = live_.find(order_id);
        ASSERT_NE(live_it, live_.end());

        const llmes::matching_core::OrderHandle handle = live_it->second.handle;
        (void)actual_.cancel_order(handle);
        (void)expected_.cancel_order(order_id);
        live_.erase(live_it);
    }

    [[nodiscard]] bool has_live_orders() const noexcept {
        return !live_.empty();
    }

    [[nodiscard]] std::uint64_t live_order_at(std::size_t index) const {
        auto it = live_.begin();
        std::advance(it, static_cast<std::ptrdiff_t>(index % live_.size()));
        return it->first;
    }

    [[nodiscard]] std::size_t live_size() const noexcept {
        return live_.size();
    }

private:
    void apply_result(std::uint64_t order_id,
                      llmes::matching_core::Side side,
                      std::int64_t price,
                      const llmes::matching_core::AddResult& result) {
        apply_trades(result);
        if (result.code == llmes::matching_core::ErrorCode::Success &&
            result.remaining_quantity > 0) {
            ASSERT_NE(result.handle, llmes::matching_core::kInvalidHandle);
            live_[order_id] =
                LiveOrder{result.handle, side, price, result.remaining_quantity};
        }
    }

    void apply_trades(const llmes::matching_core::AddResult& result) {
        for (const auto& trade : result.trades) {
            auto live_it = live_.find(trade.maker_order_id);
            ASSERT_NE(live_it, live_.end())
                << "maker id missing from harness live map";
            ASSERT_GE(live_it->second.quantity, trade.quantity);
            live_it->second.quantity -= trade.quantity;
            if (live_it->second.quantity == 0) {
                live_.erase(live_it);
            }
        }
    }

    llmes::matching_core::OrderBook actual_;
    ReferenceOrderBook expected_{};
    std::unordered_map<std::uint64_t, LiveOrder> live_{};
};

llmes::matching_core::Side RandomSide(std::mt19937_64& rng) {
    return (rng() & 1ULL) == 0 ? llmes::matching_core::Side::Buy : llmes::matching_core::Side::Sell;
}

std::int64_t RandomPrice(std::mt19937_64& rng) {
    const auto offset = static_cast<std::int64_t>(rng() % 41ULL) - 20;
    return 1000 + offset;
}

}  // namespace

TEST(OrderBookAddResult, LimitAndMarketResultsMatchReference) {
    ResultHarness h;

    h.add_limit(1, llmes::matching_core::Side::Buy, 100, 10, 1);
    h.add_limit(2, llmes::matching_core::Side::Sell, 101, 5, 2);
    h.add_limit(3, llmes::matching_core::Side::Buy, 101, 7, 3);
    h.add_market(4, llmes::matching_core::Side::Sell, 20, 4);
    h.add_limit(5, llmes::matching_core::Side::Buy, 100, 0, 5);
    h.add_market(6, llmes::matching_core::Side::Buy, 0, 6);
}

TEST(OrderBookAddResult, FifoTradesAreVisibleInAddResultOnly) {
    ResultHarness h;

    h.add_limit(10, llmes::matching_core::Side::Sell, 100, 3, 10);
    h.add_limit(11, llmes::matching_core::Side::Sell, 100, 4, 11);
    h.add_limit(12, llmes::matching_core::Side::Buy, 100, 5, 12);
}

TEST(OrderBookAddResult, LargeSingleLevelFifoSweepMatchesReferenceAddResult) {
    ResultHarness h(4096);

    for (std::uint64_t i = 0; i < 1024; ++i) {
        h.add_limit(10000 + i, llmes::matching_core::Side::Sell, 100, 1, i);
    }

    h.add_limit(20000, llmes::matching_core::Side::Buy, 100, 1024, 20000);
}

TEST(OrderBookAddResult, MarketSweepAcrossManyPriceLevelsMatchesReferenceAddResult) {
    ResultHarness h(4096);

    for (std::uint64_t i = 0; i < 256; ++i) {
        h.add_limit(30000 + i,
                    llmes::matching_core::Side::Sell,
                    1000 + static_cast<std::int64_t>(i),
                    2,
                    i);
    }

    h.add_market(40000, llmes::matching_core::Side::Buy, 512, 40000);
}

TEST(OrderBookAddResult, ModifyResultMatchesCancelThenAddSemantics) {
    ResultHarness h;

    h.add_limit(20, llmes::matching_core::Side::Sell, 105, 10, 20);
    h.add_limit(21, llmes::matching_core::Side::Buy, 100, 4, 21);
    h.modify(20, llmes::matching_core::Side::Sell, 100, 7, 22);
    h.add_market(23, llmes::matching_core::Side::Buy, 10, 23);
}

TEST(OrderBookAddResult, CancelAffectsOnlyLaterAddResults) {
    ResultHarness h;

    h.add_limit(30, llmes::matching_core::Side::Sell, 100, 5, 30);
    h.add_limit(31, llmes::matching_core::Side::Sell, 101, 5, 31);
    h.cancel(30);
    h.add_market(32, llmes::matching_core::Side::Buy, 8, 32);
}

TEST(OrderBookAddResult, RandomStatefulReplayMatchesReferenceAddResults) {
    ResultHarness h(30000);
    std::mt19937_64 rng(0x5eed1234ULL);
    std::uint64_t next_order_id = 1000;

    for (std::uint64_t step = 0; step < 10000; ++step) {
        SCOPED_TRACE("step " + std::to_string(step));
        const std::uint64_t roll = rng() % 100;

        if (!h.has_live_orders() || roll < 45) {
            const auto id = next_order_id++;
            const auto side = RandomSide(rng);
            const auto price = RandomPrice(rng);
            const auto quantity = 1 + (rng() % 25);
            h.add_limit(id, side, price, quantity, step);
            continue;
        }

        if (roll < 60) {
            const auto id = next_order_id++;
            const auto side = RandomSide(rng);
            const auto quantity = 1 + (rng() % 80);
            h.add_market(id, side, quantity, step);
            continue;
        }

        const auto target = h.live_order_at(static_cast<std::size_t>(rng()));
        if (roll < 80) {
            h.cancel(target);
            continue;
        }

        const auto side = RandomSide(rng);
        const auto price = RandomPrice(rng);
        const auto quantity = (rng() % 50 == 0) ? 0 : (1 + (rng() % 30));
        h.modify(target, side, price, quantity, step);
    }
}
