#include "obe/OrderGenerator.hpp"

#include <algorithm>
#include <cmath>

namespace obe {

OrderGenerator::OrderGenerator(GeneratorConfig cfg)
    : cfg_(cfg), rng_(cfg.seed), mid_(cfg.initial_mid) {
    live_ids_.reserve(1u << 16);
}

void OrderGenerator::random_walk_mid() {
    std::bernoulli_distribution moves(cfg_.mid_move_prob);
    if (moves(rng_)) {
        std::bernoulli_distribution up(0.5);
        mid_ += up(rng_) ? cfg_.tick : -cfg_.tick;
        if (mid_ < cfg_.tick) {
            mid_ = cfg_.tick; // keep prices positive
        }
    }
}

Quantity OrderGenerator::draw_quantity() {
    // Exponential sizes capture the heavy tail of real order sizes: many small
    // orders, a few very large ones.
    std::exponential_distribution<double> dist(1.0 / cfg_.mean_qty);
    double q = std::round(dist(rng_)) + 1.0; // ensure >= 1
    Quantity qty = static_cast<Quantity>(q);
    return std::min(qty, cfg_.max_qty);
}

Command OrderGenerator::make_submit() {
    random_walk_mid();

    std::bernoulli_distribution buy(0.5);
    const Side side = buy(rng_) ? Side::Buy : Side::Sell;

    std::uniform_int_distribution<std::uint32_t> trader_dist(0, cfg_.num_traders - 1);
    const TraderId trader = trader_dist(rng_);

    const Quantity qty = draw_quantity();

    // Distance from the mid: a geometric distribution concentrates orders at
    // the touch and thins out into the book — the classic depth profile.
    std::geometric_distribution<int> offset_dist(2.0 / (cfg_.depth_levels + 1));
    int offset = std::min(offset_dist(rng_), cfg_.depth_levels) ;

    std::bernoulli_distribution marketable(cfg_.marketable_prob);
    Price price;
    if (marketable(rng_)) {
        // Cross the spread to (likely) trade: a buy reaches up, a sell reaches
        // down by a few ticks past the mid.
        price = side == Side::Buy ? mid_ + offset * cfg_.tick
                                  : mid_ - offset * cfg_.tick;
    } else {
        // Passive: rest on its own side, away from the mid.
        price = side == Side::Buy ? mid_ - offset * cfg_.tick
                                  : mid_ + offset * cfg_.tick;
    }
    if (price < cfg_.tick) {
        price = cfg_.tick;
    }

    const OrderId id = next_id_++;
    live_ids_.push_back(id);
    return Command::make_submit(id, trader, side, price, qty);
}

Command OrderGenerator::make_cancel() {
    // Pick a random live id and remove it from the tracking set (swap-and-pop
    // for O(1) removal). The book treats an already-gone id as a no-op.
    std::uniform_int_distribution<std::size_t> pick(0, live_ids_.size() - 1);
    const std::size_t idx = pick(rng_);
    const OrderId id = live_ids_[idx];
    live_ids_[idx] = live_ids_.back();
    live_ids_.pop_back();
    return Command::make_cancel(id);
}

Command OrderGenerator::next() {
    std::bernoulli_distribution do_cancel(cfg_.cancel_ratio);
    if (!live_ids_.empty() && do_cancel(rng_)) {
        return make_cancel();
    }
    return make_submit();
}

} // namespace obe
