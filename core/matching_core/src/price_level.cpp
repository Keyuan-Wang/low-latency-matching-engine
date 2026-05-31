#include "matching/price_level.hpp"

#include <cassert>


namespace matching {

PriceLevel::PriceLevel(ChunkPool* chunk_pool) noexcept
    : chunk_pool_(chunk_pool) {
    assert(chunk_pool_ != nullptr);
}


[[nodiscard]] Order* PriceLevel::allocate() noexcept {
    Chunk* chunk = available_head_;
    const bool need_new_chunk = (chunk == nullptr);

    // If this price level has no chunk with free slots, borrow a completely
    // empty chunk from the shared pool. ChunkPool owns the backing storage;
    // this level only keeps the chunk while it contains live orders.
    if (need_new_chunk) {
        chunk = chunk_pool_->acquire_empty_chunk();
        assert(chunk != nullptr);
    }

    // Chunk owns the slot free-list. PriceLevel only receives the Order that
    // will be inserted into this level's active FIFO queue by push_back().
    Order* order = chunk->allocate_order();
    assert(order != nullptr);

    order->parent_level = this;

    // A newly acquired chunk remains available unless this allocation filled
    // it. Existing available_head_ chunks are unlinked when they become full.
    if (need_new_chunk) {
        chunk->link_available(available_head_);
    }
    else if (chunk->full())
        chunk->unlink_available(available_head_);

    return order;
}


void PriceLevel::push_back(Order& order) noexcept {
    assert(order.parent_level == this);

    // Append to the active FIFO queue for this price. Matching consumes from
    // head_, so this preserves time priority among orders at the same price.
    order.prev = tail_;
    order.next = nullptr;

    if (tail_ != nullptr) 
        tail_->next = &order;
    else
        head_ = &order;

    tail_ = &order;
    ++size_;
}


void PriceLevel::remove(Order& order) noexcept {
    assert(order.parent_level == this);
    assert(size_ > 0);

    // First unlink the order from the live FIFO queue. The allocator free-list
    // is separate and is updated only after the owning chunk is recovered.
    if (order.prev != nullptr)
        order.prev->next = order.next;
    else
        head_ = order.next;

    if (order.next != nullptr)
        order.next->prev = order.prev;
    else
        tail_ = order.prev;

    --size_;

    Chunk* chunk = chunk_pool_->chunk_from_order(&order);
    assert(chunk != nullptr);

    // If the chunk was full, it is not currently in available_head_. Releasing
    // this order creates one free slot, so the chunk must be linked back unless
    // the release makes it completely empty and returns it to ChunkPool.
    const bool was_full = chunk->full();

    chunk->release_order(&order);

    // Empty chunks are not kept by PriceLevel. Before returning the chunk to
    // ChunkPool, remove it from this level's available list.
    if (chunk->empty())  {
        chunk->unlink_available(available_head_);
        chunk_pool_->release_empty_chunk(chunk);
    }
    else if (was_full)    // The chunk moved from full to partially free.
        chunk->link_available(available_head_);
}


}   // namespace matching
