/**
 * @file flat_hash_map.cpp
 * @brief Implementation of open-addressing flat hash map (Phase 2d).
 *
 * All documented in flat_hash_map.hpp — this file contains the out-of-line
 * method bodies for @ref matching::HashTable.
 */

#include "matching/flat_hash_map.hpp"
#include <utility>

matching::HashTable::HashTable(std::size_t expected)
    : capacity_(next_power_of_2(expected > 4 ? expected : 4)),
      mask_(capacity_ - 1),
      slots_(capacity_) {}

bool matching::HashTable::insert(std::uint64_t key, void* value) {
    if (size_ > static_cast<std::size_t>(capacity_ * kLoadFactorMax))
        rehash();

    std::uint32_t dist = 0;
    std::size_t idx = hash(key) & mask_;

    while (true) {
        if (slots_[idx].state == State::EMPTY) {
            slots_[idx] = {key, value, dist, State::OCCUPIED};
            ++size_;
            return true;
        }

        if (slots_[idx].key == key) {
            slots_[idx].value = value;
            return false;
        }

        // Robin-Hood exchange: if the incoming entry has probed further than
        // the resident entry, swap — the "richer" (closer-to-ideal) entry
        // yields the slot to the "poorer" one, keeping max probe distance bounded.
        if (dist > slots_[idx].probe_dist) {
            std::swap(key, slots_[idx].key);
            std::swap(value, slots_[idx].value);
            std::swap(dist, slots_[idx].probe_dist);
        }

        // Power-of-2 mask replaces modulo — the CPU computes & in 1 cycle
        // vs. 11-25 cycles for a division-based modulo.
        idx = (idx + 1) & mask_;
        ++dist;
    }
}

void matching::HashTable::rehash() {
    std::vector<Slot> old = std::move(slots_);
    capacity_ = next_power_of_2(capacity_ * 2);
    slots_.resize(capacity_);

    mask_ = capacity_ - 1;
    size_ = 0;

    for (auto& slot : old) {
        if (slot.state == State::OCCUPIED)
            insert(slot.key, slot.value);
    }
}

void* matching::HashTable::find(std::uint64_t key) {
    std::size_t idx = hash(key) & mask_;

    while (slots_[idx].state != State::EMPTY) {
        if (slots_[idx].key == key)
            return slots_[idx].value;
        idx = (idx + 1) & mask_;
    }

    return nullptr;
}

bool matching::HashTable::erase(std::uint64_t key) {
    std::size_t idx = hash(key) & mask_;

    while (slots_[idx].state != State::EMPTY) {
        if (slots_[idx].key == key) {

            slots_[idx].state = State::EMPTY;
            --size_;

            // Backward-shift deletion: compact the cluster by shifting entries
            // toward their ideal positions, eliminating the need for tombstones.
            //
            // After clearing the target slot at `gap`, examine each subsequent
            // occupied slot: if `gap` lies on that entry's probe path (i.e. the
            // distance from `ideal_idx` to `gap` is less than to its current
            // position), the entry can safely shift backward into the gap.
            // The vacated slot becomes the new `gap`, and the scan continues
            // until an EMPTY slot terminates the cluster.
            {
                std::size_t gap = idx;
                idx = (idx + 1) & mask_;

                while (slots_[idx].state != State::EMPTY) {
                    if (slots_[idx].state == State::OCCUPIED) {
                        std::size_t ideal_idx = hash(slots_[idx].key) & mask_;
                        // (gap - ideal) & mask  <  (idx - ideal) & mask  tests
                        // whether gap is strictly between ideal and idx in
                        // probe order — this single expression handles both
                        // non-wrapping and wrapping cases correctly.
                        if (((gap - ideal_idx) & mask_) < ((idx - ideal_idx) & mask_)) {
                            slots_[gap] = slots_[idx];
                            slots_[gap].probe_dist = static_cast<std::uint32_t>((gap - ideal_idx) & mask_);
                            slots_[idx].state = State::EMPTY;
                            gap = idx;
                        }
                    }
                    idx = (idx + 1) & mask_;
                }
                return true;
            }
        }

        idx = (idx + 1) & mask_;
    }

    return false;
}
