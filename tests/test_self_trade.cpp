// Verifies self-trade prevention under each policy.

#include "obe/OrderBook.hpp"

#include <gtest/gtest.h>

using namespace obe;

TEST(SelfTrade, CancelRestingRemovesOwnOrderAndDoesNotTrade) {
    OrderBook book(SelfTradePolicy::CancelResting);
    book.submit(1, /*trader=*/7, Side::Buy, 100, 10);

    // Same trader crosses their own bid: the resting order is cancelled, no
    // trade prints, and the aggressor rests instead.
    const SubmitResult r = book.submit(2, /*trader=*/7, Side::Sell, 100, 10);

    EXPECT_EQ(r.trade_count, 0u);
    EXPECT_FALSE(book.best_bid().has_value()); // resting buy cancelled
    EXPECT_EQ(book.best_ask(), 100);           // aggressor now rests as ask
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(SelfTrade, CancelRestingSkipsOwnButTradesWithOthers) {
    OrderBook book(SelfTradePolicy::CancelResting);
    book.submit(1, /*trader=*/7, Side::Buy, 100, 10); // own (will be cancelled)
    book.submit(2, /*trader=*/9, Side::Buy, 100, 10); // other (tradeable)

    const SubmitResult r = book.submit(3, /*trader=*/7, Side::Sell, 100, 10);

    ASSERT_EQ(r.trade_count, 1u);
    EXPECT_EQ(book.last_trades()[0].maker_id, 2u); // traded with trader 9
    EXPECT_EQ(book.last_trades()[0].maker_trader, 9u);
    EXPECT_EQ(book.order_count(), 0u); // own cancelled, other fully filled
}

TEST(SelfTrade, CancelAggressingDropsRemainder) {
    OrderBook book(SelfTradePolicy::CancelAggressing);
    book.submit(1, /*trader=*/9, Side::Buy, 100, 4);  // other -> tradeable
    book.submit(2, /*trader=*/7, Side::Buy, 100, 10); // own -> blocks aggressor

    // Aggressor (trader 7) sells 10: trades 4 with trader 9, then hits its own
    // resting buy -> remainder is cancelled, nothing rests.
    const SubmitResult r = book.submit(3, /*trader=*/7, Side::Sell, 100, 10);

    EXPECT_EQ(r.status, SubmitStatus::CancelledBySelfTrade);
    EXPECT_EQ(r.filled, 4u);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_FALSE(book.best_ask().has_value());        // aggressor did not rest
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 10u); // own order untouched
}

TEST(SelfTrade, PolicyNoneAllowsSelfTrade) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, /*trader=*/7, Side::Buy, 100, 10);
    const SubmitResult r = book.submit(2, /*trader=*/7, Side::Sell, 100, 10);

    ASSERT_EQ(r.trade_count, 1u);
    EXPECT_EQ(book.last_trades()[0].taker_trader, 7u);
    EXPECT_EQ(book.last_trades()[0].maker_trader, 7u);
}
