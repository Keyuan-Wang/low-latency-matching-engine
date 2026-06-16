#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>


namespace llmes::spsc {

constexpr std::size_t k_cache_line_size = 64;

constexpr bool is_power_of_two(std::size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}




template <typename T, std::size_t Capacity>
class MutexRingBuffer {
public:
    static_assert(Capacity >= 2);
    static_assert(is_power_of_two(Capacity));

    bool push(const T& value) {
        std::lock_guard<std::mutex> lock(mutex_);

        const std::size_t next = increment(head_);
        if (next == tail_) {
            return false;
        }

        buffer_[head_] = value;
        head_ = next;
        return true;
    }

    bool pop(T& out) {
        std::lock_guard<std::mutex> lock(mutex_);

        if (tail_ == head_) {
            return false;
        }

        out = buffer_[tail_];
        tail_ = increment(tail_);
        return true;
    }

private:
    static constexpr std::size_t increment(std::size_t index) {
        return (index + 1) & (Capacity - 1);
    }

    std::array<T, Capacity> buffer_{};
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::mutex mutex_;
};







template <typename T, std::size_t Capacity>
class AtomicRingBuffer {
public:
    static_assert(Capacity >= 2);
    static_assert(is_power_of_two(Capacity));

    bool push(const T& value) {
        const std::size_t head = head_.load();
        const std::size_t next = increment(head);

        if (next == tail_.load()) {
            return false;
        }

        buffer_[head] = value;
        head_.store(next);
        return true;
    }

    bool pop(T& out) {
        const std::size_t tail = tail_.load();

        if (tail == head_.load()) {
            return false;
        }

        out = buffer_[tail];
        tail_.store(increment(tail));
        return true;
    }

private:
    static constexpr std::size_t increment(std::size_t index) {
        return (index + 1) & (Capacity - 1);
    }

    std::array<T, Capacity> buffer_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};







struct alignas(k_cache_line_size) PaddedAtomicSize {
    std::atomic<std::size_t> value{0};
};


struct alignas(k_cache_line_size) PaddedSize {
    std::size_t value = 0;
};

template <typename T, std::size_t Capacity>
class OptimizedAtomicRingBuffer {
public:
    static_assert(Capacity >= 2);
    static_assert(is_power_of_two(Capacity));

    bool push(const T& value) {
        const std::size_t head = head_.value.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);

        if (next == producer_tail_cache_.value) {
            producer_tail_cache_.value =
                tail_.value.load(std::memory_order_acquire);

            if (next == producer_tail_cache_.value) {
                return false;
            }
        }

        buffer_[head] = value;
        head_.value.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::size_t tail = tail_.value.load(std::memory_order_relaxed);

        if (tail == consumer_head_cache_.value) {
            consumer_head_cache_.value =
                head_.value.load(std::memory_order_acquire);

            if (tail == consumer_head_cache_.value) {
                return false;
            }
        }

        out = buffer_[tail];
        tail_.value.store(increment(tail), std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t increment(std::size_t index) {
        return (index + 1) & (Capacity - 1);
    }

    std::array<T, Capacity> buffer_{};

    PaddedAtomicSize head_;
    PaddedAtomicSize tail_;

    PaddedSize producer_tail_cache_;
    PaddedSize consumer_head_cache_;
};

} // namespace llmes::spsc