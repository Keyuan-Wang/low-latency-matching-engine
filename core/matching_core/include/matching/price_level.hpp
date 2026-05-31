#pragma once

#include "matching/types.hpp"
#include "matching/chunk_pool.hpp"


namespace matching {

// FIFO order queue for one price. PriceLevel owns no raw memory itself; it
// borrows chunks from ChunkPool and returns a chunk as soon as all slots in
// that chunk become free.
class PriceLevel {
public:
    explicit PriceLevel(ChunkPool* chunk_pool) noexcept;
    ~PriceLevel() = default;

    PriceLevel() = delete;

    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    PriceLevel(PriceLevel&&) = delete;
    PriceLevel& operator=(PriceLevel&&) = delete;

    [[nodiscard]] Order* allocate() noexcept;

    void push_back(Order& order) noexcept;
    void remove(Order& order) noexcept;

    [[nodiscard]] bool empty() const noexcept { return head_ == nullptr; }
    [[nodiscard]] Order& front() const noexcept { return *head_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

    [[nodiscard]] const Order* begin() const noexcept { return head_; }
    [[nodiscard]] Order* begin() noexcept { return head_; }

private:
    // Shared fixed-size chunk pool owned by OrderBook.
    ChunkPool* chunk_pool_ = nullptr;

    // Head of this level's local list of chunks that still have free slots.
    // Full chunks are not in this list; empty chunks are immediately returned
    // to ChunkPool and are not in this list either.
    Chunk* available_head_ = nullptr;

    // Active live orders at this price, in price-time priority order.
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    std::size_t size_ = 0;
};

}   // namespace matching
