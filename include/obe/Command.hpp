#pragma once

#include "obe/Types.hpp"

namespace obe {

// ---------------------------------------------------------------------------
// Command — a unit of work flowing producer -> consumer across the ring buffer
// ---------------------------------------------------------------------------
//
// The order-generator thread emits Commands; the matching-engine thread applies
// them to the OrderBook. Keeping this a small trivially-copyable POD means
// pushing it through the SPSC queue is a cheap memcpy with no allocation, and
// it serializes to a fixed-width record in the binary event log.
enum class CommandType : std::uint8_t {
    Submit = 0,
    Cancel = 1,
    Shutdown = 2, // sentinel: tells the consumer to drain-and-exit (not logged)
};

struct Command {
    CommandType type = CommandType::Submit;
    OrderType order_type = OrderType::Limit;
    OrderId id = 0;
    TraderId trader = 0;
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
    Price trigger_price = 0; // only meaningful for OrderType::Stop

    // Plain limit order (the common case; keeps existing call sites working).
    static Command make_submit(OrderId id, TraderId trader, Side side,
                               Price price, Quantity quantity) noexcept {
        Command c;
        c.type = CommandType::Submit;
        c.order_type = OrderType::Limit;
        c.id = id;
        c.trader = trader;
        c.side = side;
        c.price = price;
        c.quantity = quantity;
        return c;
    }

    // Typed submit (IOC / FOK / Limit) at a limit price.
    static Command make_submit(OrderId id, TraderId trader, Side side,
                               Price price, Quantity quantity,
                               OrderType type) noexcept {
        Command c = make_submit(id, trader, side, price, quantity);
        c.order_type = type;
        return c;
    }

    // Stop-market order: parks until `trigger` is touched, then fires.
    static Command make_stop(OrderId id, TraderId trader, Side side,
                             Price trigger, Quantity quantity) noexcept {
        Command c;
        c.type = CommandType::Submit;
        c.order_type = OrderType::Stop;
        c.id = id;
        c.trader = trader;
        c.side = side;
        c.price = 0;
        c.quantity = quantity;
        c.trigger_price = trigger;
        return c;
    }

    static Command make_cancel(OrderId id) noexcept {
        Command c;
        c.type = CommandType::Cancel;
        c.id = id;
        return c;
    }

    static Command make_shutdown() noexcept {
        Command c;
        c.type = CommandType::Shutdown;
        return c;
    }
};

} // namespace obe
