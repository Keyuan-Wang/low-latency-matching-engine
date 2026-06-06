#pragma once

#include <cassert>
#include <map>
#include <memory>

#include "matching/pricelevel_pool.hpp"
#include "price_level.hpp"
#include "ring_buffer.hpp"
#include "types.hpp"

namespace matching {

/**
 * @brief One book side split into a hot ring and a cold ordered map.
 *
 * @tparam IsAsk @c true for asks, @c false for bids.
 *
 * @details Near-best ticks (@c rank < RingSize) live in @c hot_. Farther ticks
 * use @c cold_, keyed by @ref PriceCompare. While @c hot_ is non-empty, the
 * global best price is always in the ring — @ref erase_best never walks the map
 * to find the next quote.
 *
 * @c cold_ only holds prices strictly outside the current hot window, so a tick
 * cannot exist in both tiers. An empty ring implies an empty cold map.
 *
 * @par Ownership
 * Each side owns a @ref PriceLevelPool. Ring slots and cold-map values are
 * non-owning @c PriceLevel* into that pool. Only this class calls
 * @c pool_.acquire() / @c pool_.release(). Three structural invariants
 * (hot–cold ordering, cold outside window, live-bit ↔ slot consistency) ensure
 * pool slots are never leaked or double-freed; see
 * @c report/phase7_hot_ring_cold_map_design.md.
 */
template <bool IsAsk>
class CachedSideBook {
public:
    /** @return @c true when both tiers are empty. */
    bool empty() const noexcept { return hot_.empty() && cold_.empty(); }

    /** @return Best tick; valid only when @ref empty() is false. */
    std::int64_t best_price() const noexcept { return hot_.best_price(); }

    /** @return Reference to the best @ref PriceLevel in the ring. */
    PriceLevel& best_level() noexcept { return *hot_.best_level(); }

    /**
     * @brief Look up or create the @ref PriceLevel at @p price.
     * @param price Tick to resolve.
     * @return Non-owning pointer to the level (existing or newly created).
     */
    PriceLevel* get_or_create(std::int64_t price);

    /**
     * @brief Remove the drained best level and advance to the next best tick.
     * @pre @ref best_level() is empty (matching already consumed its orders).
     */
    void erase_best();

private:
    using ColdMap = std::map<std::int64_t, PriceLevel*, PriceCompare<IsAsk>>;

    RingBuffer<IsAsk> hot_;   ///< Near-best window (non-owning @c PriceLevel* per slot).
    ColdMap           cold_;  ///< Far ticks; values are non-owning pooled levels.

    PriceLevelPool pool_{100000};  ///< Freelist; sole acquire/release authority for this side.

    /**
     * @brief Whether cold tick @p p now lies inside the hot window.
     * @param p A key currently in @c cold_.
     */
     [[gnu::always_inline]] bool cold_in_window(std::int64_t p) const noexcept {
        return hot_.in_hot_window(hot_.rank(p));
    }

    /**
     * @brief Acquire a pooled level and install it at vacant hot slot @p idx.
     * @pre @c hot_.slot(idx) is vacant (Invariant 3).
     * @return Pooled pointer stored in the ring until remove/evict + release.
     */
     [[gnu::always_inline]] PriceLevel* materialize(std::size_t idx, std::int64_t price) noexcept {
        auto l = pool_.acquire();
        hot_.insert(idx, price, l);
        return l;
    }

    /**
     * @brief Look up or insert @p price in @c cold_.
     * @pre @ref rank(price) >= RingSize (caller guarantees out-of-window).
     * @note On insert, acquires one pool object; key is fresh by Invariant 2.
     */
     [[gnu::always_inline]] PriceLevel* cold_get_or_create(std::int64_t price) noexcept {
        auto [it, inserted] = cold_.try_emplace(price);
        if (inserted) it->second = pool_.acquire();
        return it->second;
    }

    /**
     * @brief Move the most aggressive cold entry into its hot slot.
     * @pre @c cold_ is non-empty and @ref cold_in_window(cold_.begin()->first).
     * @note Does not move the anchor; the promoted tick is worse than current best.
     */
    void promote_cold() noexcept {
        auto it            = cold_.begin();
        std::int64_t p     = it->first;
        std::size_t  idx   = hot_.idx_of(hot_.rank(p));

        assert(hot_.slot(idx).price == RingBuffer<IsAsk>::kNoPrice &&
               "promote_cold(): target hot slot is already occupied");

        hot_.insert(idx, p, it->second);
        cold_.erase(it);
    }

    /**
     * @brief Best tick improved: slide the window and evict slots that fall out.
     * @param price New strictly better tick (@ref rank() < 0).
     */
    PriceLevel* reanchor_to(std::int64_t price) noexcept;

