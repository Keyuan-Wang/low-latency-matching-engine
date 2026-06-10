#pragma once

#include "types.hpp"

namespace matching {

class PriceLevel {
private:
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    std::size_t size_ = 0;

    Side book_side_;
    std::size_t idx_;

public:
    PriceLevel() = default;

    // Prevent copy constructor (two pointers pointing to the same order pool)
    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    [[gnu::always_inline]] void push_back(Order& o) {
        o.prev = tail_;
        o.next = nullptr;

        if (tail_)      tail_->next = &o;
        else            head_ = &o;
        tail_ = &o;

        ++size_;
    }


    [[gnu::always_inline]] Side side() const noexcept { return book_side_; }

    [[gnu::always_inline]] std::size_t idx() const noexcept { return idx_; }


    [[gnu::always_inline]] void erase(Order& o) {
        if (o.prev) o.prev->next = o.next;
        else        head_ = o.next;
        if (o.next) o.next->prev = o.prev;
        else        tail_ = o.prev;
        
        o.prev = o.next = nullptr;
        --size_;
    }

    /** @brief Clear FIFO links after @ref PriceLevelPool::release (level must be empty). */
    [[gnu::always_inline]] void reset() {
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0;
    }

    [[nodiscard]] [[gnu::always_inline]] bool empty() const { return head_ == nullptr; }

    [[nodiscard]] [[gnu::always_inline]] Order& front() const { return *head_; }
    
    [[nodiscard]] [[gnu::always_inline]] std::size_t size() const { return size_; }

    [[gnu::always_inline]] const Order* begin() const { return head_; };
    [[gnu::always_inline]] Order* begin() { return head_; };

    void bind_owner(Side side, std::size_t idx) noexcept { book_side_ = side; idx_ = idx; }
};

}
