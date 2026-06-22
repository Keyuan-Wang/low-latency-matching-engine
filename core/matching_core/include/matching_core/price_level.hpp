#pragma once

#include "types.hpp"

namespace llmes::matching_core {

class PriceLevel {
private:
    Order* head_ = nullptr;
    Order* tail_ = nullptr;

public:
    PriceLevel() = default;

    // Move constructor
    PriceLevel(PriceLevel&& other) noexcept
        : head_(other.head_),
          tail_(other.tail_)
    {
        other.head_ = nullptr;
        other.tail_ = nullptr;
    }

    // Move operator
    PriceLevel& operator=(PriceLevel&& other) noexcept {
        if (this != &other) {
            head_ = other.head_;
            tail_ = other.tail_;

            other.head_ = nullptr;
            other.tail_ = nullptr;
        }

        return *this;
    }

    // Prevent copy constructor (two pointers pointing to the same order pool)
    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    [[gnu::always_inline]] void push_back(Order& o) {
        o.prev = tail_;
        o.next = nullptr;

        if (tail_)      tail_->next = &o;
        else            head_ = &o;
        tail_ = &o;
    }

    [[gnu::always_inline]] void erase(Order& o) {
        if (o.prev) o.prev->next = o.next;
        else        head_ = o.next;
        if (o.next) o.next->prev = o.prev;
        else        tail_ = o.prev;
        
        o.prev = o.next = nullptr;
    }

    /** @brief Clear FIFO links after @ref PriceLevelPool::release (level must be empty). */
    [[gnu::always_inline]] void reset() {
        head_ = nullptr;
        tail_ = nullptr;
    }

    [[nodiscard]] [[gnu::always_inline]] bool empty() const { return head_ == nullptr; }

    [[nodiscard]] [[gnu::always_inline]] Order& front() const { return *head_; }

    [[gnu::always_inline]] const Order* begin() const { return head_; };
    [[gnu::always_inline]] Order* begin() { return head_; };
};

}
