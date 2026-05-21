/**
 * @file flat_hash_map.cpp
 * @brief Implementation of open-addressing flat hash map (Phase 2c).
 *
 * All documented in flat_hash_map.hpp — this file contains the out-of-line
 * method bodies for @ref matching::HashTable.
 */

#include "matching/flat_hash_map.hpp"

/**
 * @copydoc HashTable::HashTable
 */
matching::HashTable::HashTable(std::size_t expected)
    : capacity_(next_power_of_2(expected > 4 ? expected : 4)),
      mask_(capacity_ - 1),
      slots_(capacity_) {}

/**
 * @copydoc HashTable::insert
 */
bool matching::HashTable::insert(std::uint64_t key, void* value) {
    if (size_ + tombstones_ > static_cast<std::size_t>(capacity_ * kLoadFactorMax))
        rehash();

    std::size_t idx = hash(key) & mask_;

    while (slots_[idx].state == State::OCCUPIED) {
        if (slots_[idx].key == key) {
            slots_[idx].value = value;
            return false;
        }
        idx = (idx + 1) & mask_;
    }

    slots_[idx] = {key, value, State::OCCUPIED};
    ++size_;
    return true;
}

/**
 * @copydoc HashTable::rehash
 */
void matching::HashTable::rehash() {
    std::vector<Slot> old = std::move(slots_);
    capacity_ = next_power_of_2(capacity_ * 2);
    slots_.resize(capacity_);

    mask_ = capacity_ - 1;
    size_ = 0;
    tombstones_ = 0;

    for (auto& slot : old) {
        if (slot.state == State::OCCUPIED)
            insert(slot.key, slot.value);
    }
}

/**
 * @copydoc HashTable::find
 */
void* matching::HashTable::find(std::uint64_t key) {
    std::size_t idx = hash(key) & mask_;

    while (slots_[idx].state != State::EMPTY) {
        if (slots_[idx].key == key)
            return slots_[idx].value;
        idx = (idx + 1) & mask_;
    }

    return nullptr;
}

/**
 * @copydoc HashTable::erase
 */
bool matching::HashTable::erase(std::uint64_t key) {
    std::size_t idx = hash(key) & mask_;

    while (slots_[idx].state != State::EMPTY) {
        if (slots_[idx].key == key) {
            slots_[idx].state = State::TOMBSTONE;
            ++tombstones_;
            --size_;
            return true;
        }
        idx = (idx + 1) & mask_;
    }

    return false;
}
