#pragma once

#include "obe/Types.hpp"

namespace obe {

// ---------------------------------------------------------------------------
// Trade — an execution report
// ---------------------------------------------------------------------------
//
// Emitted every time an aggressing (taker) order matches a resting (maker)
// order. By exchange convention the fill prints at the *maker's* (resting)
// price — the passive order set the price and is rewarded with price
// improvement when the taker crossed through it.
struct Trade {
    Price price = 0;       // execution price (the resting/maker price)
    Quantity quantity = 0; // units exchanged

    OrderId taker_id = 0;  // aggressing order
    OrderId maker_id = 0;  // resting order that was hit
    TraderId taker_trader = 0;
    TraderId maker_trader = 0;
    Side taker_side = Side::Buy; // side of the aggressor
};

// ---------------------------------------------------------------------------
// SubmitResult — summary of what happened to a submitted order
// ---------------------------------------------------------------------------
enum class SubmitStatus : std::uint8_t {
    Rested,            // any remainder is now resting on the book
    FullyFilled,       // matched completely, nothing rested
    PartiallyFilled,   // matched some, remainder rested
    CancelledBySelfTrade, // STP=CancelAggressing removed the remainder
    Rejected,          // invalid (e.g. zero quantity, duplicate id)
    Cancelled,         // IOC: unfilled remainder cancelled (incl. zero-fill)
    Killed,            // FOK: could not fill in full, nothing was done
    StopAccepted,      // Stop: parked on the book awaiting its trigger price
};

struct SubmitResult {
    SubmitStatus status = SubmitStatus::Rejected;
    Quantity filled = 0;    // total quantity that traded
    Quantity resting = 0;   // quantity left resting on the book
    std::size_t trade_count = 0; // number of Trade reports this submit produced
};

} // namespace obe
