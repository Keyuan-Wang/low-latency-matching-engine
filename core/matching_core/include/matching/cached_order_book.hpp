#pragma once

#include "types.hpp"
#include "ring_buffer.hpp"

namespace matching {

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
