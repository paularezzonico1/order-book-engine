#pragma once

#include "obe/FlatHashMap.hpp"
#include "obe/MemoryPool.hpp"
#include "obe/Order.hpp"
#include "obe/PriceLevel.hpp"
#include "obe/Trade.hpp"
#include "obe/Types.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <vector>

namespace obe {

// ---------------------------------------------------------------------------
// OrderBook — one limit order book for a single instrument
// ---------------------------------------------------------------------------
//
// Data structures (and why):
//   * bids_ : std::map<Price, PriceLevel, std::greater<>>  — best bid first
//   * asks_ : std::map<Price, PriceLevel, std::less<>>     — best ask first
//     std::map is a red-black tree, giving O(log n) insert/find/erase of a
//     price level and, crucially, an *ordered* iterator so begin() is always
//     the best price. Comparators are flipped per side so begin() is the
//     side's most aggressive price in both cases.
//   * index_ : FlatHashMap<OrderId, Order*> — O(1) average lookup so cancel
//     can find an order without scanning, then splice it out in O(1). This is
//     an open-addressing, allocation-free table (see FlatHashMap.hpp / INDEX.md)
//     chosen over std::unordered_map to keep order indexing off the heap.
//   * pool_  : MemoryPool<Order> — Order objects for resting orders come from
//     here, never from the global allocator.
//
// Matching obeys strict price-time priority and supports full fills, partial
// fills, cancellation, and self-trade prevention.
class OrderBook {
public:
    explicit OrderBook(SelfTradePolicy stp = SelfTradePolicy::CancelResting,
                       std::size_t pool_capacity = 4096)
        : stp_(stp), pool_(pool_capacity) {
        trades_.reserve(64);
        // Pre-size the id index to the expected peak resting-order count. The
        // profiler showed __do_rehash dominating submit() under load; reserving
        // up front removes incremental rehashing from the hot path entirely.
        index_.reserve(pool_capacity);
    }

    // Submit a new limit order. Marketable quantity is matched immediately;
    // any remainder rests on the book. Trades produced are retrievable via
    // last_trades() until the next submit() call.
    SubmitResult submit(OrderId id, TraderId trader, Side side, Price price,
                        Quantity quantity);

    // Cancel a resting order by id. Returns true if it existed and was
    // removed. O(1) average (hash lookup) + O(1) splice + amortised O(log n)
    // if the level becomes empty and must be erased.
    bool cancel(OrderId id);

    // --- Top-of-book / depth queries (all O(1) or O(log n)) --------------
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity quantity_at(Side side, Price price) const;
    std::size_t bid_levels() const noexcept { return bids_.size(); }
    std::size_t ask_levels() const noexcept { return asks_.size(); }
    std::size_t order_count() const noexcept { return index_.size(); }

    // Trades produced by the most recent submit().
    const std::vector<Trade>& last_trades() const noexcept { return trades_; }

    // Pool introspection (tests assert no Order leaks).
    const MemoryPool<Order>& pool() const noexcept { return pool_; }

private:
    // Match an aggressing order against the resting `book` (the opposite side).
    // Mutates `remaining` in place; appends to trades_. Templated on the map
    // type so it works for either comparator without code duplication.
    template <typename BookSide>
    void match(BookSide& book, OrderId taker_id, TraderId taker_trader,
               Side taker_side, Price limit, Quantity& remaining,
               bool& cancelled_by_stp);

    // Does `taker_side` at `limit` cross a resting order priced at `resting`?
    static bool crosses(Side taker_side, Price limit, Price resting) noexcept {
        return taker_side == Side::Buy ? resting <= limit : resting >= limit;
    }

    void remove_from_index(Order* order) { index_.erase(order->id()); }

    SelfTradePolicy stp_;
    MemoryPool<Order> pool_;

    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>> asks_;
    FlatHashMap<OrderId, Order*> index_;

    Sequence next_seq_ = 1;
    std::vector<Trade> trades_; // reused buffer; cleared each submit()
};

} // namespace obe
