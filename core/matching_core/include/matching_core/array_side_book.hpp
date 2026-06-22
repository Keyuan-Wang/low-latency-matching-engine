#pragma once

#include "types.hpp"
#include "price_level.hpp"
#include "occupancy_tree.hpp"

#include <cassert>
#include <vector>


namespace llmes::matching_core {

template <bool IsAsk>
class ArraySideBook {
public:
    static constexpr std::int64_t kDefaultBasePrice = 0;
    static constexpr std::size_t kDefaultPriceCount = OccupancyTree::kBitCount;

    explicit ArraySideBook(std::int64_t base_price = kDefaultBasePrice, std::size_t price_count = kDefaultPriceCount)
        : base_price_(base_price)
        , price_count_(price_count)
        , levels_(price_count) {

        assert(price_count_ > 0);
        assert(price_count_ == OccupancyTree::kBitCount);
        assert((price_count_ % 64) == 0);
    }

    [[nodiscard]] [[gnu::always_inline]] bool empty() noexcept { return !has_best_; }

    [[nodiscard]] [[gnu::always_inline]] std::int64_t best_price() noexcept {
        assert(has_best_);
        return price_of(best_price_idx_);
    };

    [[nodiscard]] [[gnu::always_inline]] PriceLevel& best_level() noexcept {
        assert(has_best_);
        return levels_[best_price_idx_];
    };

    [[gnu::always_inline]] void prefetch_level(std::uint64_t price) noexcept {
        const std::size_t idx = idx_of(price);
        __builtin_prefetch(&levels_[idx], 1, 3);
    }

    PriceLevel* get_or_create(std::int64_t price) noexcept {
        const std::size_t idx = idx_of(price);

        active_tree.set(idx);

        if (!has_best_ || better_idx(idx, best_price_idx_)) {
            best_price_idx_ = idx;
            has_best_ = true;
        }

        return &levels_[idx];
    };

    void erase_best() noexcept {
        assert(has_best_);
        assert(levels_[best_price_idx_].empty());

        active_tree.clear(best_price_idx_);

        // Note in benchmark we make sure the book will never be empty
        if (active_tree.empty()) [[unlikely]] {
            has_best_ = false;
            return;
        }

        // find the next best price
        best_price_idx_ = active_tree.template next_best<IsAsk>(best_price_idx_);

        // to make sure the best price level is exposed to outside callers
        clear_ghost_best_level();
    }

private:
    std::int64_t base_price_ = kDefaultBasePrice;
    std::size_t price_count_ = kDefaultPriceCount;

    std::vector<PriceLevel> levels_;
    OccupancyTree active_tree;

    std::size_t best_price_idx_ = 0;
    bool has_best_ = false;


    [[nodiscard]] [[gnu::always_inline]] std::size_t idx_of(std::int64_t price) const noexcept {
        assert(price >= base_price_);

        const std::int64_t diff = price - base_price_;
        assert(diff >= 0);
        assert(static_cast<std::size_t>(diff) < price_count_);

        return static_cast<std::size_t>(diff);
    }


    [[nodiscard]] [[gnu::always_inline]] std::int64_t price_of(std::size_t idx) const noexcept {
        assert(idx < price_count_);

        return base_price_ + static_cast<std::int64_t>(idx);
    };


    [[nodiscard]] [[gnu::always_inline]] static bool better_idx(std::size_t idx, std::size_t best_price_idx) noexcept {
        if constexpr (IsAsk)    return idx < best_price_idx;    // for ask book, better price is smaller
        else                    return idx > best_price_idx;    // for bid book, better price is larger
    }


    void clear_ghost_best_level() noexcept {
        while (has_best_ && levels_[best_price_idx_].empty()) {
            active_tree.clear(best_price_idx_);

            if (active_tree.empty()) [[unlikely]] {
                has_best_ = false;
                return;
            }

            best_price_idx_ = active_tree.template next_best<IsAsk>(best_price_idx_);
        }
    }

};

}   // namespace llmes::matching_core
