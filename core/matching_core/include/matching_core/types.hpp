#pragma once

#include <cstdint>
#include <vector>

namespace llmes::matching_core {

class PriceLevel;  // forward decl — full definition in price_level.hpp

using OrderHandle = std::uint32_t;  // Direct index into the order pool.
inline constexpr OrderHandle kInvalidHandle = UINT32_MAX;

template <bool IsAsk>
struct PriceCompare;

template <>
struct PriceCompare<true> {
    bool operator()(std::int64_t lhs, std::int64_t rhs) const noexcept {
        return lhs < rhs;
    }
};

template <>
struct PriceCompare<false> {
    bool operator()(std::int64_t lhs, std::int64_t rhs) const noexcept {
        return lhs > rhs;
    }
};


enum class Side {
    Buy,   ///< Bid side (buy book).
    Sell,  ///< Ask side (sell book).
};


struct Trade {
    std::uint64_t taker_order_id;  ///< Aggressor order identifier.
    std::uint64_t maker_order_id;  ///< Resting order identifier.
    std::int64_t price;            ///< Execution price (maker level price).
    std::uint64_t quantity;        ///< Traded quantity.
};


enum class ErrorCode {
    Success,                   ///< Operation completed as requested.
    InvalidQuantity,           ///< Non-positive quantity (e.g. zero).
    DuplicateOrderId,          ///< Order id already present on the book.
    UnknownOrderId,            ///< Cancel: id not on book (recorded as pending cancel).
    PendingCancelExists,       ///< Insert rejected: id was cancelled before insert.
    MarketRemainderCancelled,  ///< Market order: leftover quantity not posted to book.
};



struct AddResult {
    ErrorCode code{ErrorCode::Success};  ///< Primary status of the request.

    std::uint64_t initial_quantity{0};   ///< Requested quantity at entry.
    std::uint64_t filled_quantity{0};      ///< Total matched quantity.
    std::uint64_t remaining_quantity{0};   ///< Unfilled quantity after matching / rest.

    OrderHandle handle{kInvalidHandle};

    std::vector<Trade> trades{};           ///< Individual fills, in chronological order.
};


struct Order {
    // --- business data ---      
    std::uint64_t id;         ///< Unique order identifier.
    std::int64_t price;       ///< Limit price while resting on the book.
    std::uint64_t quantity;   ///< Remaining quantity.
    std::uint64_t timestamp;  ///< Application timestamp (ordering / audit).

    // --- intrusive list links ---
    Order* prev = nullptr;
    Order* next = nullptr;

    // --- parent level links ---
    PriceLevel* parent_level = nullptr;
};

}