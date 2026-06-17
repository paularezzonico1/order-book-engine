#pragma once

#include "obe/Types.hpp"

namespace obe {

class PriceLevel; // forward declaration: an order knows the level it rests in.

// ---------------------------------------------------------------------------
// Order
// ---------------------------------------------------------------------------
//
// A single resting or aggressing limit order. The object doubles as an
// *intrusive* doubly-linked list node: each PriceLevel threads its FIFO queue
// directly through the Order's prev_/next_ pointers. Intrusive linkage is what
// makes cancellation O(1) — given the Order* we can splice it out without
// scanning the queue and without a separate node allocation.
//
// Orders are constructed exclusively from the MemoryPool on the hot path, so
// the class deliberately stays a trivial-to-construct aggregate of POD fields.
class Order {
public:
    Order() = default;

    Order(OrderId id, TraderId trader, Side side, Price price, Quantity qty) noexcept
        : id_(id),
          trader_(trader),
          side_(side),
          price_(price),
          quantity_(qty),
          remaining_(qty) {}

    // --- Identity / static attributes ------------------------------------
    OrderId id() const noexcept { return id_; }
    TraderId trader() const noexcept { return trader_; }
    Side side() const noexcept { return side_; }
    Price price() const noexcept { return price_; }
    Quantity quantity() const noexcept { return quantity_; }   // original size
    Sequence sequence() const noexcept { return sequence_; }

    // --- Mutable execution state -----------------------------------------
    Quantity remaining() const noexcept { return remaining_; }
    bool is_filled() const noexcept { return remaining_ == 0; }

    // Reduce remaining quantity by `qty`, returning the amount actually
    // filled (clamped so we never go negative).
    Quantity fill(Quantity qty) noexcept {
        const Quantity traded = qty < remaining_ ? qty : remaining_;
        remaining_ -= traded;
        return traded;
    }

    // The engine stamps the sequence number on acceptance.
    void set_sequence(Sequence seq) noexcept { sequence_ = seq; }

private:
    // Intrusive list linkage is owned by PriceLevel; OrderBook reads level_ to
    // perform O(1) cancellation. Kept private with friendship rather than
    // public accessors so external code cannot corrupt the linkage.
    friend class PriceLevel;
    friend class OrderBook;
    Order* prev_ = nullptr;
    Order* next_ = nullptr;
    PriceLevel* level_ = nullptr; // owning level, for O(1) cancel bookkeeping

    OrderId id_ = 0;
    TraderId trader_ = 0;
    Side side_ = Side::Buy;
    Price price_ = 0;
    Quantity quantity_ = 0;
    Quantity remaining_ = 0;
    Sequence sequence_ = 0;
};

// A value-type snapshot of one resting order's externally-meaningful state.
// Used to compare two books for bit-for-bit equality (e.g. live vs. replayed).
struct OrderSnapshot {
    Side side = Side::Buy;
    Price price = 0;
    OrderId id = 0;
    Quantity remaining = 0;
    Sequence sequence = 0;

    bool operator==(const OrderSnapshot& o) const noexcept {
        return side == o.side && price == o.price && id == o.id &&
               remaining == o.remaining && sequence == o.sequence;
    }
    bool operator!=(const OrderSnapshot& o) const noexcept {
        return !(*this == o);
    }
};

} // namespace obe
