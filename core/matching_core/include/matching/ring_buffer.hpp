#pragma once

#include "price_level.hpp"
#include "types.hpp"

#include <cassert>
#include <cstddef>
#include <limits>
#include <memory>
#include <utility>


namespace matching {

template <bool IsAsk>
class RingBuffer {
private:
    struct Slot;
public:
    [[nodiscard]] bool empty() const { return best_price_ == kInvalid; }
    // return the best price in uint64_t
    [[nodiscard]] std::uint64_t best_price() { return static_cast<std::uint64_t>(best_price_); }
    // return the best price level
    [[nodiscard]] PriceLevel* best_price_level() {
        return (best_price_ == kInvalid) ? nullptr : &ring_buffer_[best_price_idx_].level;
    }
    // is price a better price than best price?
    [[nodiscard]] inline bool is_better(std::int64_t a) const {
        if constexpr (IsAsk)    return a < best_price_;       // for ask book, the lower price the better
        else                    return a > best_price_;       // for bid book, the higher price the better
    }


    // calculate the index of price in ring_buffer_
    // before calling this function, must ensure price is in hot window
    [[nodiscard]] inline std::size_t calc_idx(std::int64_t price) const {
        if constexpr (IsAsk)    // for ask book, the best price is at index 0 
            return (best_price_idx_ + static_cast<std::size_t>(price - best_price_)) & (RingSize - 1);
        else                    // for bid book, the best price is at index RingSize - 1
            return (best_price_idx_ + static_cast<std::size_t>(best_price_ - price)) & (RingSize - 1);
    };
    // check if this price is in hot window
    [[nodiscard]] inline bool in_hot_window(std::int64_t price) const noexcept {
        if constexpr (IsAsk)    return price - best_price_ < RingSize;
        else                    return best_price_ - price < RingSize;
    }
    // check if the price slot is outdated
    // before calling this function, must use after calc_idx() and in_hot_window()
    [[nodiscard]] inline bool is_up_to_date(std::int64_t price, std::size_t idx) const noexcept {
        return price == ring_buffer_[idx].price;
    }


    // find a slot with price, before calling this function, it's called take because this operation will take orders from the pricelevel
    // one must ensure the price at ring_buffer_[idx] is up to date (is_up_to_date())
    [[nodiscard]] PriceLevel* take(std::size_t idx) const noexcept {return ring_buffer_[idx].level;}
    // replace ring_buffer_[idx] with new slot, return the old ring_buffer_[idx]
    // this function must be called when is_up_to_date() returns false
    [[nodiscard]] Slot replace(std::int64_t price, std::unique_ptr<PriceLevel> level, std::size_t idx) noexcept {
        Slot new_slot;
        new_slot.price = std::exchange(ring_buffer_[idx].price, price);
        new_slot.level = std::exchange(ring_buffer_[idx].level, std::move(level));
        return new_slot;
    }
    // erase ring_buffer_[idx], set the slot to be invalid, return the empty PriceLevel
    // this function must be called when PriceLevel at ring_buffer_[idx] is empty
    [[nodiscard]] std::unique_ptr<PriceLevel> erase(std::size_t idx) noexcept {
        assert(ring_buffer_[idx].level->empty());
        ring_buffer_[idx].price = kInvalid;
        return std::exchange(ring_buffer_[idx].level, nullptr);
    }


private:
    static constexpr int RingSize = 16;     // size of ring buffer
    // Note the price must be signed 64 int, for modulo taking
    static constexpr std::int64_t kInvalid = std::numeric_limits<std::int64_t>::min();    // invalid price

    std::int64_t best_price_ = kInvalid;
    std::size_t best_price_idx_;

    struct Slot {
        std::int64_t price = kInvalid;
        std::unique_ptr<PriceLevel> level{};  // pointer to PriceLevel, which is allocated during setup
    };

    std::array<Slot, RingSize> ring_buffer_;    // light-weight order book on hot path, set to be increasing from left to right

    // this function should only be called when:
    // 1. the best price level has been taken; 2. the best price level becomes empty
    [[noreturn]] void move_best_price() noexcept {
        assert(ring_buffer_[best_price_idx_].level->empty());

        for (int i = 1; i < RingSize; ++i) {
            if constexpr (IsAsk)  {
                std::size_t new_idx = (best_price_idx_ + i) & (RingSize - 1);
                if (!ring_buffer_[new_idx].level->empty()) {
                    ring_buffer_[new_idx].price = std::exchange(ring_buffer_[best_price_idx_].price, kInvalid);
                    ring_buffer_[new_idx].price = std::exchange(ring_buffer_[best_price_idx_].price, kInvalid);
                }
        }
    }
};

}   // namespace matching