    /** @brief Move every live hot level into @c cold_ (large price jump). */
    void flush_all_to_cold() noexcept;
};


template <bool IsAsk>
PriceLevel* CachedSideBook<IsAsk>::get_or_create(std::int64_t price) {
    // First level on an empty side: anchor slot 0, no cold tier yet.
    if (hot_.empty()) [[unlikely]] {
        hot_.set_anchor(0, price);
        return materialize(0, price);
    }

    const std::int64_t r = hot_.rank(price);

    // Dominant path: tick lands inside the current hot window.
    if (hot_.in_hot_window(r)) [[likely]] {
        std::size_t idx  = hot_.idx_of(r);
        auto&       s    = hot_.slot(idx);
        if (s.price == price) return s.level;  // exact tick hit

        assert(!hot_.slot(idx).level);       // Invariant 3: vacant slot only
        return materialize(idx, price);
    }

    // Strictly better than best: re-center the ring on the new tick.
    if (r < 0) return reanchor_to(price);

    // Beyond the window: fall back to the ordered cold map.
    return cold_get_or_create(price);
}


template <bool IsAsk>
void CachedSideBook<IsAsk>::erase_best() {
    assert(hot_.best_level() != nullptr && hot_.best_level()->empty() &&
           "erase_best() called before the best level was fully drained");

    pool_.release(hot_.slot(hot_.anchor()).level);
    hot_.remove(hot_.anchor());

    // Walk toward worse prices until we find a non-empty level or exhaust hot.
    for (;;) {
        int off = hot_.next_live_offset();

        if (off < 0) {
            // No live hot slot left: pull best from cold or go fully empty.
            if (cold_.empty()) {
                hot_.reset();
                return;
            }
            auto it        = cold_.begin();
            std::int64_t p = it->first;
            hot_.set_anchor(0, p);
            hot_.insert(0, p, it->second);
            cold_.erase(it);
            break;
        }

        std::size_t idx = (hot_.anchor() + static_cast<std::size_t>(off))
                          & RingBuffer<IsAsk>::kMask;
        hot_.set_anchor(idx, hot_.slot(idx).price);

        if (!hot_.slot(idx).level->empty()) break;

        // Ghost slot: level emptied by cancels before erase_best reached it.
        pool_.release(hot_.slot(idx).level);
        hot_.remove(idx);
    }

    // Window moved: any cold tick that now fits must be promoted into hot.
    while (!cold_.empty() && cold_in_window(cold_.begin()->first)) {
        assert(hot_.rank(cold_.begin()->first) > 0 &&
               "promote_cold() about to promote a price better than or equal to best");
        promote_cold();
    }
}


template <bool IsAsk>
PriceLevel* CachedSideBook<IsAsk>::reanchor_to(std::int64_t price) noexcept {
    // d = how many ticks better the new price is than the old best.
    const std::size_t d = static_cast<std::size_t>(-hot_.rank(price));

    if (d >= RingBuffer<IsAsk>::RingSize) {
        // Jump wider than the ring: nothing in hot stays in-window.
        flush_all_to_cold();
        hot_.set_anchor(0, price);
        return materialize(0, price);
    }

    // Compute new anchor index while the ring still uses the old frame.
    const std::size_t new_anchor_idx = hot_.idx_of(hot_.rank(price));

    // Evict the d slots at the "far" end of the window that wrap out of range.
    // Index order: (anchor + RingSize - k) for k = 1..d.
    for (std::size_t k = 1; k <= d; ++k) {
        std::size_t  i = (hot_.anchor() + RingBuffer<IsAsk>::RingSize - k) & RingBuffer<IsAsk>::kMask;
        std::int64_t evicted_price = hot_.slot(i).price;

        if (hot_.slot(i).level) {
            if (hot_.slot(i).level->empty()) {
                pool_.release(hot_.slot(i).level);
                hot_.remove(i);
            } else {
                // Resting orders remain: park the level in cold at its old tick.
                cold_.emplace(evicted_price, hot_.evict(i));
            }
        }
    }

    hot_.set_anchor(new_anchor_idx, price);
    return materialize(new_anchor_idx, price);
}


template <bool IsAsk>
void CachedSideBook<IsAsk>::flush_all_to_cold() noexcept {
    for (std::size_t i = 0; i < RingBuffer<IsAsk>::RingSize; ++i) {
        if (!hot_.slot(i).level) continue;

        std::int64_t p = hot_.slot(i).price;
        if (hot_.slot(i).level->empty()) {
            pool_.release(hot_.slot(i).level);
            hot_.remove(i);
        } else {
            cold_.emplace(p, hot_.evict(i));
        }
    }
    hot_.reset();
}

}   // namespace matching
