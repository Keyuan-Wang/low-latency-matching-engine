#pragma once

#include "price_level.hpp"
#include "types.hpp"

#include <array>
#include <bit>
#include <cassert>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace matching {

template <std::size_t N>
struct uint_from_size;

template <>
struct uint_from_size<8> { using type = std::uint8_t; };

template <>
struct uint_from_size<16> { using type = std::uint16_t; };

template <>
struct uint_from_size<32> { using type = std::uint32_t; };

template <>
struct uint_from_size<64> { using type = std::uint64_t; };

/**
 * @brief Fixed-size ring of the nearest @c RingSize price ticks around the best quote.
 *
 * @tparam IsAsk @c true for the ask side, @c false for the bid side.
 *
 * @details Each slot may reference one pooled @ref PriceLevel (non-owning pointer).
 * @ref CachedSideBook owns pool acquire/release; this class only maintains slot
 * occupancy and pointer wiring.
 *
 * A slot is live when its bit is set in @c live_mask_, @c price != @ref kNoPrice,
 * and @c level is non-null. Cleared slots always reset @c price to @ref kNoPrice
 * so @c slot.price == query_price in the caller is a safe hit test.
 */
template <bool IsAsk>
class RingBuffer {
public:
    /** Number of price ticks kept in the ring. Must be a power of two. */
    static constexpr std::size_t   RingSize = 16;

    /** Bit mask for index wrap: @c idx & kMask == idx % RingSize. */
    static constexpr std::size_t   kMask    = RingSize - 1;

    /** Marks an unused slot; no legitimate order price equals @c INT64_MIN. */
    static constexpr std::int64_t  kNoPrice = INT64_MIN;

    static_assert((RingSize & (RingSize - 1)) == 0, "RingSize must be a power of 2");
    static_assert(RingSize <= 64, "live_mask_ is uint64_t; RingSize cannot exceed 64");

private:
    /** One ring cell: tick price plus optional non-owning level pointer. */
    struct Slot {
        std::int64_t  price = kNoPrice;
        PriceLevel*   level{};
    };

    /** Cached best tick; equals @c slots_[anchor_].price when non-empty. */
    std::int64_t    best_price_  = kNoPrice;

    /** Ring index of the current best (most aggressive) live slot. */
    std::size_t     anchor_      = 0;

    /** Bit @c i set iff @c slots_[i] is live. */
    using MaskType = typename uint_from_size<RingSize>::type;
    MaskType  live_mask_   = 0;

    std::array<Slot, RingSize>      slots_;

public:
    RingBuffer()                             = default;
    RingBuffer(const RingBuffer&)            = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    /**
     * @brief Tick offset from best to @p price in the side's aggression direction.
     * @param price Query tick.
     * @return @c 0 at best; positive for worse ticks inside/outside the window;
     *         negative if @p price is strictly better than current best.
     * @pre @ref empty() is false.
     */
    [[gnu::always_inline]] std::int64_t rank(std::int64_t price) const noexcept {
        assert(best_price_ != kNoPrice && "rank() called on empty ring");
        // Ask: lower price is better. Bid: higher price is better.
        if constexpr (IsAsk) return price - best_price_;
        else                 return best_price_ - price;
    }

    /**
     * @brief Whether offset @p r falls inside the hot window @c [0, RingSize).
     * @param r Output of @ref rank().
     *
     * Casting negative @p r to @c uint64_t makes it huge, so one comparison
     * covers both "better than best" and "farther than window".
     */
    [[gnu::always_inline]] bool in_hot_window(std::int64_t r) const noexcept {
        return static_cast<std::uint64_t>(r) < RingSize;
    }

    /**
     * @brief Physical ring index for logical offset @p r from @c anchor_.
     * @param r Output of @ref rank().
     * @pre @ref empty() is false.
     *
     * Negative @p r wraps correctly because @c RingSize is a power of two
     * and we mask with @ref kMask instead of using integer modulo.
     */
    [[gnu::always_inline]] std::size_t idx_of(std::int64_t r) const noexcept {
        return (anchor_ + static_cast<std::size_t>(r)) & kMask;
    }

