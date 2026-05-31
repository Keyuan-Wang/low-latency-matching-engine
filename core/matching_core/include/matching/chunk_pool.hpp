#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>


#include "matching/types.hpp"

#ifndef LLMES_ORDER_CHUNK_SIZE
#define LLMES_ORDER_CHUNK_SIZE 256
#endif

namespace matching {


class Chunk {
public:
    static constexpr std::size_t kChunkSize = static_cast<std::size_t>(LLMES_ORDER_CHUNK_SIZE);
    static_assert(kChunkSize > 0, "LLMES_ORDER_CHUNK_SIZE must be positive");
    static_assert(kChunkSize <= 65535, "LLMES_ORDER_CHUNK_SIZE must fit in uint16_t");
    
    explicit Chunk() noexcept;

    Chunk(const Chunk&) = delete;
    Chunk& operator=(const Chunk&) = delete;
    Chunk(Chunk&&) = delete;
    Chunk& operator=(Chunk&&) = delete;

    [[nodiscard]] Order* allocate_order() noexcept;
    void release_order(Order* order) noexcept;

    // Link/unlink this chunk in one OrderLevel's available-chunk list.
    // Scope: these functions are only for the per-PriceLevel list of chunks that
    // can still allocate at least one order. They must not be used for the
    // ChunkPool global empty-chunk list, which is managed by ChunkPool.
    // Typical callers:
    // - link after a full chunk receives a freed slot.
    // - unlink after an allocation makes this chunk full.
    // - unlink before an empty chunk is returned to ChunkPool.
    void link_available(Chunk*& head) noexcept;
    void unlink_available(Chunk*& head) noexcept;

    [[nodiscard]] inline bool full() const noexcept { return free_count_ == 0; };
    [[nodiscard]] inline bool empty() const noexcept { return free_count_ == kChunkSize; };

private:
    // A slot is the allocation unit inside a chunk. The Order links
    // remain reserved for the active FIFO queue; next_free is used only
    // while this slot is not holding a live order.
    struct Slot {
        Order order;
        Slot* next_free = nullptr;
    };

    // Slots are contiguous within the chunk. This is the locality boundary
    // we want each hot price level to reuse.
    std::array<Slot, kChunkSize> slots_;

    // Head of the chunk-local free slot stack and the number of free slots.
    // These fields let the chunk answer full()/empty() in O(1).
    Slot* free_head_ = nullptr;
    std::uint16_t free_count_ = kChunkSize;

    // Links used only by an OrderLevel's available-chunk list. A chunk is in
    // that list exactly when it has at least one free slot and is not empty.
    Chunk* prev_available_ = nullptr;
    Chunk* next_available_ = nullptr;

    // Link used only by ChunkPool's global empty-chunk free list.
    Chunk* next_free_pool_ = nullptr;

    // Recover the containing Slot from an Order pointer known to belong to
    // this chunk. This keeps allocator metadata out of Order itself.
    [[nodiscard]] Slot* slot_from_order(const Order* order) noexcept;

    // ChunkPool owns the backing array and is allowed to wire chunks into
    // the global empty-chunk list.
    friend class ChunkPool;
};


class ChunkPool {
public :
    explicit ChunkPool(std::size_t order_capacity);

    [[nodiscard]] Chunk* acquire_empty_chunk() noexcept;
    void release_empty_chunk(Chunk* chunk) noexcept;

    [[nodiscard]] Chunk* chunk_from_order(Order* order) noexcept;

    // delete all copy and move constructors to prevent memory leak
    ChunkPool(const ChunkPool&) = delete;
    ChunkPool& operator=(const ChunkPool&) = delete;

    ChunkPool(ChunkPool&&) = delete;
    ChunkPool& operator=(ChunkPool&&) = delete;

private:
    inline static std::size_t chunk_count_for(std::size_t order_capacity) noexcept {
        return (order_capacity + Chunk::kChunkSize - 1) / Chunk::kChunkSize;
    };

    // Fixed-size backing storage. The array is allocated once in the
    // constructor and never resized, so Chunk* and Order* remain stable.
    std::size_t chunk_count_ = 0;
    std::unique_ptr<Chunk[]> chunks_;

    // Head of the global stack of completely empty chunks.
    Chunk* free_chunk_head_ = nullptr;
};

}   // namespace matching
