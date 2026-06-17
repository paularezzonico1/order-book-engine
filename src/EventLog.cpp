#include "obe/EventLog.hpp"

#include "obe/MatchingEngine.hpp"

#include <cstring>
#include <stdexcept>

namespace obe {

namespace {

// Serialize a Command into a fixed 40-byte record. Fields are written in a
// fixed order and width so the layout is stable regardless of struct padding.
// (Endianness follows the writing host; see EVENT_SOURCING.md.)
void encode(const Command& c, char* buf) {
    std::memset(buf, 0, EventLogWriter::kRecordSize);
    buf[0] = static_cast<char>(c.type);
    buf[1] = static_cast<char>(c.order_type);
    buf[2] = static_cast<char>(c.side);
    // buf[3] padding
    std::memcpy(buf + 4, &c.trader, sizeof(c.trader));        // u32 @4
    std::memcpy(buf + 8, &c.id, sizeof(c.id));                // u64 @8
    std::memcpy(buf + 16, &c.price, sizeof(c.price));         // i64 @16
    std::memcpy(buf + 24, &c.quantity, sizeof(c.quantity));   // u64 @24
    std::memcpy(buf + 32, &c.trigger_price, sizeof(c.trigger_price)); // i64 @32
}

Command decode(const char* buf) {
    Command c;
    c.type = static_cast<CommandType>(static_cast<std::uint8_t>(buf[0]));
    c.order_type = static_cast<OrderType>(static_cast<std::uint8_t>(buf[1]));
    c.side = static_cast<Side>(static_cast<std::uint8_t>(buf[2]));
    std::memcpy(&c.trader, buf + 4, sizeof(c.trader));
    std::memcpy(&c.id, buf + 8, sizeof(c.id));
    std::memcpy(&c.price, buf + 16, sizeof(c.price));
    std::memcpy(&c.quantity, buf + 24, sizeof(c.quantity));
    std::memcpy(&c.trigger_price, buf + 32, sizeof(c.trigger_price));
    return c;
}

} // namespace

constexpr char EventLogWriter::kMagic[4];

EventLogWriter::EventLogWriter(const std::string& path)
    : out_(path, std::ios::binary | std::ios::trunc) {
    if (!out_) {
        throw std::runtime_error("EventLogWriter: cannot open " + path);
    }
    out_.write(kMagic, sizeof(kMagic));
    out_.write(reinterpret_cast<const char*>(&kVersion), sizeof(kVersion));
}

void EventLogWriter::append(const Command& cmd) {
    // The shutdown sentinel is control-plane, never a domain event. Drop it
    // defensively so a log can never contain — and thus never replay — one,
    // regardless of what the caller passes.
    if (cmd.type == CommandType::Shutdown) {
        return;
    }
    char buf[kRecordSize];
    encode(cmd, buf);
    out_.write(buf, kRecordSize);
    ++count_;
}

void EventLogWriter::flush() { out_.flush(); }

EventLogWriter::~EventLogWriter() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

std::vector<Command> read_event_log(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("read_event_log: cannot open " + path);
    }

    char magic[4];
    std::uint32_t version = 0;
    in.read(magic, sizeof(magic));
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (!in || std::memcmp(magic, EventLogWriter::kMagic, 4) != 0) {
        throw std::runtime_error("read_event_log: bad magic in " + path);
    }
    if (version != EventLogWriter::kVersion) {
        throw std::runtime_error("read_event_log: unsupported version");
    }

    // Read complete fixed-width records. If the final record is torn (a writer
    // crash mid-append), the last read() comes up short and the loop stops,
    // dropping the partial record. That is the intended crash-recovery
    // semantics: a command that was not durably written is treated as never
    // having happened, leaving a consistent prefix of the stream.
    std::vector<Command> events;
    char buf[EventLogWriter::kRecordSize];
    while (in.read(buf, EventLogWriter::kRecordSize)) {
        events.push_back(decode(buf));
    }
    return events;
}

void replay(const std::vector<Command>& events, MatchingEngine& engine) {
    for (const Command& c : events) {
        engine.apply(c);
    }
}

void replay_log(const std::string& path, MatchingEngine& engine) {
    replay(read_event_log(path), engine);
}

} // namespace obe
