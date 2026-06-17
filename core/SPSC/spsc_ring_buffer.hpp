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

struct alignas(k_cache_line_size) PaddedAtomicSize {
    std::atomic<std::size_t> value{0};
};

struct alignas(k_cache_line_size) PaddedSize {
    std::size_t value = 0;
};

// ---------------------------------------------------------------------------
// Step 0: mutex baseline
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
class SpscRingBufferMutex {
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

// ---------------------------------------------------------------------------
// Step 1: lock-free atomics, seq_cst, no cache-line padding
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
class SpscRingBufferAtomicV1 {
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

// ---------------------------------------------------------------------------
// Step 2: relaxed / acquire / release + cache-line padding, no local cache
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
class SpscRingBufferAtomicV2 {
public:
    static_assert(Capacity >= 2);
    static_assert(is_power_of_two(Capacity));

    bool push(const T& value) {
        const std::size_t head = head_.value.load(std::memory_order_relaxed);
        const std::size_t next = increment(head);

        if (next == tail_.value.load(std::memory_order_acquire)) {
            return false;
        }

        buffer_[head] = value;
        head_.value.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::size_t tail = tail_.value.load(std::memory_order_relaxed);
        if (tail == head_.value.load(std::memory_order_acquire)) {
            return false;
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
};

// ---------------------------------------------------------------------------
// Step 3: cached opponent head/tail (modulo indices)
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
class SpscRingBufferAtomicV3 {
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

// ---------------------------------------------------------------------------
// Step 4: cached local monotonic head/tail counters
// ---------------------------------------------------------------------------

template <typename T, std::size_t Capacity>
class SpscRingBufferAtomicV4 {
public:
    static_assert(Capacity >= 2);
    static_assert(is_power_of_two(Capacity));

    bool push(const T& value) {
        if (producer_head_.value - producer_tail_.value == Capacity) {
            producer_tail_.value = tail_.value.load(std::memory_order_acquire);

            if (producer_head_.value - producer_tail_.value == Capacity) {
                return false;
            }
        }

        buffer_[idx_of(producer_head_.value)] = value;
        ++producer_head_.value;

        head_.value.store(producer_head_.value, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        if (consumer_head_.value == consumer_tail_.value) {
            consumer_head_.value = head_.value.load(std::memory_order_acquire);

            if (consumer_head_.value == consumer_tail_.value) {
                return false;
            }
        }

        out = buffer_[idx_of(consumer_tail_.value)];
        ++consumer_tail_.value;

        tail_.value.store(consumer_tail_.value, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t idx_of(std::size_t counter) noexcept {
        return counter & (Capacity - 1);
    }

    std::array<T, Capacity> buffer_{};

    PaddedAtomicSize head_;
    PaddedAtomicSize tail_;

    PaddedSize producer_head_;
    PaddedSize producer_tail_;

    PaddedSize consumer_head_;
    PaddedSize consumer_tail_;
};

} // namespace llmes::spsc
