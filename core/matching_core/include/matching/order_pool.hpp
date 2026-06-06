#pragma once

#include <cassert>
#include <cstddef>
#include <vector>

#include "types.hpp"

namespace matching {

class OrderPool {
private:
    std::vector<Order> pool_;   // Note: the pool_ should not be resize during running
    Order* free_head_ = nullptr;

public:
    explicit OrderPool(std::size_t capacity) : pool_(capacity) {
        for (auto& o : pool_) {
            o.next = free_head_;
            free_head_ = &o;
        }
    }

    [[nodiscard]] [[gnu::always_inline]] OrderHandle acquire() noexcept {  // Returns kInvalidHandle if the pool is full.
        if (!free_head_) return kInvalidHandle;

        Order* o = free_head_;
        free_head_ = o->next;

        o->prev = nullptr;
        o->next = nullptr;

        return static_cast<OrderHandle>(index_of(o));
    }
    [[gnu::always_inline]] void    release(Order* o) noexcept {    // return a freed slot back to top of stack
        o->next = free_head_;
        free_head_ = o;
    }

    [[nodiscard]] [[gnu::always_inline]] Order* resolve(OrderHandle h) noexcept {
        assert(h != kInvalidHandle);
        assert(static_cast<std::size_t>(h) < pool_.size());
        return &pool_[h];
    }

    [[nodiscard]] [[gnu::always_inline]] std::size_t capacity() const noexcept { return pool_.size(); }

    // return the index of order in the pool_
    [[nodiscard]] [[gnu::always_inline]]  std::size_t index_of(const Order* o) const noexcept { return o - pool_.data(); };
    // return the order pointer at pool_[idx]
    [[gnu::always_inline]]  Order* at(std::size_t idx) noexcept { return &pool_[idx]; };
};

}