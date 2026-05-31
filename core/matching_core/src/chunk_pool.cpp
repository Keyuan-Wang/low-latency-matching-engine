#include "matching/chunk_pool.hpp"

#include <cassert>
#include <cstddef>
#include <memory>


namespace matching {

// --- Chunk ---

Chunk::Chunk() noexcept {
    // Build the initial free-slot stack inside this chunk. The order does
    // not matter for allocation; each pop returns one cache-local slot.
    Slot* next = nullptr;
    for (auto& slot : slots_) {
        slot.next_free = next;
        free_head_ = &slot;

        next = &slot;
    }

    free_count_ = kChunkSize;
}

Order* Chunk::allocate_order() noexcept {
    if (full())     return nullptr;

    // Pop one slot from the chunk-local free stack. Order::prev/next are not
    // involved here; they are reserved for the live FIFO queue in OrderLevel.
    Slot* free_slot = free_head_;
    free_head_ = free_slot->next_free;

    // A live slot must not remain linked through the allocator free stack.
    free_slot->next_free = nullptr;

    --free_count_;

    return &free_slot->order;
}


void Chunk::release_order(Order* order) noexcept {
    assert(order != nullptr);
    assert(free_count_ < kChunkSize);

    order->clear();

    // The Order pointer is enough to recover the Slot because slots_ is a
    // fixed contiguous array and Order is embedded at a fixed offset in Slot.
    Slot* slot = slot_from_order(order);

    // Push the released slot back to the front of the chunk-local free stack.
    slot->next_free = free_head_;
    free_head_ = slot;

    ++free_count_;
}


void Chunk::link_available(Chunk*& head) noexcept {
    // This operation is scoped to a single OrderLevel. The caller passes that
    // level's available_head_, and this chunk becomes the new head of that
    // level-local list. ChunkPool does not participate in this list.
    //
    // Intended use: when release_order() changes a chunk from full to
    // partially free, OrderLevel links it here so future allocations can find
    // it in O(1).

    assert(!full());

    // A chunk must not be linked into two available lists at the same time.
    assert(prev_available_ == nullptr); 
    assert(next_available_ == nullptr);
    
    // Insert at the head so OrderLevel can get an allocatable chunk in O(1).
    next_available_ = head;

    if (head != nullptr) {
        head->prev_available_ = this;
    }

    head = this;
}

void Chunk::unlink_available(Chunk*& head) noexcept {
    // This removes the chunk from the same OrderLevel-local available list
    // that link_available() inserted it into. It must support removing either
    // the head or a middle node:
    // - allocate_order() can make the head chunk full, so OrderLevel unlinks it.
    // - release_order() can make any partially-free chunk empty, so OrderLevel
    //   unlinks it before returning it to ChunkPool.
    //
    // This function intentionally does not touch next_free_pool_; global empty
    // chunk ownership is handled only by ChunkPool.

    // Remove this chunk from its OrderLevel's available list. This must handle
    // both cases: allocate() removes the head when it becomes full, while
    // remove() may release an arbitrary middle chunk when it becomes empty.
    if (prev_available_ != nullptr) {
        prev_available_->next_available_ = next_available_;
    } else {
        assert(head == this);
        head = next_available_;
    }

    if (next_available_ != nullptr) {
        next_available_->prev_available_ = prev_available_;
    }

    prev_available_ = nullptr;
    next_available_ = nullptr;
}


Chunk::Slot* Chunk::slot_from_order(const Order* order) noexcept {
    const auto* base = reinterpret_cast<const std::byte*>(slots_.data());
    const auto* p = reinterpret_cast<const std::byte*>(order);
    const auto* limit = reinterpret_cast<const std::byte*>(slots_.data() + kChunkSize);

    // The caller must pass an Order stored inside this chunk. These asserts
    // catch stale pointers and orders from another pool in debug builds.
    assert(p >= base);
    assert(p < limit);

    // Convert byte offset to Slot index. This avoids storing an owner pointer
    // in every Order while keeping lookup O(1).
    const auto offset = static_cast<std::size_t>(p - base);
    const auto slot_index = offset / sizeof(Slot);

    assert(slot_index < kChunkSize);

    Slot* slot = &slots_[slot_index];
    assert(&slot->order == order);

    return slot;
}


// --- ChunkPool ---

ChunkPool::ChunkPool(std::size_t order_capacity)
    : chunk_count_(chunk_count_for(order_capacity))
    , chunks_(std::make_unique<Chunk[]>(chunk_count_)) {
    
    // All chunks start empty, so wire the entire fixed array into the global
    // free-chunk stack. The chunks_ array is never resized after this point.
    for (std::size_t i = 0; i < chunk_count_; ++i) {
        Chunk& chunk = chunks_[i];

        assert(chunk.empty());

        chunk.next_free_pool_ = free_chunk_head_;
        free_chunk_head_ = &chunk;
    }
}


Chunk* ChunkPool::acquire_empty_chunk() noexcept {
    Chunk* chunk = free_chunk_head_;

    // check if chunk pool is full
    assert(chunk != nullptr);

    // Pop one completely empty chunk from the global free-chunk stack.
    free_chunk_head_ = chunk->next_free_pool_;
    chunk->next_free_pool_ = nullptr;

    assert(chunk->empty());

    return chunk;
}

void ChunkPool::release_empty_chunk(Chunk* chunk) noexcept {
    assert(chunk != nullptr);
    assert(chunk->empty());

    // The caller should already have removed this chunk from the level's
    // available list. Clearing these links here prevents stale linkage from
    // leaking into the global free-chunk stack.
    chunk->prev_available_ = nullptr;
    chunk->next_available_ = nullptr;

    // Push the empty chunk back onto the global free-chunk stack.
    chunk->next_free_pool_ = free_chunk_head_;
    free_chunk_head_ = chunk;
}


Chunk* ChunkPool::chunk_from_order(Order* order) noexcept {
    assert(order != nullptr);

    // chunks_ is one fixed contiguous Chunk[] allocation, so an Order address
    // can be mapped back to its containing Chunk by byte offset.
    const auto* base = reinterpret_cast<const std::byte*>(chunks_.get());
    const auto* limit = reinterpret_cast<const std::byte*>(chunks_.get() + chunk_count_);
    const auto* p = reinterpret_cast<const std::byte*>(order);

    assert(p >= base);
    assert(p < limit);

    const auto offset = static_cast<std::size_t>(p - base);
    const auto chunk_index = offset / sizeof(Chunk);

    assert(chunk_index < chunk_count_);

    Chunk* chunk = chunks_.get() + chunk_index;

    return chunk;
}

}   // namespace matching
