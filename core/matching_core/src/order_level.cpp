#include "matching/order_level.hpp"

#include <cassert>
#include <vector>


namespace matching {

OrderLevel::OrderLevel(OrderChunkPool* chunk_pool) : chunk_pool_(chunk_pool) {
    for (auto& chunk : *chunk_pool_) {
        o.next = free_head_;
        free_head_ = &o;
    }
}

OrderLevel::OrderLevel(OrderLevel&& other) noexcept
    : pool_(std::move(other.pool_))
    , free_head_(other.free_head_)
    , head_(other.head_)
    , tail_(other.tail_)
    , size_(other.size_)
{
    other.free_head_ = nullptr;
    other.head_ = nullptr;
    other.tail_ = nullptr;
    other.size_ = 0;
}

OrderLevel& OrderLevel::operator=(OrderLevel&& other) noexcept {
    if (this != &other) {
        pool_ = std::move(other.pool_);
        free_head_ = other.free_head_;
        head_ = other.head_;
        tail_ = other.tail_;
        size_ = other.size_;

        other.free_head_ = nullptr;
        other.head_ = nullptr;
        other.tail_ = nullptr;
        other.size_ = 0;
    }

    return *this;
}

[[nodiscard]] Order* OrderLevel::allocate() {
    if (!free_head_)    return nullptr;

    Order* result = free_head_;
    free_head_ = result->next;

    result->prev = nullptr;
    result->next = nullptr;

    return result;
}

void OrderLevel::push_back(Order& o) {
    o.prev = tail_;
    o.next = nullptr;

    if (tail_)      tail_->next = &o;
    else            head_ = &o;
    tail_ = &o;

    ++size_;
}

void OrderLevel::remove(Order& o) {
    // remove o from intrusive list
    if (o.prev) o.prev->next = o.next;
    else        head_ = o.next;
    if (o.next) o.next->prev = o.prev;
    else        tail_ = o.prev;
    
    o.prev = o.next = nullptr;
    --size_;

    // remove o from memory pool
    o.next = free_head_;
    free_head_ = &o;
}

void OrderLevel::clear() {
    assert(size_ == 0);
    pool_.clear();
    free_head_ = nullptr;
    head_ = nullptr;
    tail_ = nullptr;
    size_ = 0;
}

}