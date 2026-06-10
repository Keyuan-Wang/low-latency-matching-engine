#include "matching/occupancy_tree.hpp"

#include <bit>


namespace matching {

OccupancyTree::OccupancyTree() {
    levels_.fill(0);
}


void OccupancyTree::set(std::size_t bit) noexcept {
    assert(bit < kBitCount);

    std::size_t idx = bit;

    // -------- loop unrolling (3 loops) --------

    // loop 1: first level: levels_[0:1024]

    // which word?
    std::size_t word_idx = idx / kWordBits;         // leave for compiler optimization
    // which bit?
    std::size_t bit_idx = idx & (kWordBits - 1);    // leave for compiler optimization
    std::uint64_t mask = static_cast<std::uint64_t>(1) << bit_idx;

    std::uint64_t& l1_word = levels_[word_idx];

    bool was_empty = l1_word == 0;

    l1_word |= mask;                                   // set word[bit_idx] to 1

    if (!was_empty) [[likely]]      return;         // the upper level word is already 1

    idx = word_idx;


    // loop 2: second level: levels_[1024:1024+16]

    // which word?
    word_idx = idx / kWordBits;                     // leave for compiler optimization
    // which bit?
    bit_idx = idx & (kWordBits - 1);                // leave for compiler optimization
    mask = static_cast<std::uint64_t>(1) << bit_idx;

    std::uint64_t& l2_word = levels_[1024 + word_idx];

    was_empty = l2_word == 0;

    l2_word |= mask;                                   // set word[bit_idx] to 1

    if (!was_empty) [[likely]]      return;         // the upper level word is already 1

    idx = word_idx;


    // loop 3: last level: levels_[-1]

    // which bit?
    bit_idx = idx & (kWordBits - 1);                // leave for compiler optimization
    mask = static_cast<std::uint64_t>(1) << bit_idx;

    std::uint64_t& l3_word = levels_.back();

    was_empty = l3_word == 0;

    l3_word |= mask;                                   // set word[bit_idx] to 1

    return;
}


void OccupancyTree::clear(std::size_t bit) noexcept {
    assert(bit < kBitCount);

    std::size_t idx = bit;


    // -------- loop unrolling (3 levels) --------

    // loop 1: first level: levels_[0:1024]

    // which word?
    std::size_t word_idx = idx / kWordBits;         // leave for compiler optimization
    // which bit?
    std::size_t bit_idx = idx & (kWordBits - 1);    // leave for compiler optimization
    std::uint64_t mask = static_cast<std::uint64_t>(1) << bit_idx;

    std::uint64_t& l1_word = levels_[word_idx];

    assert((l1_word & mask) != 0 && "clear() requires a currently set bit");

    l1_word &= ~mask;                                  // set word[bit_idx] to 0

    if (l1_word != 0) [[likely]]      return;         // the upper level word is already 1

    idx = word_idx;


    // loop 2: second level: levels_[1024:1024+16]

    // which word?
    word_idx = idx / kWordBits;                     // leave for compiler optimization
    // which bit?
    bit_idx = idx & (kWordBits - 1);                // leave for compiler optimization
    mask = static_cast<std::uint64_t>(1) << bit_idx;

    std::uint64_t& l2_word = levels_[1024 + word_idx];

    assert((l2_word & mask) != 0 && "clear() requires a currently set bit");

    l2_word &= ~mask;                                  // set word[bit_idx] to 0

    if (l2_word != 0) [[likely]]      return;         // the upper level word is already 1

    idx = word_idx;


    // loop 3: last level: levels_[-1]

    // which bit?
    bit_idx = idx & (kWordBits - 1);                // leave for compiler optimization
    mask = static_cast<std::uint64_t>(1) << bit_idx;

    std::uint64_t& l3_word = levels_.back();

    assert((l3_word & mask) != 0 && "clear() requires a currently set bit");

    l3_word &= ~mask;                                  // set word[bit_idx] to 0

    return;
}



std::size_t OccupancyTree::find_next_set(std::size_t bit_pos) const noexcept {
    assert(bit_pos < kBitCount);

    // ------------------------ recursion unrolling (3 levels) ------------------------

    // ---------- loop 1: levels[0:1024] ----------

    const std::size_t l1_word_idx = bit_pos / kWordBits;
    const std::size_t l1_bit_idx = bit_pos & (kWordBits - 1);

    const std::uint64_t l1_word = levels_[l1_word_idx] & mask_high(l1_bit_idx);            // mask from 63th bit to bit_idxth bit

    // found the next best price in current word
    // for benchmark case, this is likely to happen as best price shifts slowly
    if (l1_word != 0) [[likely]] {
        // find the index of next best price
        const std::size_t found = l1_word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(l1_word));
        // is it a valid index?
        assert(found < 1024*64);
        return found;
    }

    // whole book empty, this will never happen in the benchmark case
    assert(l1_word_idx + 1 < 16*64);

    const std::size_t l2_bit_pos = l1_word_idx + 1;


    // ---------- loop 2: levels[1024 : 1024 + 16] ----------

    const std::size_t l2_word_idx = l2_bit_pos / kWordBits;
    const std::size_t l2_bit_idx = l2_bit_pos & (kWordBits - 1);

