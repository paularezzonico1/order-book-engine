#pragma once

#include <cstdint>
#include <ostream>

namespace obe {

// ---------------------------------------------------------------------------
// Domain primitives
// ---------------------------------------------------------------------------
//
// Price is expressed in integer "ticks" (e.g. cents) rather than floating
// point. Real exchanges never use float for prices: rounding error breaks
// price-time priority and equality comparisons. A signed 64-bit integer keeps
// arithmetic exact and comparisons trivial.
using Price = std::int64_t;

// Quantities and identifiers are unsigned 64-bit. Quantity 0 is never a valid
// resting order; it is used internally to mean "fully consumed".
using Quantity = std::uint64_t;
using OrderId = std::uint64_t;

// Monotonic sequence number assigned by the engine on acceptance. It is the
// tie-breaker that enforces *time* priority within a single price level: lower
// sequence == arrived earlier == matched first.
using Sequence = std::uint64_t;

// Identifies the participant that submitted an order. Used by self-trade
// prevention to avoid matching a participant against their own resting order.
using TraderId = std::uint32_t;

// ---------------------------------------------------------------------------
// Side of the book
// ---------------------------------------------------------------------------
enum class Side : std::uint8_t {
    Buy = 0,   // bid  — wants to buy at or below its limit price
    Sell = 1,  // ask  — wants to sell at or above its limit price
};

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

inline const char* to_string(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

inline std::ostream& operator<<(std::ostream& os, Side s) {
    return os << to_string(s);
}

// ---------------------------------------------------------------------------
// Order type / time-in-force
// ---------------------------------------------------------------------------
// Limit  — rest any unmatched remainder on the book (the default).
// IOC    — Immediate-Or-Cancel: match what is available now, cancel the rest.
// FOK    — Fill-Or-Kill: fill the entire quantity immediately or do nothing.
// Stop   — parked until a trigger price is touched, then fires as a market
//          order (an IOC priced to sweep the book).
enum class OrderType : std::uint8_t {
    Limit = 0,
    IOC = 1,
    FOK = 2,
    Stop = 3,
};

inline const char* to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit: return "LIMIT";
        case OrderType::IOC: return "IOC";
        case OrderType::FOK: return "FOK";
        case OrderType::Stop: return "STOP";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Self-trade prevention policy
// ---------------------------------------------------------------------------
// When an aggressing order would match a resting order from the same trader,
// one of the two must be removed instead of trading.
enum class SelfTradePolicy : std::uint8_t {
    None = 0,         // allow the trade (no STP)
    CancelResting,    // cancel the resting (older) order, keep aggressing
    CancelAggressing, // cancel the remainder of the aggressing order
};

} // namespace obe
