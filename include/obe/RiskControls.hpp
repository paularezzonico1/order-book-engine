#pragma once

#include "obe/Types.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <unordered_set>

namespace obe {

// ---------------------------------------------------------------------------
// Pre-trade risk controls
// ---------------------------------------------------------------------------
//
// A broker/exchange must run pre-trade risk checks *in front of* the matching
// core — in the US this is a legal requirement (SEC Rule 15c3-5, the "market
// access rule"). This component is that gate: every inbound order is validated
// before it can touch the book, and every rejection is categorised and counted.
//
// It is deliberately a separate, single-responsibility object (not bolted into
// OrderBook) so it can be unit-tested in isolation and reused in front of any
// matching core.

enum class RejectReason : std::uint8_t {
    Accepted = 0,     // passed all checks
    MaxOrderSize,     // quantity exceeds the per-order size cap
    MaxNotional,      // price * quantity exceeds the notional cap
    PriceCollar,      // priced too far from the reference (fat-finger guard)
    KillSwitch,       // trading halted by the operator
    DuplicateOrder,   // order id already seen this session
};

inline const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::Accepted: return "ACCEPTED";
        case RejectReason::MaxOrderSize: return "MAX_ORDER_SIZE";
        case RejectReason::MaxNotional: return "MAX_NOTIONAL";
        case RejectReason::PriceCollar: return "PRICE_COLLAR";
        case RejectReason::KillSwitch: return "KILL_SWITCH";
        case RejectReason::DuplicateOrder: return "DUPLICATE_ORDER";
    }
    return "?";
}

// Limits. A zero value disables that individual check, so a default-constructed
// RiskConfig is permissive except for duplicate-id protection.
struct RiskConfig {
    Quantity max_order_size = 0;       // 0 = disabled
    std::uint64_t max_notional = 0;    // 0 = disabled; compared against price*qty
    std::uint32_t collar_bps = 0;      // 0 = disabled; max deviation from reference, in bps
    bool enforce_unique_ids = true;    // reject re-used client order ids this session
};

// Immutable copyable view of the counters (the atomics themselves are not
// copyable). Returned by RiskManager::metrics().
struct RiskMetricsSnapshot {
    std::uint64_t accepted = 0;
    std::uint64_t max_order_size = 0;
    std::uint64_t max_notional = 0;
    std::uint64_t price_collar = 0;
    std::uint64_t kill_switch = 0;
    std::uint64_t duplicate = 0;

    std::uint64_t total_rejected() const noexcept {
        return max_order_size + max_notional + price_collar + kill_switch +
               duplicate;
    }
};

class RiskManager {
public:
    explicit RiskManager(RiskConfig cfg = {}) : cfg_(cfg) {}

    // Validate one inbound order. `reference` anchors the fat-finger collar
    // (typically the opposite touch the order would trade against); pass
    // std::nullopt when no reference exists (e.g. an empty book) to skip the
    // collar. On Accepted, the id is recorded for duplicate protection.
    RejectReason check(OrderId id, Side side, Price price, Quantity qty,
                       std::optional<Price> reference);

    // The kill switch is the canonical *cross-thread* operational control: an
    // admin/control thread engages it while the matching thread reads it on the
    // hot path, hence std::atomic with release/acquire ordering.
    void engage_kill_switch(bool on) noexcept {
        kill_switch_.store(on, std::memory_order_release);
    }
    bool kill_switch_engaged() const noexcept {
        return kill_switch_.load(std::memory_order_acquire);
    }

    RiskMetricsSnapshot metrics() const noexcept;
    const RiskConfig& config() const noexcept { return cfg_; }

private:
    RiskConfig cfg_;
    std::atomic<bool> kill_switch_{false};

    // Session-unique client order ids. (Realistic: COIDs must be unique per
    // session; we never re-admit an id we have accepted.)
    std::unordered_set<OrderId> seen_ids_;

    // Counters are atomic so metrics() is race-free even if read from another
    // thread while the matching thread updates them. Relaxed is sufficient:
    // they are independent counters with no ordering relationship to protect.
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> rej_size_{0};
    std::atomic<std::uint64_t> rej_notional_{0};
    std::atomic<std::uint64_t> rej_collar_{0};
    std::atomic<std::uint64_t> rej_kill_{0};
    std::atomic<std::uint64_t> rej_dup_{0};
};

} // namespace obe
