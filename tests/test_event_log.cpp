// Event-sourcing tests: serialization round-trip, and the headline property —
// replaying the journal reconstructs the live book bit-for-bit.

#include "obe/EventLog.hpp"
#include "obe/MatchingEngine.hpp"
#include "obe/OrderGenerator.hpp"

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
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

// Assert two engines hold identical externally-observable state.
void expect_equivalent(const MatchingEngine& a, const MatchingEngine& b) {
    EXPECT_EQ(a.book().snapshot(), b.book().snapshot());
    EXPECT_EQ(a.book().best_bid(), b.book().best_bid());
    EXPECT_EQ(a.book().best_ask(), b.book().best_ask());
    EXPECT_EQ(a.book().order_count(), b.book().order_count());
    EXPECT_EQ(a.stats().commands, b.stats().commands);
    EXPECT_EQ(a.stats().submits, b.stats().submits);
    EXPECT_EQ(a.stats().cancels, b.stats().cancels);
    EXPECT_EQ(a.stats().trades, b.stats().trades);
}

void write_raw(const std::string& path, const char* bytes, std::size_t n) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes, static_cast<std::streamsize>(n));
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

// --------------------------------------------------------------------------
// Sentinel handling: the shutdown command must never be journaled or replayed.
// --------------------------------------------------------------------------
TEST(EventLog, ShutdownSentinelIsNeverJournaled) {
    const std::string path = temp_path("obe_sentinel.bin");
    {
        EventLogWriter w(path);
        w.append(Command::make_submit(1, 1, Side::Buy, 100, 5));
        w.append(Command::make_shutdown()); // must be dropped defensively
        w.append(Command::make_cancel(1));
        EXPECT_EQ(w.record_count(), 2u);
    }
    const std::vector<Command> events = read_event_log(path);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].type, CommandType::Submit);
    EXPECT_EQ(events[1].type, CommandType::Cancel);

    // And via the engine path: applying a sentinel inline journals nothing.
    const std::string path2 = temp_path("obe_sentinel2.bin");
    MatchingEngine eng;
    eng.enable_logging(path2);
    eng.apply(Command::make_submit(1, 1, Side::Buy, 100, 5));
    eng.apply(Command::make_shutdown());
    eng.flush_log();
    EXPECT_EQ(read_event_log(path2).size(), 1u);
}

// --------------------------------------------------------------------------
// Malformed / truncated logs.
// --------------------------------------------------------------------------
TEST(EventLog, MissingFileThrows) {
    EXPECT_THROW(read_event_log(temp_path("obe_does_not_exist.bin")),
                 std::runtime_error);
}

TEST(EventLog, BadMagicThrows) {
    const std::string path = temp_path("obe_badmagic.bin");
    const char bytes[8] = {'X', 'X', 'X', 'X', 1, 0, 0, 0};
    write_raw(path, bytes, sizeof(bytes));
    EXPECT_THROW(read_event_log(path), std::runtime_error);
}

TEST(EventLog, UnsupportedVersionThrows) {
    const std::string path = temp_path("obe_badversion.bin");
    char bytes[8] = {'O', 'B', 'E', 'L', 0, 0, 0, 0};
    const std::uint32_t bad = EventLogWriter::kVersion + 99;
    std::memcpy(bytes + 4, &bad, sizeof(bad));
    write_raw(path, bytes, sizeof(bytes));
    EXPECT_THROW(read_event_log(path), std::runtime_error);
}

TEST(EventLog, ShortHeaderThrows) {
    const std::string path = temp_path("obe_shorthdr.bin");
    const char bytes[3] = {'O', 'B', 'E'}; // fewer than 8 header bytes
    write_raw(path, bytes, sizeof(bytes));
    EXPECT_THROW(read_event_log(path), std::runtime_error);
}

TEST(EventLog, HeaderOnlyLogReplaysToEmptyBook) {
    const std::string path = temp_path("obe_headeronly.bin");
    { EventLogWriter w(path); } // writes header, no records
    const std::vector<Command> events = read_event_log(path);
    EXPECT_TRUE(events.empty());

    MatchingEngine replayed;
    replay(events, replayed);
    EXPECT_EQ(replayed.book().order_count(), 0u);
    EXPECT_EQ(replayed.stats().commands, 0u);
}

