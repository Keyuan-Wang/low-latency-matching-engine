#include "spsc_ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <immintrin.h>
#endif

namespace {

constexpr std::size_t k_capacity = 1024;

template <typename Queue>
void run_benchmark(const char* name, std::uint64_t total) {
    Queue queue;

    std::atomic<bool> start{false};
    std::uint64_t checksum = 0;

    std::thread producer([&] {
        while (!start.load(std::memory_order_acquire)) {
            _mm_pause();
        }

        for (std::uint64_t value = 1; value <= total; ++value) {
            while (!queue.push(value)) {
                _mm_pause();
            }
        }
    });

    std::thread consumer([&] {
        while (!start.load(std::memory_order_acquire)) {
            _mm_pause();
        }

        for (std::uint64_t count = 0; count < total;) {
            std::uint64_t value = 0;

            if (queue.pop(value)) {
                checksum += value;
                ++count;
            } else {
                _mm_pause();
            }
        }
    });

    const auto begin = std::chrono::steady_clock::now();
    start.store(true, std::memory_order_release);

    producer.join();
    consumer.join();

    const auto end = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = end - begin;

    const std::uint64_t expected = total * (total + 1) / 2;
    const double seconds = elapsed.count();
    const double ns_per_message = seconds * 1'000'000'000.0 / total;
    const double million_messages_per_second =
        static_cast<double>(total) / seconds / 1'000'000.0;

    std::cout << name
              << " total=" << total
              << " seconds=" << seconds
              << " ns/msg=" << ns_per_message
              << " Mmsg/s=" << million_messages_per_second
              << " checksum=" << (checksum == expected ? "ok" : "bad")
              << '\n';
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc >= 2 ? argv[1] : "all";
    const std::uint64_t total =
        argc >= 3 ? std::strtoull(argv[2], nullptr, 10) : 20'000'000ULL;

    using MutexQueue =
        llmes::spsc::SpscRingBufferMutex<std::uint64_t, k_capacity>;
    using AtomicV1Queue =
        llmes::spsc::SpscRingBufferAtomicV1<std::uint64_t, k_capacity>;
    using AtomicV2Queue =
        llmes::spsc::SpscRingBufferAtomicV2<std::uint64_t, k_capacity>;
    using AtomicV3Queue =
        llmes::spsc::SpscRingBufferAtomicV3<std::uint64_t, k_capacity>;
    using AtomicV4Queue =
        llmes::spsc::SpscRingBufferAtomicV4<std::uint64_t, k_capacity>;

    if (mode == "mutex" || mode == "all") {
        run_benchmark<MutexQueue>("mutex", total);
    }

    if (mode == "atomicv1" || mode == "all") {
        run_benchmark<AtomicV1Queue>("atomic_v1", total);
    }

    if (mode == "atomicv2" || mode == "all") {
        run_benchmark<AtomicV2Queue>("atomic_v2", total);
    }

    if (mode == "atomicv3" || mode == "all") {
        run_benchmark<AtomicV3Queue>("atomic_v3", total);
    }

    if (mode == "atomicv4" || mode == "all") {
        run_benchmark<AtomicV4Queue>("atomic_v4", total);
    }

    return 0;
}
