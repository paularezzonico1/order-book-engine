// Verifies full fills, partial fills of the resting order, and partial fills of
// the aggressing order (remainder resting).

#include "obe/OrderBook.hpp"

#include <gtest/gtest.h>

using namespace obe;

TEST(PartialFills, RestingOrderPartiallyFilled) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, 1, Side::Buy, 100, 10);

    // Sell only 4 -> resting buy keeps 6.
    const SubmitResult r = book.submit(2, 1, Side::Sell, 100, 4);

    EXPECT_EQ(r.status, SubmitStatus::FullyFilled); // the aggressor filled fully
    EXPECT_EQ(r.filled, 4u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 6u);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(PartialFills, AggressorPartiallyFilledRestsRemainder) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, 1, Side::Buy, 100, 10);

    // Sell 25: 10 trades, 15 rests as the new best ask.
    const SubmitResult r = book.submit(2, 1, Side::Sell, 100, 25);

    EXPECT_EQ(r.status, SubmitStatus::PartiallyFilled);
    EXPECT_EQ(r.filled, 10u);
    EXPECT_EQ(r.resting, 15u);
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_EQ(book.best_ask(), 100);
    EXPECT_EQ(book.quantity_at(Side::Sell, 100), 15u);
}

TEST(PartialFills, AggressorFillsAcrossMultipleMakers) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, 1, Side::Buy, 100, 6);
    book.submit(2, 1, Side::Buy, 100, 6);

    // Sell 9: fully fills id=1 (6) then partially fills id=2 (3).
    const SubmitResult r = book.submit(3, 1, Side::Sell, 100, 9);

    ASSERT_EQ(r.trade_count, 2u);
    EXPECT_EQ(book.last_trades()[0].maker_id, 1u);
    EXPECT_EQ(book.last_trades()[0].quantity, 6u);
    EXPECT_EQ(book.last_trades()[1].maker_id, 2u);
    EXPECT_EQ(book.last_trades()[1].quantity, 3u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 3u); // id=2 has 3 left
}

TEST(PartialFills, ExactFullFillLeavesEmptyBook) {
    OrderBook book(SelfTradePolicy::None);
    book.submit(1, 1, Side::Sell, 100, 10);
    const SubmitResult r = book.submit(2, 1, Side::Buy, 100, 10);

    EXPECT_EQ(r.status, SubmitStatus::FullyFilled);
    EXPECT_EQ(r.resting, 0u);
    EXPECT_EQ(book.order_count(), 0u);
    EXPECT_EQ(book.pool().live_count(), 0u); // no Order leaked
}

TEST(PartialFills, RejectsZeroQuantityAndDuplicateId) {
    OrderBook book(SelfTradePolicy::None);
    EXPECT_EQ(book.submit(1, 1, Side::Buy, 100, 0).status, SubmitStatus::Rejected);

    book.submit(2, 1, Side::Buy, 100, 5);
    EXPECT_EQ(book.submit(2, 1, Side::Buy, 100, 5).status, SubmitStatus::Rejected);
}
