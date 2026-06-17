// Unit tests for the free-list MemoryPool allocator.

#include "obe/MemoryPool.hpp"
#include "obe/Order.hpp"

#include <gtest/gtest.h>
#include <unordered_set>
#include <vector>

using namespace obe;

TEST(MemoryPool, AcquireConstructsWithArgs) {
    MemoryPool<Order> pool(8);
    Order* o = pool.acquire(OrderId{42}, TraderId{3}, Side::Sell, Price{100},
                            Quantity{7});
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->id(), 42u);
    EXPECT_EQ(o->side(), Side::Sell);
    EXPECT_EQ(o->remaining(), 7u);
    EXPECT_EQ(pool.live_count(), 1u);
    pool.release(o);
    EXPECT_EQ(pool.live_count(), 0u);
}

TEST(MemoryPool, ReleasedSlotIsReused) {
    MemoryPool<Order> pool(8);
    Order* a = pool.acquire();
    pool.release(a);
    Order* b = pool.acquire();
    // Free-list is LIFO, so the just-released slot comes straight back.
    EXPECT_EQ(a, b);
    pool.release(b);
}

TEST(MemoryPool, GrowsBeyondInitialCapacity) {
    MemoryPool<Order> pool(4); // small to force growth
    std::vector<Order*> held;
    for (int i = 0; i < 1000; ++i) {
        held.push_back(pool.acquire());
    }
    EXPECT_EQ(pool.live_count(), 1000u);
    EXPECT_GE(pool.capacity(), 1000u);
    EXPECT_GT(pool.block_count(), 1u); // it had to grow

    // All live pointers must be distinct (no slot handed out twice).
    std::unordered_set<Order*> unique(held.begin(), held.end());
    EXPECT_EQ(unique.size(), held.size());

    for (Order* o : held) {
        pool.release(o);
    }
    EXPECT_EQ(pool.live_count(), 0u);
}

TEST(MemoryPool, ReleaseNullIsSafe) {
    MemoryPool<Order> pool(2);
    pool.release(nullptr); // must be a no-op
    EXPECT_EQ(pool.live_count(), 0u);
}
