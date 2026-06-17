#include "obe/OrderBook.hpp"

namespace obe {

// ---------------------------------------------------------------------------
// match — consume an aggressing order against the resting opposite side
// ---------------------------------------------------------------------------
template <typename BookSide>
void OrderBook::match(BookSide& book, OrderId taker_id, TraderId taker_trader,
                      Side taker_side, Price limit, Quantity& remaining,
                      bool& cancelled_by_stp) {
    // Walk price levels best-first. book.begin() is always the most aggressive
    // resting price because the comparator is chosen per side.
    while (remaining > 0 && !book.empty()) {
        auto level_it = book.begin();
        PriceLevel& level = level_it->second;

        // Stop as soon as the best opposite price no longer crosses our limit.
        if (!crosses(taker_side, limit, level.price())) {
            break;
        }

        // Within a level, the front order is the oldest -> time priority.
        while (remaining > 0 && !level.empty()) {
            Order* maker = level.front();

            // ---- Self-trade prevention -----------------------------------
            if (stp_ != SelfTradePolicy::None &&
                maker->trader() == taker_trader) {
                if (stp_ == SelfTradePolicy::CancelResting) {
                    // Remove the older resting order and keep matching.
                    level.unlink(maker);
                    remove_from_index(maker);
                    pool_.release(maker);
                    continue;
                }
                // CancelAggressing: abandon the rest of the taker. `remaining`
                // is left intact so the caller can report what already filled.
                cancelled_by_stp = true;
                return;
            }

            // ---- Execute the fill (at the maker/resting price) -----------
            const Quantity maker_rem = maker->remaining();
            const Quantity traded = remaining < maker_rem ? remaining : maker_rem;

            trades_.push_back(Trade{maker->price(), traded, taker_id,
                                    maker->id(), taker_trader, maker->trader(),
                                    taker_side});

            remaining -= traded;
            maker->fill(traded);
            level.on_fill(traded);

            if (maker->is_filled()) {
                level.unlink(maker);
                remove_from_index(maker);
                pool_.release(maker);
            }
        }

        // A fully-consumed level is removed so begin() stays valid/cheap.
        if (level.empty()) {
            book.erase(level_it);
        }
    }
}

// ---------------------------------------------------------------------------
// submit — match marketable quantity, then rest any remainder
// ---------------------------------------------------------------------------
SubmitResult OrderBook::submit(OrderId id, TraderId trader, Side side,
                               Price price, Quantity quantity) {
    trades_.clear();
    SubmitResult res;

    // Reject malformed or duplicate orders rather than corrupting the book.
    if (quantity == 0 || index_.find(id) != index_.end()) {
        res.status = SubmitStatus::Rejected;
        return res;
    }

    Quantity remaining = quantity;
    bool cancelled_by_stp = false;

    // The taker is matched using locals only: if it fully fills it never needs
    // an Order object, sparing a pool acquire/release on the common path.
    if (side == Side::Buy) {
        match(asks_, id, trader, side, price, remaining, cancelled_by_stp);
    } else {
        match(bids_, id, trader, side, price, remaining, cancelled_by_stp);
    }

    res.filled = quantity - remaining;
    res.trade_count = trades_.size();

    if (cancelled_by_stp) {
        res.status = SubmitStatus::CancelledBySelfTrade;
        res.resting = 0;
        return res;
    }

    if (remaining > 0) {
        // Rest the remainder. Now — and only now — we materialise an Order.
        Order* order = pool_.acquire(id, trader, side, price, remaining);
        order->set_sequence(next_seq_++);

        if (side == Side::Buy) {
            auto [it, _] = bids_.try_emplace(price, price);
            it->second.enqueue(order);
        } else {
            auto [it, _] = asks_.try_emplace(price, price);
            it->second.enqueue(order);
        }
        index_.emplace(id, order);

        res.resting = remaining;
        res.status = res.filled > 0 ? SubmitStatus::PartiallyFilled
                                    : SubmitStatus::Rested;
    } else {
        res.status = SubmitStatus::FullyFilled;
        res.resting = 0;
    }
    return res;
}

// ---------------------------------------------------------------------------
// cancel — O(1) splice via the order's back-pointer to its level
// ---------------------------------------------------------------------------
bool OrderBook::cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) {
        return false;
    }
    Order* order = it->second;
    index_.erase(it);

    PriceLevel* level = order->level_;
    const Price level_price = level->price();
    const Side side = order->side();

    level->unlink(order); // O(1)
    const bool level_now_empty = level->empty();
    pool_.release(order);

    // Only touch the tree (O(log n)) when the whole level disappears.
    if (level_now_empty) {
        if (side == Side::Buy) {
            bids_.erase(level_price);
        } else {
            asks_.erase(level_price);
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Top-of-book / depth queries
// ---------------------------------------------------------------------------
std::optional<Price> OrderBook::best_bid() const {
    if (bids_.empty()) {
        return std::nullopt;
    }
    return bids_.begin()->first;
}

std::optional<Price> OrderBook::best_ask() const {
    if (asks_.empty()) {
        return std::nullopt;
    }
    return asks_.begin()->first;
}

Quantity OrderBook::quantity_at(Side side, Price price) const {
    if (side == Side::Buy) {
        auto it = bids_.find(price);
        return it == bids_.end() ? 0 : it->second.total_quantity();
    }
    auto it = asks_.find(price);
    return it == asks_.end() ? 0 : it->second.total_quantity();
}

} // namespace obe
