// Unit tests for the lock-free SPSC ring buffer, including a real two-thread
// producer/consumer run.

#include "obe/RingBuffer.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <thread>
#include <vector>

using namespace obe;

TEST(RingBuffer, FifoOrder) {
    SpscRingBuffer<int> q(8);
    EXPECT_TRUE(q.empty());
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(q.push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int out = -1;
        EXPECT_TRUE(q.pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(q.empty());
}

TEST(RingBuffer, RejectsPushWhenFull) {
    SpscRingBuffer<int> q(4); // capacity rounds to 4
    int pushed = 0;
    while (q.push(pushed)) {
        ++pushed;
    }
    EXPECT_EQ(static_cast<std::size_t>(pushed), q.capacity());
    int out = 0;
    EXPECT_FALSE(q.push(999)); // full
    EXPECT_TRUE(q.pop(out));   // make room
    EXPECT_TRUE(q.push(999));  // now fits
}

TEST(RingBuffer, PopOnEmptyReturnsFalse) {
    SpscRingBuffer<int> q(4);
    int out = 0;
    EXPECT_FALSE(q.pop(out));
}

TEST(RingBuffer, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(SpscRingBuffer<int>(5).capacity(), 8u);
    EXPECT_EQ(SpscRingBuffer<int>(16).capacity(), 16u);
    EXPECT_EQ(SpscRingBuffer<int>(1).capacity(), 2u);
}

TEST(RingBuffer, ConcurrentProducerConsumer) {
    constexpr std::uint64_t N = 1'000'000;
    SpscRingBuffer<std::uint64_t> q(1024);

    std::uint64_t consumed = 0;
    std::uint64_t sum = 0;
    std::thread consumer([&] {
        std::uint64_t v;
        while (consumed < N) {
            if (q.pop(v)) {
                EXPECT_EQ(v, consumed); // strict FIFO across threads
                sum += v;
                ++consumed;
            }
        }
    });

    for (std::uint64_t i = 0; i < N; ++i) {
        while (!q.push(i)) {
            std::this_thread::yield();
        }
    }
    consumer.join();

    EXPECT_EQ(consumed, N);
    EXPECT_EQ(sum, N * (N - 1) / 2); // Gauss sum sanity check
}
