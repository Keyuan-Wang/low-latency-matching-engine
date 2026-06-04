#pragma once

#include <map>

#include "types.hpp"
#include "ring_buffer.hpp"

namespace matching {

class PriceLevelPool;

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


template <bool IsAsk>
class CachedSideBook {
public:
    bool empty() const noexcept { return hot_buffer_.empty() && cold_map_.empty(); }
    std::int64_t best_price()   const noexcept { return hot_buffer_.best_price(); }     // global best price must be in hot_buffer
    PriceLevel&  best_level()         noexcept { return hot_buffer_.best_level(); }

    PriceLevel*  get_or_create(std::int64_t price);     // 48% hot path (add_limit_order)
    void         erase_best();                          // 2% cold path

private:
    RingBuffer<IsAsk>   hot_buffer_;
    std::map<std::int64_t, PriceLevel*, PriceCompare<IsAsk>> cold_map_;
    std::uint64_t cold_best_price_ = RingBuffer<IsAsk>::kNoPrice;   // cache cold.begin()->first
    PriceLevelPool* pool_;                                          // memory pool for PriceLevel object

    bool cold_in_window(std::int64_t cbest) const noexcept {
        return hot_buffer_.in_hot_window(hot_buffer_.rank(cbest));
    }
}
        

template <bool IsAsk>
class CachedSideBook {
public:
    [[nodiscard]] std::int64_t best_price() const noexcept;

    [[nodiscard]] PriceLevel& best_level() const noexcept;

    [[nodiscard]] std::pair<PriceLevel*, bool> get_or_create(std::int64_t price) {
        auto [it, inserted] = cold_map_.try_emplace(price);
        return {&it->second, inserted};
    };

    void erase_best() noexcept;

    [[nodiscard]] bool empty() const noexcept { return cold_.empty() && hot_.empty(); };

private:
    RingBuffer<IsAsk> hot_;

    std::map<std::int64_t, std::unique_ptr<PriceLevel>, PriceCompare<IsAsk>> cold_{};
};

};      // namespace matching
