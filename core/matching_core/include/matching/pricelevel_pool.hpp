#pragma once

#include "price_level.hpp"

#include <cassert>
#include <vector>

namespace matching {

/**
 * @brief Freelist allocator for @ref PriceLevel objects on one book side.
 *
 * @details Storage is a single @c std::vector<Slot> where each @c Slot embeds a
 * @ref PriceLevel and a freelist link. @c acquire() returns @c &(slot.level);
 * @c release() recovers the parent @c Slot from that pointer (standard-layout
 * first member) and pushes it back on the stack.
 *
 * Only @ref CachedSideBook may call @c release(). Ring slots and cold-map nodes
 * hold non-owning @c PriceLevel* into this pool.
 */
class PriceLevelPool {
private:
    struct Slot {
        PriceLevel level;
        Slot*      next = nullptr;
    };

public:
    explicit PriceLevelPool(std::size_t capacity)  {
        pool_.resize(capacity);
    
        Slot* p = nullptr;
        for (auto& slot : pool_) {
            slot.next = p;
            free_head_ = &slot;
    
            p = &slot;
        }
    }

    /** @return Pointer to a reset-ready level, or asserts if the pool is exhausted. */
    [[nodiscard]] [[gnu::always_inline]] inline PriceLevel* acquire() noexcept {
        assert(free_head_ != nullptr && "PriceLevelPool exhausted");

        Slot* slot = free_head_;
        free_head_ = slot->next;

        return &(slot->level);
    }

    /**
     * @brief Return an empty level to the freelist.
     * @pre @p level came from this pool and @p level->empty().
     */
    [[gnu::always_inline]] void release(PriceLevel* level) noexcept {
        assert(level != nullptr && level->empty());

        level->reset();

        // level points at Slot::level (first member); recover the Slot header.
        auto* slot = reinterpret_cast<Slot*>(level);

        slot->next = free_head_;
        free_head_ = slot;
    }

    [[nodiscard]] [[gnu::always_inline]] std::size_t capacity() const noexcept { return pool_.size(); }

private:
    std::vector<Slot> pool_;
    Slot*             free_head_ = nullptr;
};

}   // namespace matching