    /** @return @c true when no slot is live. */
    [[gnu::always_inline]] bool empty() const noexcept { return live_mask_ == 0; }

    /** @return Current best tick (undefined if empty). */
    [[gnu::always_inline]] std::int64_t best_price() const noexcept { return best_price_; }

    /** @return Non-owning pointer to the level at @c anchor_. */
    [[gnu::always_inline]] PriceLevel* best_level() const noexcept { return slots_[anchor_].level; }

    /** @return Ring index of the best slot. */
    [[gnu::always_inline]] std::size_t anchor() const noexcept { return anchor_; }

    [[gnu::always_inline]] Slot&       slot(std::size_t i)       noexcept { return slots_[i]; }
    [[gnu::always_inline]] const Slot& slot(std::size_t i) const noexcept { return slots_[i]; }

    /**
     * @brief Install a new level at ring index @p idx.
     * @param idx Target slot (caller computes via @ref idx_of()).
     * @param price Tick stored in the slot; must not be @ref kNoPrice.
     * @param level Non-owning pooled level; must be non-null.
     * @pre @c slots_[idx] is vacant (@ref Invariant 3 in phase7 design doc).
     */
     [[gnu::always_inline]] void insert(std::size_t idx, std::int64_t price, PriceLevel* level) noexcept {
        assert(price != kNoPrice);
        assert(level != nullptr);
        slots_[idx].price = price;
        slots_[idx].level = level;
        live_mask_ |= (static_cast<MaskType>(1) << idx);
    }

    /**
     * @brief Clear the slot; does not return the level to @ref PriceLevelPool.
     * @pre @c slots_[idx].level is null or already empty (no resting orders).
     */
     [[gnu::always_inline]] void remove(std::size_t idx) noexcept {
        assert(
            (slots_[idx].level == nullptr || slots_[idx].level->empty()) &&
            "remove() called on non-empty PriceLevel — resting orders would be leaked"
        );
        live_mask_ &= ~(static_cast<MaskType>(1) << idx);
        slots_[idx].price = kNoPrice;
        slots_[idx].level = nullptr;
    }

    /**
     * @brief Detach the level pointer at @p idx and clear the slot.
     * @return Non-owning pooled level (caller typically stores it in the cold map).
     */
    [[nodiscard]] [[gnu::always_inline]] PriceLevel* evict(std::size_t idx) noexcept {
        live_mask_ &= ~(static_cast<MaskType>(1) << idx);
        slots_[idx].price = kNoPrice;
        return std::exchange(slots_[idx].level, nullptr);
    }

    /**
     * @brief Find the next live slot toward worse prices.
     * @return Offset from @c anchor_, or @c -1 if nothing is live.
     *
     * Rotates @c live_mask_ so bit @c anchor_ becomes bit 0, then uses
     * @c countr_zero. Because @c MaskType is exactly @c RingSize bits wide,
     * @c std::rotr performs a natural N-bit rotation with no masking needed.
     */
    [[gnu::always_inline]] int next_live_offset() const noexcept {
        const std::uint64_t rotated = std::rotr(live_mask_, static_cast<int>(anchor_));
        return rotated ? std::countr_zero(rotated) : -1;
    }

    /**
     * @brief Repoint the ring origin without touching slot contents.
     * @param idx New anchor index; slot @p idx should already match @p price.
     * @param price New best tick written to @c best_price_.
     */
    [[gnu::always_inline]] void set_anchor(std::size_t idx, std::int64_t price) noexcept {
        assert(price != kNoPrice);
        anchor_     = idx;
        best_price_ = price;
    }

    /**
     * @brief Drop all live bits after slots were individually cleared.
     * @note @c anchor_ is left stale; the next @ref set_anchor() overwrites it.
     */
    [[gnu::always_inline]] void reset() noexcept {
        live_mask_  = 0;
        best_price_ = kNoPrice;
    }
};

}   // namespace matching
