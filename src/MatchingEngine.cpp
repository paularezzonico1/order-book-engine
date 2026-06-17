#include "obe/MatchingEngine.hpp"

namespace obe {

std::size_t MatchingEngine::apply(const Command& cmd) {
    switch (cmd.type) {
        case CommandType::Submit: {
            const SubmitResult r = book_.submit(cmd.id, cmd.trader, cmd.side,
                                                cmd.price, cmd.quantity);
            ++stats_.submits;
            stats_.trades += r.trade_count;
            ++stats_.commands;
            return r.trade_count;
        }
        case CommandType::Cancel:
            book_.cancel(cmd.id);
            ++stats_.cancels;
            ++stats_.commands;
            return 0;
    }
    return 0;
}

void MatchingEngine::consume_loop() {
    Command cmd;
    // Run while requested, and after a stop request keep draining until the
    // queue is empty so no in-flight command is dropped.
    while (running_.load(std::memory_order_acquire) || !queue_.empty()) {
        if (queue_.pop(cmd)) {
            apply(cmd);
        } else {
            // Nothing to do: yield rather than burning a core in a tight spin.
            // A real exchange might busy-spin for lower latency; we favour
            // being a good citizen on shared CI hardware.
            std::this_thread::yield();
        }
    }
}

void MatchingEngine::start() {
    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&MatchingEngine::consume_loop, this);
}

void MatchingEngine::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
}

} // namespace obe
