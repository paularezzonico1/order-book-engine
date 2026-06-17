// Event-sourcing tests: serialization round-trip, and the headline property —
// replaying the journal reconstructs the live book bit-for-bit.

#include "obe/EventLog.hpp"
#include "obe/MatchingEngine.hpp"
#include "obe/OrderGenerator.hpp"

#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace obe;

namespace {
std::string temp_path(const char* name) {
    return ::testing::TempDir() + name;
}

std::vector<Command> make_stream(std::size_t n, std::uint64_t seed) {
    GeneratorConfig cfg;
    cfg.seed = seed;
    cfg.depth_levels = 30;
    OrderGenerator gen(cfg);
    std::vector<Command> cmds;
    cmds.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        cmds.push_back(gen.next());
    }
    return cmds;
}
} // namespace

TEST(EventLog, SerializationRoundTrip) {
    const std::string path = temp_path("obe_roundtrip.bin");
    {
        EventLogWriter w(path);
        w.append(Command::make_submit(1, 7, Side::Buy, 12345, 10));
        w.append(Command::make_submit(2, 8, Side::Sell, 12346, 20));
        w.append(Command::make_cancel(1));
    } // RAII flush/close

    const std::vector<Command> events = read_event_log(path);
    ASSERT_EQ(events.size(), 3u);

    EXPECT_EQ(events[0].type, CommandType::Submit);
    EXPECT_EQ(events[0].id, 1u);
    EXPECT_EQ(events[0].trader, 7u);
    EXPECT_EQ(events[0].side, Side::Buy);
    EXPECT_EQ(events[0].price, 12345);
    EXPECT_EQ(events[0].quantity, 10u);

    EXPECT_EQ(events[1].side, Side::Sell);
    EXPECT_EQ(events[1].price, 12346);

    EXPECT_EQ(events[2].type, CommandType::Cancel);
    EXPECT_EQ(events[2].id, 1u);
}

TEST(EventLog, ReplayReconstructsStateBitForBit) {
    constexpr std::size_t N = 100'000;
    const std::vector<Command> cmds = make_stream(N, /*seed=*/2024);
    const std::string path = temp_path("obe_session.bin");

    // --- Live run, journaling every command ---
    MatchingEngine live;
    live.enable_logging(path);
    for (const auto& c : cmds) {
        live.apply(c);
    }
    live.flush_log();

    // The journal must contain exactly the applied commands.
    const std::vector<Command> events = read_event_log(path);
    ASSERT_EQ(events.size(), N);

    // --- Replay into a fresh engine purely from the log ---
    MatchingEngine replayed;
    replay(events, replayed);

    // --- Prove equivalence ---
    EXPECT_EQ(live.book().snapshot(), replayed.book().snapshot()); // resting state
    EXPECT_EQ(live.stats().commands, replayed.stats().commands);
    EXPECT_EQ(live.stats().submits, replayed.stats().submits);
    EXPECT_EQ(live.stats().cancels, replayed.stats().cancels);
    EXPECT_EQ(live.stats().trades, replayed.stats().trades);
    EXPECT_EQ(live.book().best_bid(), replayed.book().best_bid());
    EXPECT_EQ(live.book().best_ask(), replayed.book().best_ask());
}

TEST(EventLog, ReplayFromThreadedLiveRun) {
    constexpr std::size_t N = 100'000;
    const std::vector<Command> cmds = make_stream(N, /*seed=*/99);
    const std::string path = temp_path("obe_threaded_session.bin");

    // Live run through the SPSC pipeline, journaling on the consumer thread.
    MatchingEngine live(1u << 16);
    live.enable_logging(path);
    live.start();
    for (const auto& c : cmds) {
        while (!live.queue().push(c)) {
            std::this_thread::yield();
        }
    }
    live.drain_and_join();
    live.flush_log();

    MatchingEngine replayed;
    replay_log(path, replayed);

    EXPECT_EQ(live.book().snapshot(), replayed.book().snapshot());
    EXPECT_EQ(live.stats().trades, replayed.stats().trades);
}
