#pragma once

#include "obe/Command.hpp"

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace obe {

class MatchingEngine; // forward decl for replay()

// ---------------------------------------------------------------------------
// Event sourcing: an append-only binary log of applied Commands
// ---------------------------------------------------------------------------
//
// The engine is deterministic: given the same ordered sequence of Commands it
// always reaches the same state. So the *only* thing we must persist to make
// the system crash-recoverable is the input stream. This is the event-sourcing
// model: journal every Command, and reconstruct state by replaying the journal.
//
// On-disk format (little-endian-on-disk assumes a same-endian reader; documented
// in EVENT_SOURCING.md):
//   * 8-byte header: magic "OBEL" + uint32 version
//   * then fixed-width 40-byte records, one per Command, in apply order.
// Fixed-width records mean replay never has to parse variable framing and the
// file offset of event N is trivially computable.
class EventLogWriter {
public:
    static constexpr char kMagic[4] = {'O', 'B', 'E', 'L'};
    static constexpr std::uint32_t kVersion = 1;
    static constexpr std::size_t kRecordSize = 40;

    // Opens `path` for writing, truncating any existing file, and writes the
    // header. Throws std::runtime_error if the file cannot be opened.
    explicit EventLogWriter(const std::string& path);

    // Append one command. (Shutdown sentinels are control-plane, not events —
    // callers should not pass them.)
    void append(const Command& cmd);

    void flush();
    std::uint64_t record_count() const noexcept { return count_; }

    ~EventLogWriter(); // flushes + closes (RAII)

private:
    std::ofstream out_;
    std::uint64_t count_ = 0;
};

// Read an entire log back into memory, validating the header. Throws
// std::runtime_error on a missing file or bad magic/version.
std::vector<Command> read_event_log(const std::string& path);

// Replay a recorded command stream into an engine, reconstructing its state.
// The engine must be fresh and must not itself have logging enabled.
void replay(const std::vector<Command>& events, MatchingEngine& engine);

// Convenience: read `path` and replay it into `engine`.
void replay_log(const std::string& path, MatchingEngine& engine);

} // namespace obe
