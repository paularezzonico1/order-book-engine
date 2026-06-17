#include "obe/RiskControls.hpp"

namespace obe {

RejectReason RiskManager::check(OrderId id, Side /*side*/, Price price,
                                Quantity qty, std::optional<Price> reference) {
    // Kill switch first: when halted, nothing gets through (cancels are handled
    // separately by the caller and remain allowed).
    if (kill_switch_.load(std::memory_order_acquire)) {
        rej_kill_.fetch_add(1, std::memory_order_relaxed);
        return RejectReason::KillSwitch;
    }

    // Max order size.
    if (cfg_.max_order_size != 0 && qty > cfg_.max_order_size) {
        rej_size_.fetch_add(1, std::memory_order_relaxed);
        return RejectReason::MaxOrderSize;
    }

    // Max notional (price * quantity).
    if (cfg_.max_notional != 0) {
        const std::uint64_t notional =
            static_cast<std::uint64_t>(price) * static_cast<std::uint64_t>(qty);
        if (notional > cfg_.max_notional) {
            rej_notional_.fetch_add(1, std::memory_order_relaxed);
            return RejectReason::MaxNotional;
        }
    }

    // Fat-finger price collar: reject if priced more than collar_bps away from
    // the reference. Pure integer math: |price-ref|*10000 > collar_bps*ref.
    if (cfg_.collar_bps != 0 && reference.has_value() && *reference > 0) {
        const Price ref = *reference;
        const Price diff = price > ref ? price - ref : ref - price;
        const std::uint64_t scaled =
            static_cast<std::uint64_t>(diff) * 10000ull;
        const std::uint64_t limit =
            static_cast<std::uint64_t>(cfg_.collar_bps) *
            static_cast<std::uint64_t>(ref);
        if (scaled > limit) {
            rej_collar_.fetch_add(1, std::memory_order_relaxed);
            return RejectReason::PriceCollar;
        }
    }

    // Duplicate client order id.
    if (cfg_.enforce_unique_ids) {
        if (!seen_ids_.insert(id).second) {
            rej_dup_.fetch_add(1, std::memory_order_relaxed);
            return RejectReason::DuplicateOrder;
        }
    }

    accepted_.fetch_add(1, std::memory_order_relaxed);
    return RejectReason::Accepted;
}

RiskMetricsSnapshot RiskManager::metrics() const noexcept {
    RiskMetricsSnapshot s;
    s.accepted = accepted_.load(std::memory_order_relaxed);
    s.max_order_size = rej_size_.load(std::memory_order_relaxed);
    s.max_notional = rej_notional_.load(std::memory_order_relaxed);
    s.price_collar = rej_collar_.load(std::memory_order_relaxed);
    s.kill_switch = rej_kill_.load(std::memory_order_relaxed);
    s.duplicate = rej_dup_.load(std::memory_order_relaxed);
    return s;
}

} // namespace obe
