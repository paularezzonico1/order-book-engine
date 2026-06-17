// Verifies cancellation: removal of a resting order, level cleanup, idempotent
// behaviour on unknown ids, and that pooled memory is reclaimed.

#include "obe/OrderBook.hpp"

#include <gtest/gtest.h>

using namespace obe;

TEST(Cancellation, CancelRemovesRestingOrder) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, 1, Side::Buy, 100, 10);
    ASSERT_EQ(book.best_bid(), 100);

    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.bid_levels(), 0u); // emptied level is erased from the tree
}

TEST(Cancellation, CancelUnknownIdReturnsFalse) {
    OrderBook book(SelfTradePolicy::None);
    EXPECT_FALSE(book.cancel(42));
    book.submit(1, 1, Side::Buy, 100, 10);
    EXPECT_TRUE(book.cancel(1));
    EXPECT_FALSE(book.cancel(1)); // already gone
}

TEST(Cancellation, CancelOneOfTwoKeepsLevelAndPriority) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, 1, Side::Buy, 100, 10);
    book.submit(2, 1, Side::Buy, 100, 10);

    EXPECT_TRUE(book.cancel(1)); // cancel the older order
    EXPECT_EQ(book.bid_levels(), 1u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 10u);

    // The remaining order (id=2) is now front and should trade.
    const SubmitResult r = book.submit(3, 1, Side::Sell, 100, 10);
    ASSERT_EQ(r.trade_count, 1u);
    EXPECT_EQ(book.last_trades()[0].maker_id, 2u);
}

TEST(Cancellation, CancelMiddleOfQueue) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, 1, Side::Buy, 100, 5);
    book.submit(2, 1, Side::Buy, 100, 5);
    book.submit(3, 1, Side::Buy, 100, 5);

    EXPECT_TRUE(book.cancel(2)); // splice out the middle node
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 10u);

    // Remaining FIFO order must be 1 then 3.
    book.submit(4, 1, Side::Sell, 100, 10);
    ASSERT_EQ(book.last_trades().size(), 2u);
    EXPECT_EQ(book.last_trades()[0].maker_id, 1u);
    EXPECT_EQ(book.last_trades()[1].maker_id, 3u);
}

TEST(Cancellation, PoolReclaimsMemoryAfterCancel) {
    OrderBook book(SelfTradePolicy::None);
    for (OrderId i = 1; i <= 100; ++i) {
        book.submit(i, 1, Side::Buy, 100 + static_cast<Price>(i), 1);
    }
    EXPECT_EQ(book.pool().live_count(), 100u);
    for (OrderId i = 1; i <= 100; ++i) {
        EXPECT_TRUE(book.cancel(i));
    }
    EXPECT_EQ(book.pool().live_count(), 0u);
    EXPECT_EQ(book.order_count(), 0u);
}
