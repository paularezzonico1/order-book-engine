// Unit + randomized-oracle tests for the open-addressing FlatHashMap. The
// oracle test is the important one: it hammers insert/erase/find against a
// std::unordered_map reference, which exercises the backward-shift deletion
// (the part most likely to harbour a bug).

#include "obe/FlatHashMap.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <random>
#include <unordered_map>

using namespace obe;

TEST(FlatHashMap, BasicInsertFindErase) {
    FlatHashMap<std::uint64_t, int> m;
    EXPECT_TRUE(m.empty());
    m.insert(10, 100);
    m.insert(20, 200);
    ASSERT_NE(m.find(10), nullptr);
    EXPECT_EQ(*m.find(10), 100);
    EXPECT_EQ(*m.find(20), 200);
    EXPECT_EQ(m.find(30), nullptr);
    EXPECT_EQ(m.size(), 2u);

    EXPECT_TRUE(m.erase(10));
    EXPECT_FALSE(m.erase(10));
    EXPECT_EQ(m.find(10), nullptr);
    EXPECT_EQ(*m.find(20), 200); // survivor still findable
    EXPECT_EQ(m.size(), 1u);
}

TEST(FlatHashMap, InsertOverwrites) {
    FlatHashMap<std::uint64_t, int> m;
    m.insert(5, 1);
    m.insert(5, 2);
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(*m.find(5), 2);
}

TEST(FlatHashMap, KeyZeroIsValid) {
    // No reserved sentinel key: 0 must behave like any other key.
    FlatHashMap<std::uint64_t, int> m;
    m.insert(0, 42);
    ASSERT_NE(m.find(0), nullptr);
    EXPECT_EQ(*m.find(0), 42);
    EXPECT_TRUE(m.erase(0));
    EXPECT_EQ(m.find(0), nullptr);
}

TEST(FlatHashMap, GrowsAndKeepsAllEntries) {
    FlatHashMap<std::uint64_t, int> m(8);
    for (int i = 0; i < 10000; ++i) {
        m.insert(static_cast<std::uint64_t>(i) * 7 + 1, i);
    }
    EXPECT_EQ(m.size(), 10000u);
    EXPECT_GT(m.capacity(), 10000u);
    for (int i = 0; i < 10000; ++i) {
        auto* v = m.find(static_cast<std::uint64_t>(i) * 7 + 1);
        ASSERT_NE(v, nullptr);
        EXPECT_EQ(*v, i);
    }
}

TEST(FlatHashMap, RandomizedOracleAgainstUnorderedMap) {
    std::mt19937_64 rng(0xC0FFEE);
    std::uniform_int_distribution<std::uint64_t> key_dist(0, 5000);
    std::uniform_int_distribution<int> op_dist(0, 2);

    FlatHashMap<std::uint64_t, std::uint64_t> flat;
    std::unordered_map<std::uint64_t, std::uint64_t> oracle;

    for (int step = 0; step < 200000; ++step) {
        const std::uint64_t k = key_dist(rng);
        switch (op_dist(rng)) {
            case 0: { // insert/update
                const std::uint64_t v = rng();
                flat.insert(k, v);
                oracle[k] = v;
                break;
            }
            case 1: { // erase
                EXPECT_EQ(flat.erase(k), oracle.erase(k) == 1);
                break;
            }
            case 2: { // find
                auto* fv = flat.find(k);
                auto oit = oracle.find(k);
                if (oit == oracle.end()) {
                    EXPECT_EQ(fv, nullptr);
                } else {
                    ASSERT_NE(fv, nullptr);
                    EXPECT_EQ(*fv, oit->second);
                }
                break;
            }
        }
        ASSERT_EQ(flat.size(), oracle.size()) << "diverged at step " << step;
    }

    // Final full reconciliation.
    for (const auto& [k, v] : oracle) {
        auto* fv = flat.find(k);
        ASSERT_NE(fv, nullptr);
        EXPECT_EQ(*fv, v);
    }
}
