#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
public:
    static_assert(Capacity >= 2);

    bool push(const T& value) {
        const std::size_t head = head_.load();
        const std::size_t next_ = increment(head);

        if (next_ == tail_.load())     return false;   // full

        buffer_[head_] = value;
        head_.store(next_);
        return true;
    }


    std::optional<T> pop() {

        const std::size_t tail = tail_.load();

        if (head_.load() == tail)     return std::nullopt;   // empty

        const T value = buffer_[tail];

        tail_.store(increment(tail));

        return value;
    }
    
    bool empty() const { return tail_ == head_; }

    bool full() const { return increment(head_) == tail_; }

private:
    static constexpr std::size_t increment(std::size_t index) {
        return (index + 1) % Capacity;
    }


    std::array<T, Capacity> buffer_{};
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};