#pragma once

#include <array>

#include "matching/types.hpp"


namespace matching {

class OrderChunkPool {
public:
    static constexpr std::size_t kChunkSize = 256;

    struct Chunk {
        std::array<Order, kChunkSize> orders;
        Chunk* next = nullptr;
    };

    explicit OrderChunkPool(std::size_t order_capacity);

    // get new free chunk 
    Chunk* acquire() noexcept;
    // release chunk list hold by a price level
    void release_chain(Chunk* head) noexcept;
    
    // delete all copt constructors to prevent memory leak
    OrderChunkPool(const OrderChunkPool&) = delete;
    OrderChunkPool& operator=(const OrderChunkPool&) = delete;

private:
    std::vector<Chunk> chunks_;
    Chunk* free_head_ = nullptr;
};



class OrderLevel {
private:
    OrderChunkPool* chunk_pool_ = nullptr;
    OrderChunkPool::Chunk* chunks_ = nullptr;
    
    Order* free_head_ = nullptr;
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    std::size_t size_ = 0;

    // attach a new chunk to current chunk list
    void attach_chunk(OrderChunkPool::Chunk* chunk) noexcept;
    // realse the chunks hold by current OrderLevel
    void release_chunks() noexcept;

public:
    // --- Constructor ---
    explicit OrderLevel(OrderChunkPool* chunk_pool);
    // must explicityly specify the capacty of order level
    OrderLevel() = delete;

    // Move constructor
    OrderLevel(OrderLevel&& other) noexcept;
    // Move operator
    OrderLevel& operator=(OrderLevel&& other) noexcept;

    // Prevent copy constructor (two pointers pointing to the same order pool)
    OrderLevel(const OrderLevel&) = delete;
    OrderLevel& operator=(const OrderLevel&) = delete;



    [[nodiscard]] Order* allocate();          // Get an empty slot from the top of stack, if return nullptr, the pool is full

    void push_back(Order& o);

    void remove(Order& o);

    [[nodiscard]] bool empty() const { return head_ == nullptr; }

    [[nodiscard]] Order& front() const { return *head_; }
    
    [[nodiscard]] std::size_t size() const { return size_; }

    const Order* begin() const { return head_; };
    Order* begin() { return head_; };
};

}