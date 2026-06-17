// Verifies strict price-time priority: better prices trade first, and within a
// price level the oldest resting order trades first (FIFO).

#include "obe/OrderBook.hpp"

#include <gtest/gtest.h>

using namespace obe;

namespace {
// STP off so we can use a single trader and isolate priority behaviour.
OrderBook make_book() { return OrderBook(SelfTradePolicy::None); }
} // namespace

TEST(PriceTimePriority, TimePriorityWithinLevel) {
    OrderBook book = make_book();
    // Two resting buys at the same price; id=1 arrived first.
    book.submit(1, 1, Side::Buy, 100, 10);
    book.submit(2, 1, Side::Buy, 100, 10);

    // A sell that can fill exactly one of them must hit the oldest (id=1).
    const SubmitResult r = book.submit(3, 1, Side::Sell, 100, 10);

    ASSERT_EQ(r.trade_count, 1u);
    EXPECT_EQ(book.last_trades()[0].maker_id, 1u);
    EXPECT_EQ(book.last_trades()[0].quantity, 10u);
    // id=2 still rests.
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 10u);
    EXPECT_EQ(book.order_count(), 1u);
}

TEST(PriceTimePriority, BetterPriceTradesFirst) {
    OrderBook book = make_book();
    book.submit(1, 1, Side::Buy, 100, 10); // worse for a seller
    book.submit(2, 1, Side::Buy, 101, 10); // best bid

    // Seller crossing at 100 should hit the highest bid (101) first.
    const SubmitResult r = book.submit(3, 1, Side::Sell, 100, 5);

    ASSERT_EQ(r.trade_count, 1u);
    EXPECT_EQ(book.last_trades()[0].maker_id, 2u);
    EXPECT_EQ(book.last_trades()[0].price, 101); // fills at the maker's price
    EXPECT_EQ(book.quantity_at(Side::Buy, 101), 5u);
    EXPECT_EQ(book.quantity_at(Side::Buy, 100), 10u);
}

TEST(PriceTimePriority, AggressorSweepsMultipleLevelsInOrder) {
    OrderBook book = make_book();
    book.submit(1, 1, Side::Sell, 100, 5);
    book.submit(2, 1, Side::Sell, 101, 5);
    book.submit(3, 1, Side::Sell, 102, 5);

    // Big buy crossing through all three levels.
    const SubmitResult r = book.submit(4, 1, Side::Buy, 102, 15);

    ASSERT_EQ(r.trade_count, 3u);
    EXPECT_EQ(r.filled, 15u);
    // Trades print in ascending ask price order (best first).
    EXPECT_EQ(book.last_trades()[0].price, 100);
    EXPECT_EQ(book.last_trades()[1].price, 101);
    EXPECT_EQ(book.last_trades()[2].price, 102);
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_EQ(book.order_count(), 0u);
}

TEST(PriceTimePriority, NonCrossingOrderRestsAndDoesNotTrade) {
    OrderBook book = make_book();
    book.submit(1, 1, Side::Buy, 99, 10);
    const SubmitResult r = book.submit(2, 1, Side::Sell, 100, 10);

    EXPECT_EQ(r.trade_count, 0u);
    EXPECT_EQ(r.status, SubmitStatus::Rested);
    EXPECT_EQ(book.best_bid(), 99);
    EXPECT_EQ(book.best_ask(), 100);
}
