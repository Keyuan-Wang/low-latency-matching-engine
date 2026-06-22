#include "matching_core/occupancy_tree.hpp"

#include <bit>


namespace llmes::matching_core {

// Layout: levels_[0..63] = L1 (64 words, one bit per price index)
//         levels_[64]    = L2 (1 word, one bit per L1 word)

static constexpr std::size_t kL1Count = OccupancyTree::kBitCount / 64;  // 64
static constexpr std::size_t kL2Offset = kL1Count;                       // 64

OccupancyTree::OccupancyTree() {
    levels_.fill(0);
}


// -------- loop unrolling (2 levels) --------
void OccupancyTree::set(std::size_t bit) noexcept {
    assert(bit < kBitCount);

    // level 1: levels_[0:64]
    const std::size_t word_idx = bit / kWordBits;       // which word?
    const std::uint64_t mask = static_cast<std::uint64_t>(1) << (bit & (kWordBits - 1));  // which bit?

    std::uint64_t& l1_word = levels_[word_idx];
    const bool was_empty = l1_word == 0;
    l1_word |= mask;                                    // set word[bit_idx] to 1

    if (!was_empty) [[likely]] return;                   // the upper level word is already 1

    // level 2: levels_[64] (single word)
    levels_[kL2Offset] |= static_cast<std::uint64_t>(1) << word_idx;
}


void OccupancyTree::clear(std::size_t bit) noexcept {
    assert(bit < kBitCount);

    // level 1: levels_[0:64]
    const std::size_t word_idx = bit / kWordBits;       // which word?
    const std::uint64_t mask = static_cast<std::uint64_t>(1) << (bit & (kWordBits - 1));  // which bit?

    std::uint64_t& l1_word = levels_[word_idx];
    assert((l1_word & mask) != 0 && "clear() requires a currently set bit");

    l1_word &= ~mask;                                   // clear word[bit_idx] to 0

    if (l1_word != 0) [[likely]] return;                 // the upper level word is still non-zero

    // level 2: levels_[64] (single word)
    levels_[kL2Offset] &= ~(static_cast<std::uint64_t>(1) << word_idx);
}


// -------- recursion unrolling (2 levels) --------
std::size_t OccupancyTree::find_next_set(std::size_t bit_pos) const noexcept {
    assert(bit_pos < kBitCount);

    const std::size_t l1_word_idx = bit_pos / kWordBits;    // which word?
    const std::size_t l1_bit_idx = bit_pos & (kWordBits - 1);  // which bit?

    // mask from bit_idx-th bit to 63rd bit
    const std::uint64_t l1_word = levels_[l1_word_idx] & mask_high(l1_bit_idx);

    if (l1_word != 0) [[likely]] {
        // found the next best price in current word
        // for benchmark case, this is likely to happen as best price shifts slowly
        const std::size_t found = l1_word_idx * kWordBits +
            static_cast<std::size_t>(std::countr_zero(l1_word));
        assert(found < kBitCount);
        return found;
    }

    // not found in current L1 word, find the next non-empty L1 word via L2
    assert(l1_word_idx + 1 < kL1Count);

    // find the bit in level 2 (single word)
    const std::uint64_t l2_word = levels_[kL2Offset] & mask_high(l1_word_idx + 1);
    assert(l2_word != 0);                                   // whole book empty, this will never happen in the benchmark case

    // find the index of next non-empty L1 word
    const std::size_t target_l1_idx = static_cast<std::size_t>(std::countr_zero(l2_word));
    assert(target_l1_idx < kL1Count);                       // is it a valid index?

    // find the index of next best price in target L1 word
    const std::uint64_t target_l1_word = levels_[target_l1_idx];
    assert(target_l1_word != 0);

    const std::size_t found = target_l1_idx * kWordBits +
        static_cast<std::size_t>(std::countr_zero(target_l1_word));
    assert(found < kBitCount);
    return found;
}


std::size_t OccupancyTree::find_prev_set(std::size_t bit_pos) const noexcept {
    assert(bit_pos < kBitCount);

    const std::size_t l1_word_idx = bit_pos / kWordBits;    // which word?
    const std::size_t l1_bit_idx = bit_pos & (kWordBits - 1);  // which bit?

    // mask from 0th bit to bit_idx-th bit
    const std::uint64_t l1_word = levels_[l1_word_idx] & mask_low(l1_bit_idx);

    if (l1_word != 0) [[likely]] {
        // found the prev best price in current word
        // for benchmark case, this is likely to happen as best price shifts slowly
        const std::size_t found = l1_word_idx * kWordBits +
            (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(l1_word)));
        assert(found < kBitCount);
        return found;
    }

    // not found in current L1 word, find the prev non-empty L1 word via L2
    assert(l1_word_idx > 0);

    // find the bit in level 2 (single word)
    const std::uint64_t l2_word = levels_[kL2Offset] & mask_low(l1_word_idx - 1);
    assert(l2_word != 0);                                   // whole book empty, this will never happen in the benchmark case

    // find the index of prev non-empty L1 word
    const std::size_t target_l1_idx = kWordBits - 1 -
        static_cast<std::size_t>(std::countl_zero(l2_word));
    assert(target_l1_idx < kL1Count);                       // is it a valid index?

    // find the index of prev best price in target L1 word
    const std::uint64_t target_l1_word = levels_[target_l1_idx];
    assert(target_l1_word != 0);

    const std::size_t found = target_l1_idx * kWordBits +
        (kWordBits - 1 - static_cast<std::size_t>(std::countl_zero(target_l1_word)));
    assert(found < kBitCount);
    return found;
}


}   // namespace llmes::matching_core
