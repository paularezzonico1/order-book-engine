#pragma once

#include "obe/Order.hpp"
#include "obe/Types.hpp"

#include <cstddef>

namespace obe {

// ---------------------------------------------------------------------------
// PriceLevel
// ---------------------------------------------------------------------------
//
// All resting orders at one price, held as a FIFO intrusive doubly-linked
// list. FIFO is the mechanical realisation of *time priority*: the order at the
// front is the oldest and must trade first.
//
// Every mutating operation is O(1):
//   * enqueue      — append at tail (new orders are always youngest)
//   * front/pop    — read/remove at head (oldest trades first)
//   * unlink       — splice out an arbitrary order given its node (cancel)
//
// The list threads through Order::prev_/next_; PriceLevel owns that linkage but
// not the Order storage itself (that belongs to the MemoryPool).
//
// Non-movable: Orders hold a back-pointer (level_) to their owning level, so
// the object must stay put once created. It is therefore constructed in place
// inside the std::map node via try_emplace and never relocated.
class PriceLevel {
public:
    explicit PriceLevel(Price price) noexcept : price_(price) {}

    PriceLevel(const PriceLevel&) = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;
    PriceLevel(PriceLevel&&) = delete;
    PriceLevel& operator=(PriceLevel&&) = delete;

    Price price() const noexcept { return price_; }
    bool empty() const noexcept { return head_ == nullptr; }
    std::size_t order_count() const noexcept { return count_; }

    // Aggregate resting quantity at this level (sum of remaining_). Maintained
    // incrementally so depth queries are O(1) instead of O(orders).
    Quantity total_quantity() const noexcept { return total_qty_; }

    Order* front() const noexcept { return head_; }

    // Append an order at the tail (youngest). O(1).
    void enqueue(Order* order) noexcept {
        order->level_ = this;
        order->prev_ = tail_;
        order->next_ = nullptr;
        if (tail_ != nullptr) {
            tail_->next_ = order;
        } else {
            head_ = order;
        }
        tail_ = order;
        ++count_;
        total_qty_ += order->remaining();
    }

    // Remove an arbitrary order from the list given its node. O(1).
    // Used both by cancellation and when the head order is fully filled.
    void unlink(Order* order) noexcept {
        if (order->prev_ != nullptr) {
            order->prev_->next_ = order->next_;
        } else {
            head_ = order->next_;
        }
        if (order->next_ != nullptr) {
            order->next_->prev_ = order->prev_;
        } else {
            tail_ = order->prev_;
        }
        order->prev_ = order->next_ = nullptr;
        order->level_ = nullptr;
        --count_;
        total_qty_ -= order->remaining();
    }

    // Record that `traded` units were filled from `order` (already deducted
    // from the order's remaining) so the level aggregate stays consistent.
    void on_fill(Quantity traded) noexcept { total_qty_ -= traded; }

private:
    Price price_;
    Order* head_ = nullptr; // oldest — trades first
    Order* tail_ = nullptr; // youngest — appended last
    std::size_t count_ = 0;
    Quantity total_qty_ = 0;
};

} // namespace obe
