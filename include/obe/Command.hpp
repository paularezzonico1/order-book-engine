#pragma once

#include "obe/Types.hpp"

namespace obe {

// ---------------------------------------------------------------------------
// Command — a unit of work flowing producer -> consumer across the ring buffer
// ---------------------------------------------------------------------------
//
// The order-generator thread emits Commands; the matching-engine thread applies
// them to the OrderBook. Keeping this a small trivially-copyable POD means
// pushing it through the SPSC queue is a cheap memcpy with no allocation.
enum class CommandType : std::uint8_t {
    Submit = 0,
    Cancel = 1,
};

struct Command {
    CommandType type = CommandType::Submit;
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;

    static Command make_submit(OrderId id, TraderId trader, Side side,
                               Price price, Quantity quantity) noexcept {
        return Command{CommandType::Submit, id, trader, side, price, quantity};
    }

    static Command make_cancel(OrderId id) noexcept {
        Command c;
        c.type = CommandType::Cancel;
        c.id = id;
        return c;
    }
};

} // namespace obe
