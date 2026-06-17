#include "obe/MatchingEngine.hpp"

namespace obe {

std::size_t MatchingEngine::apply(const Command& cmd) {
    // Journal the command as received (the event-sourcing record) before it is
    // processed, so the log is a faithful, replayable input stream.
    if (log_ && cmd.type != CommandType::Shutdown) {
        log_->append(cmd);
    }
    switch (cmd.type) {
        case CommandType::Submit: {
            // Pre-trade risk gate (if installed) runs *before* the book sees
            // the order. The collar anchors on the opposite touch — the price
            // this order would actually trade against.
            if (risk_) {
                const std::optional<Price> reference =
                    cmd.side == Side::Buy ? book_.best_ask() : book_.best_bid();
                if (risk_->check(cmd.id, cmd.side, cmd.price, cmd.quantity,
                                 reference) != RejectReason::Accepted) {
                    commands_.fetch_add(1, std::memory_order_relaxed);
                    return 0; // received, but rejected pre-trade
                }
            }
            const SubmitResult r = book_.submit(cmd.id, cmd.trader, cmd.side,
                                                cmd.price, cmd.quantity);
            submits_.fetch_add(1, std::memory_order_relaxed);
            trades_.fetch_add(r.trade_count, std::memory_order_relaxed);
            commands_.fetch_add(1, std::memory_order_relaxed);
            return r.trade_count;
        }
        case CommandType::Cancel:
            book_.cancel(cmd.id);
            cancels_.fetch_add(1, std::memory_order_relaxed);
            commands_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        case CommandType::Shutdown:
            return 0; // handled by consume_loop; a no-op if applied inline
    }
    return 0;
}

MatchingEngine::Stats MatchingEngine::stats() const noexcept {
    Stats s;
    s.commands = commands_.load(std::memory_order_relaxed);
    s.submits = submits_.load(std::memory_order_relaxed);
    s.cancels = cancels_.load(std::memory_order_relaxed);
    s.trades = trades_.load(std::memory_order_relaxed);
    return s;
}

void MatchingEngine::consume_loop() {
    Command cmd;
    for (;;) {
        if (queue_.pop(cmd)) {
            // The shutdown sentinel is ordered behind every prior command in
            // the FIFO, so reaching it guarantees all real work is done.
            if (cmd.type == CommandType::Shutdown) {
                break;
            }
            apply(cmd);
        } else {
            // Empty queue: yield rather than burning a core. A latency-tuned
            // build would busy-spin with a pause; we favour being a good
            // citizen on shared CI hardware.
            std::this_thread::yield();
        }
    }
}

void MatchingEngine::start() {
    worker_ = std::thread(&MatchingEngine::consume_loop, this);
}

void MatchingEngine::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void MatchingEngine::drain_and_join() {
    // Push the sentinel (blocking if the queue is momentarily full), then join.
    while (!queue_.push(Command::make_shutdown())) {
        std::this_thread::yield();
    }
    join();
}

} // namespace obe