    const std::uint64_t l2_word = levels_[1024 + l2_word_idx] & mask_high(l2_bit_idx);                   // mask from 63th bit to bit_idxth bit


    if (l2_word != 0) [[likely]] {
        // find the index of next best price at level 2
        const std::size_t level2_word_idx = l2_word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(l2_word));
        // is it a valid index?
        assert(level2_word_idx < 16*64);

        // find the bit in level 1
        const std::uint64_t l1_word = levels_[level2_word_idx];
        assert(l1_word != 0);
        const std::size_t found = level2_word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(l1_word));
        // is it a valid index?
        assert(found < 1024*64);

        return found;
    }

    // whole book empty, this will never happen in the benchmark case
    assert(l2_word_idx + 1 < 16);

    const std::size_t l3_bit_pos = l2_word_idx + 1;


    // ---------- loop 3: levels[-1] ----------

    const std::size_t l3_word_idx = l3_bit_pos / kWordBits;
    const std::size_t l3_bit_idx = l3_bit_pos & (kWordBits - 1);

    const std::uint64_t l3_word = levels_.back() & mask_high(l3_bit_idx);                   // mask from 63th bit to bit_idxth bit
    assert(l3_word != 0);


    // find the index of next best price at level 1
    const std::size_t level3_word_idx = l3_word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(l3_word));
    // is it a valid index?
    assert(level3_word_idx < 64);

    // find the bit in level 2
    const std::uint64_t l2_word_ = levels_[1024 + level3_word_idx];
    assert(l2_word_ != 0);
    const std::size_t level2_word_idx = level3_word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(l2_word_));
    // is it a valid index?
    assert(level2_word_idx < 16*64);

    // find the bit in level 1
    const std::uint64_t l1_word_ = levels_[level2_word_idx];
    assert(l1_word_ != 0);
    const std::size_t found = level2_word_idx * kWordBits + static_cast<std::size_t>(std::countr_zero(l1_word_));
    // is it a valid index?
    assert(found < 1024*64);

    return found;
}




std::size_t OccupancyTree::find_prev_set(std::size_t bit_pos) const noexcept {
    assert(bit_pos < kBitCount);

    // ------------------------ recursion unrolling (3 levels) ------------------------

    // ---------- loop 1: levels[0:1024] ----------

    const std::size_t l1_word_idx = bit_pos / kWordBits;
    const std::size_t l1_bit_idx = bit_pos & (kWordBits - 1);

    const std::uint64_t l1_word = levels_[l1_word_idx] & mask_low(l1_bit_idx);            // mask from 0th bit to bit_idxth bit

    // found the next best price in current word
    // for benchmark case, this is likely to happen as best price shifts slowly
    if (l1_word != 0) [[likely]] {
        // find the index of next best price
        const std::size_t found = l1_word_idx * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l1_word)));
        // is it a valid index?
        assert(found < 1024*64);
        return found;
    }

    // whole book empty, this will never happen in the benchmark case
    assert(l1_word_idx > 0);

    const std::size_t l2_bit_pos = l1_word_idx - 1;


    // ---------- loop 2: levels[1024 : 1024 + 16] ----------

    const std::size_t l2_word_idx = l2_bit_pos / kWordBits;
    const std::size_t l2_bit_idx = l2_bit_pos & (kWordBits - 1);

    const std::uint64_t l2_word = levels_[1024 + l2_word_idx] & mask_low(l2_bit_idx);                   // mask from 0th bit to bit_idxth bit


    if (l2_word != 0) [[likely]] {
        // find the index of next best price at level 2
        const std::size_t level2_word_idx = l2_word_idx * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l2_word)));
        // is it a valid index?
        assert(level2_word_idx < 16*64);

        // find the bit in level 1
        const std::uint64_t l1_word = levels_[level2_word_idx];
        assert(l1_word != 0);
        const std::size_t found = level2_word_idx * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l1_word)));
        // is it a valid index?
        assert(found < 1024*64);

        return found;
    }

    // whole book empty, this will never happen in the benchmark case
    assert(l2_word_idx > 0);

    const std::size_t l3_bit_pos = l2_word_idx - 1;


    // ---------- loop 3: levels[-1] ----------

    const std::uint64_t l3_word = levels_.back() & mask_low(l3_bit_pos);                   // mask from 0th bit to bit_idxth bit
    assert(l3_word != 0);

    // find the index of next best price at level 1
    const std::size_t level3_word_idx = kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l3_word));
    // is it a valid index?
    assert(level3_word_idx < 64);

    // find the bit in level 2
    const std::uint64_t l2_word_ = levels_[1024 + level3_word_idx];
    assert(l2_word_ != 0);
    const std::size_t level2_word_idx = level3_word_idx * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l2_word_)));
    // is it a valid index?
    assert(level2_word_idx < 16*64);

    // find the bit in level 1
    const std::uint64_t l1_word_ = levels_[level2_word_idx];
    assert(l1_word_ != 0);
    const std::size_t found = level2_word_idx * kWordBits + (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l1_word_)));
    // is it a valid index?
    assert(found < 1024*64);

    return found;
}


}   // namespace matching
