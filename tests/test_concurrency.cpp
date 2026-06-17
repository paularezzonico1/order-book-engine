// Concurrency tests for the MatchingEngine pipeline. Run these under
// ThreadSanitizer (cmake -DOBE_TSAN=ON) to verify the lock-free hand-off, the
// atomic stats counters, and the shutdown handshake are race-free.

#include "obe/MatchingEngine.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace obe;

// The sentinel handshake must process every enqueued command before exiting —
// nothing dropped, nothing double-counted — across many repetitions.
TEST(Concurrency, ShutdownHandshakeDropsNothing) {
    constexpr std::uint64_t N = 50'000;
    for (int rep = 0; rep < 20; ++rep) {
        MatchingEngine engine(1u << 12); // small queue forces real back-pressure
        engine.start();

        std::thread producer([&] {
            for (std::uint64_t i = 0; i < N; ++i) {
                Command c = Command::make_submit(i + 1, 1, Side::Buy,
                                                 100 + static_cast<Price>(i % 50),
                                                 1);
                while (!engine.queue().push(c)) {
                    std::this_thread::yield();
                }
            }
        });

        producer.join();
        engine.drain_and_join();

        // Every command was applied exactly once.
        EXPECT_EQ(engine.stats().commands, N);
        EXPECT_EQ(engine.stats().submits, N);
    }
}

// stats() is read concurrently with the consumer updating the counters. Under
// TSan this proves the read is race-free (atomic), and the counts stay sane.
TEST(Concurrency, ConcurrentStatsReadIsRaceFree) {
    constexpr std::uint64_t N = 200'000;
    MatchingEngine engine(1u << 14);
    engine.start();

    std::atomic<bool> stop_reader{false};
    std::thread reader([&] {
        std::uint64_t last = 0;
        while (!stop_reader.load(std::memory_order_acquire)) {
            const auto s = engine.stats();      // concurrent atomic read
            EXPECT_GE(s.commands, last);         // counters never go backwards
            last = s.commands;
        }
    });

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i) {
            Command c = Command::make_submit(i + 1, 1, Side::Sell,
                                             200 + static_cast<Price>(i % 20), 1);
            while (!engine.queue().push(c)) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    engine.drain_and_join();
    stop_reader.store(true, std::memory_order_release);
    reader.join();

    EXPECT_EQ(engine.stats().commands, N);
}

// The kill switch is engaged from one thread while the engine matches on
// another — a deliberate cross-thread control. No race, and orders submitted
// strictly after the switch is observed are rejected (so some get through, the
// rest are halted; total accounted for).
TEST(Concurrency, KillSwitchFromAnotherThread) {
    constexpr std::uint64_t N = 100'000;
    MatchingEngine engine(1u << 14);
    engine.enable_risk(RiskConfig{}); // permissive but kill-switchable
    engine.start();

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < N; ++i) {
            Command c = Command::make_submit(i + 1, 1, Side::Buy, 100, 1);
            while (!engine.queue().push(c)) {
                std::this_thread::yield();
            }
        }
    });

    // Halt partway through.
    engine.risk()->engage_kill_switch(true);

    producer.join();
    engine.drain_and_join();

    const auto m = engine.risk()->metrics();
    EXPECT_EQ(m.accepted + m.total_rejected(), N); // every order accounted for
    EXPECT_GT(m.kill_switch, 0u);                  // some were halted
}
