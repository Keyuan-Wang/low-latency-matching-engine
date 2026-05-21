/**
 * @file flat_hash_map.hpp
 * @brief Open-addressing flat hash map with linear probing (Phase 2c).
 *
 * Replaces std::unordered_map<uint64_t, Order*> with a cache-local
 * alternative.  All slots live in a single contiguous std::vector — no
 * per-entry heap allocations, no pointer chasing during lookup.
 *
 * Key design decisions:
 *   - Open addressing + linear probing → probe chain fits in 1–2 cache lines
 *   - Power-of-2 capacity → index via `hash & mask`, no modulo
 *   - Tombstone-based deletion → O(1) amortised erase without shifting
 *   - Load-factor-triggered rehash → also purges accumulated tombstones
 */

#pragma once

#include <cstdint>
#include <vector>

namespace matching {

/** @brief Slot state for the open-addressing hash table. */
enum class State : std::uint8_t {
    EMPTY,     ///< Never used — probe chain ends here.
    OCCUPIED,  ///< Holds a live key/value pair.
    TOMBSTONE  ///< Was occupied but erased — probe chain continues.
};

/**
 * @brief Minimal open-addressing hash table for Order* lookup.
 *
 * @par Thread safety
 * Not thread-safe; external synchronisation required.
 */
class HashTable {
private:
    /** @brief One slot in the open-addressing array. */
    struct Slot {
        std::uint64_t key;     ///< Order id.
        void*         value;   ///< Pointer to the matching::Order.
        State         state = State::EMPTY;
    };

    std::size_t        size_       = 0;  ///< Live entry count.
    std::size_t        tombstones_ = 0;  ///< Tombstone count (reclaimed on rehash).
    std::size_t        capacity_   = 0;  ///< Current capacity (always power of 2).
    std::uint64_t      mask_       = 0;  ///< capacity_ - 1 (index mask).
    std::vector<Slot>  slots_{};         ///< Backing store — contiguous array.

    /** @brief Bit mixer: multiply by golden ratio to smear high bits into low bits. */
    static std::size_t hash(std::uint64_t key) {
        return static_cast<std::size_t>(key * 0x9E3779B97F4A7C15ULL);
    }

    /** @brief Round up to the next power of two (or 1 if v ≤ 1). */
    static std::size_t next_power_of_2(std::uint64_t v) {
        if (v <= 1) return 1;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        v |= v >> 32;
        ++v;
        return static_cast<std::size_t>(v);
    }

    /** @brief Grow to ~2× current capacity and rehash all live entries. */
    void rehash();

public:
    static constexpr double kLoadFactorMax = 0.60;

    /**
     * @param expected  Expected number of live entries.
     *                  Capacity is rounded to the next power of
     *                  two and scaled by 1/kLoadFactorMax.
     */
    explicit HashTable(std::size_t expected = 1024);

    /** @brief Insert or update a key/value pair. */
    bool insert(std::uint64_t key, void* value);

    /** @brief Look up a key; return value or nullptr. */
    void* find(std::uint64_t key);

    /** @brief Erase a key (tombstone).  True if the key existed. */
    bool erase(std::uint64_t key);

    // --- accessors ---

    [[nodiscard]] std::size_t size()       const noexcept { return size_; }
    [[nodiscard]] std::size_t tombstones() const noexcept { return tombstones_; }
    [[nodiscard]] std::size_t capacity()   const noexcept { return capacity_; }
    [[nodiscard]] bool        empty()      const noexcept { return size_ == 0; }
};

}  // namespace matching
