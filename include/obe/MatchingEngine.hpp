#pragma once

#include "obe/Command.hpp"
#include "obe/OrderBook.hpp"
#include "obe/RingBuffer.hpp"
#include "obe/RiskControls.hpp"
#include "obe/Types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

    // Launch the consumer thread. The engine runs until stop() drains and joins.
    void start();
    void stop();

    Stats stats() const noexcept { return stats_; }

    // Apply a single command inline (used by single-threaded benchmarks/tests
    // that bypass the queue). Returns the number of trades it produced.
    std::size_t apply(const Command& cmd);

private:
    void consume_loop();

    SpscRingBuffer<Command> queue_;
    OrderBook book_;
    std::unique_ptr<RiskManager> risk_; // null => risk gate disabled
    std::thread worker_;
    std::atomic<bool> running_{false};
    Stats stats_{};
};

} // namespace obe
