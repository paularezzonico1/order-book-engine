// High-volume randomized stress test. Drives a large synthetic order flow
// through the engine and asserts global invariants, then re-runs the identical
// stream through the threaded SPSC pipeline and checks the two agree bit-for-
// bit (proving the lock-free hand-off preserves ordering and semantics).

#include "obe/MatchingEngine.hpp"
#include "obe/OrderGenerator.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <vector>

using namespace obe;

namespace {
std::vector<Command> make_stream(std::size_t n, std::uint64_t seed) {
    GeneratorConfig cfg;
    cfg.seed = seed;
    cfg.depth_levels = 40;
    OrderGenerator gen(cfg);
    std::vector<Command> cmds;
    cmds.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        cmds.push_back(gen.next());
    }
    return cmds;
}
} // namespace

TEST(Stress, InvariantsHoldUnderHighVolume) {
    constexpr std::size_t N = 200'000;
    const std::vector<Command> cmds = make_stream(N, /*seed=*/12345);

    MatchingEngine engine;
    for (const auto& c : cmds) {
        engine.apply(c);

        // The book must never be crossed after a command settles: any
        // marketable quantity is matched on submit.
        const auto bid = engine.book().best_bid();
        const auto ask = engine.book().best_ask();
        if (bid && ask) {
            ASSERT_LT(*bid, *ask) << "crossed book detected";
        }
    }

    // No Order leaked: every resting order owns exactly one live pool slot.
    EXPECT_EQ(engine.book().order_count(), engine.book().pool().live_count());
    EXPECT_GT(engine.stats().trades, 0u);
    EXPECT_EQ(engine.stats().commands, N);
}

TEST(Stress, ThreadedPipelineMatchesInlineExecution) {
    constexpr std::size_t N = 200'000;
    const std::vector<Command> cmds = make_stream(N, /*seed=*/67890);

    // Reference: single-threaded inline execution.
    MatchingEngine inlined;
    for (const auto& c : cmds) {
        inlined.apply(c);
    }

    // Pipeline: same stream across the lock-free queue, separate consumer thread.
    MatchingEngine piped(1u << 16);
    piped.start();
    std::thread producer([&] {
        for (const auto& c : cmds) {
            while (!piped.queue().push(c)) {
                std::this_thread::yield();
            }
        }
    });
    producer.join();
    piped.stop();

    // Identical inputs in identical order => identical outcome.
    EXPECT_EQ(piped.stats().commands, inlined.stats().commands);
    EXPECT_EQ(piped.stats().submits, inlined.stats().submits);
    EXPECT_EQ(piped.stats().cancels, inlined.stats().cancels);
    EXPECT_EQ(piped.stats().trades, inlined.stats().trades);
    EXPECT_EQ(piped.book().best_bid(), inlined.book().best_bid());
    EXPECT_EQ(piped.book().best_ask(), inlined.book().best_ask());
    EXPECT_EQ(piped.book().order_count(), inlined.book().order_count());
}
