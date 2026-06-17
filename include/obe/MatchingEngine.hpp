#pragma once

#include "obe/Command.hpp"
#include "obe/EventLog.hpp"
#include "obe/OrderBook.hpp"
#include "obe/RingBuffer.hpp"
#include "obe/RiskControls.hpp"
#include "obe/Types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace obe {

// ---------------------------------------------------------------------------
// MatchingEngine — the consumer side of the pipeline
// ---------------------------------------------------------------------------
//
// Owns the OrderBook and an SPSC ring buffer. A producer thread enqueues
// Commands via queue(); the engine's own thread dequeues them and applies them
// to the book. This separation models a real trading system where a network/
// gateway thread feeds a single-writer matching core, and it lets us benchmark
// the cross-thread hand-off in isolation from the matching cost.
class MatchingEngine {
public:
    // Copyable, plain snapshot returned by stats(). The live counters behind it
    // are atomics (see below) so this read is race-free even mid-run.
    struct Stats {
        std::uint64_t commands = 0;
        std::uint64_t submits = 0;
        std::uint64_t cancels = 0;
        std::uint64_t trades = 0;
    };

    explicit MatchingEngine(std::size_t queue_capacity = 1u << 16,
                            SelfTradePolicy stp = SelfTradePolicy::CancelResting,
                            std::size_t pool_capacity = 1u << 16)
        : queue_(queue_capacity), book_(stp, pool_capacity) {}

    SpscRingBuffer<Command>& queue() noexcept { return queue_; }
    OrderBook& book() noexcept { return book_; }
    const OrderBook& book() const noexcept { return book_; }

    // Install a pre-trade risk gate. Off by default (zero-overhead) so the
    // perf-sensitive benchmarks measure the matching core in isolation; once
    // enabled, every inbound submit is risk-checked before reaching the book.
    void enable_risk(RiskConfig cfg) { risk_ = std::make_unique<RiskManager>(cfg); }
    RiskManager* risk() noexcept { return risk_.get(); }
    const RiskManager* risk() const noexcept { return risk_.get(); }

    // Event sourcing: journal every applied command to an append-only log so
    // the session can be replayed/recovered. Off by default. Logging happens on
    // the consumer thread (single writer), so there is no contention.
    void enable_logging(const std::string& path) {
        log_ = std::make_unique<EventLogWriter>(path);
    }
    void flush_log() {
        if (log_) {
            log_->flush();
        }
    }

    // Launch the consumer thread. It runs until it dequeues the shutdown
    // sentinel (see drain_and_join), so shutdown is data-driven and ordered
    // behind every previously-enqueued command — no command can be dropped.
    void start();

    // Enqueue the shutdown sentinel and join the consumer. Contract: the
    // producer must have finished pushing (no other thread is concurrently
    // pushing) — typically called right after the producer thread is joined.
    // This is the clean producer-complete handshake that replaces the old
    // running-flag race.
    void drain_and_join();

    // Join without enqueuing a sentinel (for callers that pushed their own).
    void join();

    // Race-free snapshot of the counters (atomic loads under the hood).
    Stats stats() const noexcept;

    // Apply a single command inline (used by single-threaded benchmarks/tests
    // that bypass the queue). Returns the number of trades it produced.
    std::size_t apply(const Command& cmd);

private:
    void consume_loop();

    SpscRingBuffer<Command> queue_;
    OrderBook book_;
    std::unique_ptr<RiskManager> risk_;      // null => risk gate disabled
    std::unique_ptr<EventLogWriter> log_;    // null => journaling disabled
    std::thread worker_;

    // Counters live as atomics: the consumer thread writes them while stats()
    // may read them from another thread. Relaxed ordering suffices — they are
    // independent tallies with nothing else to order against.
    std::atomic<std::uint64_t> commands_{0};
    std::atomic<std::uint64_t> submits_{0};
    std::atomic<std::uint64_t> cancels_{0};
    std::atomic<std::uint64_t> trades_{0};
};

} // namespace obe