TEST(EventLog, TornTrailingRecordIsRecoveredGracefully) {
    const std::string path = temp_path("obe_torn.bin");
    {
        EventLogWriter w(path);
        w.append(Command::make_submit(1, 1, Side::Buy, 100, 5));
        w.append(Command::make_submit(2, 1, Side::Sell, 101, 5));
    }
    // Simulate a crash mid-append: tack on a partial (17-byte) record.
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        const char partial[17] = {0};
        out.write(partial, sizeof(partial));
    }

    // The two complete records are recovered; the torn tail is dropped.
    const std::vector<Command> events = read_event_log(path);
    ASSERT_EQ(events.size(), 2u);

    MatchingEngine replayed;
    replay(events, replayed);
    EXPECT_EQ(replayed.book().order_count(), 2u);
}

// --------------------------------------------------------------------------
// Replay equivalence across the full feature surface.
// --------------------------------------------------------------------------
TEST(EventLog, ReplayAcrossCancellationsAndPartialFills) {
    const std::string path = temp_path("obe_cancel_partial.bin");
    const std::vector<Command> cmds = {
        Command::make_submit(1, 1, Side::Buy, 100, 10),
        Command::make_submit(2, 2, Side::Sell, 100, 4),  // partial-fills id1
        Command::make_submit(3, 2, Side::Sell, 100, 10), // fills id1, rests 4
        Command::make_cancel(3),                          // cancel the resting ask
        Command::make_submit(4, 3, Side::Buy, 99, 5),     // rests
        Command::make_cancel(999),                        // cancel of unknown id
    };

    MatchingEngine live;
    live.enable_logging(path);
    for (const auto& c : cmds) live.apply(c);
    live.flush_log();

    MatchingEngine replayed;
    replay_log(path, replayed);
    expect_equivalent(live, replayed);
    // Sanity: id4 rests at 99, nothing else.
    EXPECT_EQ(replayed.book().best_bid(), 99);
    EXPECT_FALSE(replayed.book().best_ask().has_value());
}

TEST(EventLog, ReplayWithSelfTradePrevention) {
    const std::string path = temp_path("obe_stp.bin");
    const std::vector<Command> cmds = {
        Command::make_submit(1, 7, Side::Buy, 100, 10),  // trader 7 resting bid
        Command::make_submit(2, 9, Side::Buy, 100, 10),  // trader 9 behind
        Command::make_submit(3, 7, Side::Sell, 100, 10), // trader 7 crosses own
    };

    // Both engines must share the same self-trade policy to reproduce state.
    MatchingEngine live(1u << 12, SelfTradePolicy::CancelResting);
    live.enable_logging(path);
    for (const auto& c : cmds) live.apply(c);
    live.flush_log();

    MatchingEngine replayed(1u << 12, SelfTradePolicy::CancelResting);
    replay_log(path, replayed);
    expect_equivalent(live, replayed);
}

TEST(EventLog, ReplayWithRejectedRiskOrders) {
    const std::string path = temp_path("obe_risk.bin");
    RiskConfig cfg;
    cfg.max_order_size = 100; // orders above 100 are rejected pre-trade

    const std::vector<Command> cmds = {
        Command::make_submit(1, 1, Side::Buy, 100, 50),   // accepted, rests
        Command::make_submit(2, 1, Side::Buy, 100, 5000), // REJECTED (size)
        Command::make_submit(3, 2, Side::Sell, 100, 50),  // accepted, fills id1
    };

    MatchingEngine live;
    live.enable_risk(cfg);
    live.enable_logging(path);
    for (const auto& c : cmds) live.apply(c);
    live.flush_log();

    // Replay engine must carry the identical risk config: rejected orders are
    // journaled too and must be re-rejected the same way.
    MatchingEngine replayed;
    replayed.enable_risk(cfg);
    replay_log(path, replayed);

    expect_equivalent(live, replayed);
    EXPECT_GT(live.risk()->metrics().max_order_size, 0u);
    EXPECT_EQ(live.risk()->metrics().max_order_size,
              replayed.risk()->metrics().max_order_size);
    EXPECT_EQ(live.risk()->metrics().accepted,
              replayed.risk()->metrics().accepted);
}

TEST(EventLog, StressReplayAcrossSeeds) {
    for (std::uint64_t seed : {1u, 7u, 42u, 1234u}) {
        const std::vector<Command> cmds = make_stream(50'000, seed);
        const std::string path = temp_path("obe_stress_seed.bin");

        MatchingEngine live;
        live.enable_logging(path);
        for (const auto& c : cmds) live.apply(c);
        live.flush_log();

        MatchingEngine replayed;
        replay_log(path, replayed);
        expect_equivalent(live, replayed);
        ASSERT_EQ(read_event_log(path).size(), cmds.size());
    }
}
