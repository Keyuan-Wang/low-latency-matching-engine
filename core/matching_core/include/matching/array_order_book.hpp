#pragma once

#include "types.hpp"
#include "price_level.hpp"
#include "occupancy_tree.hpp"

#include <cassert>
#include <vector>


namespace matching {

template <bool IsAsk>
class ArraySideBook {
public:
    static constexpr std::int64_t kDefaultBasePrice = 0;
    static constexpr std::size_t kDefaultPriceCount = 1u << 16;

    explicit ArraySideBook(std::int64_t base_price = kDefaultBasePrice)
        : base_price_(base_price)
        , price_count_(kDefaultPriceCount)
        , levels_(kDefaultPriceCount) {

        assert(price_count_ > 0);
        assert(price_count_ == OccupancyTree::kBitCount);
        assert((price_count_ % 64) == 0);

        for (std::size_t i = 0; i < kDefaultPriceCount; ++i) {
            if constexpr (IsAsk)    levels_[i].bind_owner(Side::Sell, i);
            else                    levels_[i].bind_owner(Side::Buy, i);
        }
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

    PriceLevel* get_or_create(std::int64_t price) noexcept {
        const std::size_t idx = idx_of(price);

        if (levels_[idx].empty()) [[unlikely]]
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
    }


    [[gnu::always_inline]] void clear(std::size_t bit_pos) noexcept {
        if (bit_pos == best_price_idx_)
            erase_best();
        else [[likely]]
            active_tree.clear(bit_pos); 
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

};

}   // namespace matching
