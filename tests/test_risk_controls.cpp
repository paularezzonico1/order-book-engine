// Unit tests for the pre-trade risk controls, plus an engine-integration test
// proving a rejected order never reaches the book.

#include "obe/MatchingEngine.hpp"
#include "obe/RiskControls.hpp"

#include <gtest/gtest.h>
#include <optional>

using namespace obe;

TEST(RiskControls, MaxOrderSize) {
    RiskConfig cfg;
    cfg.max_order_size = 100;
    RiskManager rm(cfg);
    EXPECT_EQ(rm.check(1, Side::Buy, 100, 100, std::nullopt), RejectReason::Accepted);
    EXPECT_EQ(rm.check(2, Side::Buy, 100, 101, std::nullopt), RejectReason::MaxOrderSize);
    EXPECT_EQ(rm.metrics().accepted, 1u);
    EXPECT_EQ(rm.metrics().max_order_size, 1u);
}

TEST(RiskControls, MaxNotional) {
    RiskConfig cfg;
    cfg.max_notional = 1'000'000; // price*qty cap
    RiskManager rm(cfg);
    EXPECT_EQ(rm.check(1, Side::Buy, 100, 10'000, std::nullopt), RejectReason::Accepted);   // 1,000,000
    EXPECT_EQ(rm.check(2, Side::Buy, 100, 10'001, std::nullopt), RejectReason::MaxNotional); // 1,000,100
}

TEST(RiskControls, FatFingerCollarBuyAndSell) {
    RiskConfig cfg;
    cfg.collar_bps = 100; // 1% band around the reference
    RiskManager rm(cfg);

    // Reference 10000: a buy up to 10100 is fine, 10101 is a fat finger.
    EXPECT_EQ(rm.check(1, Side::Buy, 10100, 1, std::optional<Price>(10000)), RejectReason::Accepted);
    EXPECT_EQ(rm.check(2, Side::Buy, 10101, 1, std::optional<Price>(10000)), RejectReason::PriceCollar);
    // A sell down to 9900 is fine, 9899 trips the collar.
    EXPECT_EQ(rm.check(3, Side::Sell, 9900, 1, std::optional<Price>(10000)), RejectReason::Accepted);
    EXPECT_EQ(rm.check(4, Side::Sell, 9899, 1, std::optional<Price>(10000)), RejectReason::PriceCollar);
    // No reference => collar cannot anchor, so it is skipped.
    EXPECT_EQ(rm.check(5, Side::Buy, 999999, 1, std::nullopt), RejectReason::Accepted);
}

TEST(RiskControls, KillSwitchHaltsAllOrders) {
    RiskManager rm; // permissive config
    EXPECT_FALSE(rm.kill_switch_engaged());
    EXPECT_EQ(rm.check(1, Side::Buy, 100, 1, std::nullopt), RejectReason::Accepted);

    rm.engage_kill_switch(true);
    EXPECT_TRUE(rm.kill_switch_engaged());
    EXPECT_EQ(rm.check(2, Side::Buy, 100, 1, std::nullopt), RejectReason::KillSwitch);
    EXPECT_EQ(rm.check(3, Side::Sell, 100, 1, std::nullopt), RejectReason::KillSwitch);

    rm.engage_kill_switch(false);
    EXPECT_EQ(rm.check(4, Side::Buy, 100, 1, std::nullopt), RejectReason::Accepted);
    EXPECT_EQ(rm.metrics().kill_switch, 2u);
}

TEST(RiskControls, DuplicateOrderIdRejected) {
    RiskManager rm; // enforce_unique_ids defaults true
    EXPECT_EQ(rm.check(7, Side::Buy, 100, 1, std::nullopt), RejectReason::Accepted);
    EXPECT_EQ(rm.check(7, Side::Buy, 100, 1, std::nullopt), RejectReason::DuplicateOrder);
    EXPECT_EQ(rm.metrics().duplicate, 1u);
}

TEST(RiskControls, DefaultConfigOnlyEnforcesUniqueIds) {
    RiskManager rm; // all limits disabled
    EXPECT_EQ(rm.check(1, Side::Buy, 1'000'000, 1'000'000, std::optional<Price>(1)),
              RejectReason::Accepted); // huge order, far price -> still fine
}

TEST(RiskControls, EngineRejectsFatFingerBeforeBook) {
    MatchingEngine engine;
    RiskConfig cfg;
    cfg.collar_bps = 50; // 0.5%
    engine.enable_risk(cfg);

    // Seed a resting ask at 10000 so the collar has a reference (best ask).
    engine.apply(Command::make_submit(1, 1, Side::Sell, 10000, 10));

    // A buy at 11000 is ~10% through the ask -> rejected, never reaches book.
    const std::size_t trades =
        engine.apply(Command::make_submit(2, 2, Side::Buy, 11000, 10));
    EXPECT_EQ(trades, 0u);
    EXPECT_EQ(engine.book().order_count(), 1u);          // only the resting ask
    EXPECT_EQ(engine.risk()->metrics().price_collar, 1u);
    EXPECT_EQ(engine.stats().submits, 1u);               // the rejected one didn't count
}
