#pragma once

#include "price_level.hpp"
#include "types.hpp"
#include "absl/container/flat_hash_map.h"

#include <cstddef>
#include <limits>


namespace matching {

template <bool IsAsk>
class RingBuffer {
private:
    static constexpr int RingSize = 16;     // size of ring buffer

    // Note the price must be signed 64 int, for modulo taking
    static constexpr std::int64_t Invalid = std::numeric_limits<std::int64_t>::min();    // invalid price

    struct Slot {
        std::int64_t price = Invalid;
        PriceLevel level;  // pointer to intrusive list with price
    };

    std::array<Slot, RingSize> ring_buffer_;    // light-weight order book on hot path

    absl::flat_hash_map<std::int64_t, PriceLevel> cold_map_;  // heavy-weight order book on cold path

    std::int64_t best_price_ = Invalid;
    std::size_t best_price_idx_;

    inline std::size_t calc_idx(std::int64_t price) const {
        return (best_price_idx_ + static_cast<std::size_t>(price - best_price_)) & (RingSize - 1);
    };

    inline bool in_hot_window(std::int64_t price) const {
        if (best_price_ == Invalid)     return false;
        if constexpr (IsAsk)    return (price >= best_price_) && (price - best_price_ < RingSize);
        else                    return (price <= best_price_) && (best_price_ - price < RingSize);
    }

    // is a better than b?
    inline bool better(std::int64_t a, std::int64_t b) const {
        if constexpr (IsAsk)    return a < b;       // for ask book, the lower price the better
        else                    return a > b;       // for bid book, the higher price the better
    }

    // binary search
    auto find_cold(std::int64_t price);

    void evict_to_cold(std::size_t idx);

public:
    PriceLevel* find(std::int64_t price);

    void insert(std::int64_t price, Order* order);

    void erase(std::int64_t price);

    std::int64_t best_price() { return best_price_; }
    bool empty() const { return best_price_ == Invalid; }

    PriceLevel* best_price_level() {
        return (best_price_ == Invalid) ? nullptr : &ring_buffer_[best_price_idx_].level;
    }
};

template <bool IsAsk>
PriceLevel* RingBuffer<IsAsk>::find(std::int64_t price) {
    // the book has not been initialized yet
    if (best_price_ == Invalid)     return nullptr;

    std::size_t idx = calc_idx(price);
    auto& slot = ring_buffer_[idx];

    // hot ring hit and correct price
    if (slot.price == price)
        return &(slot.level);
    
    // hot ring hit but outdated price, move it to cold map
    if (!slot.level.empty()) {
        cold_map_[slot.price] = std::move(slot.level);
    }

    // Set this slot to be invalid
    slot.price = Invalid;

    // reaching here means current slot is empty (slot.level.empty())
    auto it = cold_map_.find(price);
    if (it == cold_map_.end())  
        return nullptr;     // current price does not exist
    
    // move PriceLevel from cold map to hot array
    slot.level = std::move(it->second);
    cold_map_.erase(it);
    slot.price = price;

    return &(slot.level);
}

template <bool IsAsk>
void RingBuffer<IsAsk>::insert(std::int64_t price, Order* order) {
    // for empty order book, insert the best price in the middle of hot ring buffer
    if (best_price_ == Invalid) {
        if constexpr (IsAsk)                    // best price for ask book is the lowest price
            best_price_idx_ = 0;
        else
            best_price_idx_ = RingSize - 1;     // best price for bid book is the highest price
        best_price_ = price;
    }
    // for better price, update the best price and idx
    else if (better(price, best_price_)) {
        best_price_idx_ = calc_idx(price);
        best_price_ = price;
    }

    // check if price is within the hot range [best price +- RingSize ticks]
    if (abs(price - best_price_) < RingSize) { 
        std::size_t idx = calc_idx(price);
        auto& slot = ring_buffer_[idx];

        // if hot path hit but outdated data
        // 1. evict it to cold vector; 2. push new price into it
        if (slot.price == Invalid) {
            slot.price = price;
            slot.level.push_back
        }
    }

    
    
    
    
}

}