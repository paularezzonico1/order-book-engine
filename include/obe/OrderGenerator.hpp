#pragma once

#include "obe/Command.hpp"
#include "obe/Types.hpp"

#include <cstddef>
#include <cstdint>
#include <random>
#include <vector>

namespace obe {

// ---------------------------------------------------------------------------
// OrderGenerator — synthetic but market-realistic order flow
// ---------------------------------------------------------------------------
//
// Produces a reproducible stream of Commands modelling the salient features of
// real equity order flow: a mid price that random-walks, a depth profile that
// concentrates liquidity near the touch, occasionally marketable (crossing)
// orders that generate trades, heavy-tailed order sizes, multiple participants
// (so self-trade prevention actually fires), and a realistic cancel rate (most
// real orders are cancelled, not filled).
//
// See SYNTHETIC_DATA.md for the modelling rationale. Everything is driven by a
// seeded PRNG so benchmarks are deterministic and comparable run-to-run.
struct GeneratorConfig {
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;

    Price initial_mid = 100'00; // mid price in ticks (here: $100.00, tick=1c)
    Price tick = 1;

    // Liquidity within ~depth_levels ticks of the mid; sizing of the tail.
    int depth_levels = 50;

    // Probability a submitted order is priced to cross the spread (-> a trade).
    double marketable_prob = 0.25;

    // Fraction of commands that are cancels of a live resting order.
    double cancel_ratio = 0.20;

    // Heavy-tailed size: exponential with this mean, clamped to [1, max_qty].
    double mean_qty = 20.0;
    Quantity max_qty = 1000;

    // Distinct participants; STP can only trigger with > 1.
    TraderId num_traders = 16;

    // Mid-price random walk: each step moves +/- 1 tick with this probability.
    double mid_move_prob = 0.05;
};

class OrderGenerator {
public:
    explicit OrderGenerator(GeneratorConfig cfg = {});

    // Produce the next command in the stream.
    Command next();

    Price mid() const noexcept { return mid_; }
    const GeneratorConfig& config() const noexcept { return cfg_; }

private:
    Command make_submit();
    Command make_cancel();
    void random_walk_mid();
    Quantity draw_quantity();

    GeneratorConfig cfg_;
    std::mt19937_64 rng_;
    Price mid_;
    OrderId next_id_ = 1;

    // Ids believed to be resting, so cancels target real orders. This is an
    // approximation (an id may have filled meanwhile) which is fine: cancel of
    // an unknown id is a harmless no-op in the book.
    std::vector<OrderId> live_ids_;
};

} // namespace obe
