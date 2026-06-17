# Synthetic Order Flow

The benchmark and stress tests are driven by `OrderGenerator`
(`include/obe/OrderGenerator.hpp`, `src/OrderGenerator.cpp`), a deterministic
generator that produces a stream of `Command`s (submit / cancel) approximating
the statistical shape of real equity market order flow.

The goal is **not** a faithful market simulator — it is to exercise the engine
along the dimensions that actually stress a matching core: tree depth, FIFO
queue length, fill vs. rest vs. cancel mix, and self-trade handling.

## Why synthetic (and not a captured tape)?

* **Reproducibility.** Everything is driven by a single seeded `std::mt19937_64`.
  Given a seed, the stream is identical on every machine and every run, so
  benchmark numbers and the threaded/inline equivalence test are deterministic.
* **Controllability.** A few knobs (`GeneratorConfig`) let the sweep walk the
  parameter space — e.g. book depth — to expose the `O(log n)` price-level cost.
* **No licensing/PII concerns.** Real tick data is encumbered; synthetic flow
  is free to commit and share.

## The model

Each call to `next()` emits one command. With probability `cancel_ratio` (and
only if there are live orders) it is a **cancel** of a randomly chosen
outstanding order; otherwise it is a **submit**. Real venues see far more
cancels than fills, so a meaningful cancel rate (default 20%) keeps the book
churning the way a live book does.

A submit is built from these independent draws:

| Attribute | Distribution | Rationale |
|-----------|--------------|-----------|
| **Mid price** | Random walk: ±1 tick with prob. `mid_move_prob` per submit | The reference price drifts over time, so resting liquidity spans a moving range of price levels rather than piling onto a single key — this is what makes the `std::map` actually grow and exercises `O(log n)`. |
| **Side** | Fair coin (Buy/Sell) | Balanced two-sided flow. |
| **Trader** | Uniform over `num_traders` | Multiple participants so self-trade prevention genuinely fires (with one trader every cross would be a self-trade). |
| **Quantity** | Exponential, mean `mean_qty`, clamped to `[1, max_qty]` | Order sizes are heavy-tailed in reality: many small orders, a few large ones. The exponential reproduces that skew. |
| **Price offset from mid** | Geometric, capped at `depth_levels` | Liquidity concentrates at the touch and thins deeper into the book — the classic depth profile. Geometric gives that exponential fall-off. |
| **Marketable?** | Bernoulli, prob. `marketable_prob` | A fraction of orders are priced *through* the spread so they trade immediately (generating fills and partial fills); the rest rest passively. |

For a **marketable** order the price reaches across the mid (a buy bids up, a
sell offers down), so it crosses resting liquidity and produces trades. For a
**passive** order the price sits on its own side, away from the mid, and rests.

## Tuning knobs (`GeneratorConfig`)

| Field | Default | Effect |
|-------|---------|--------|
| `seed` | golden-ratio constant | PRNG seed; fixes the whole stream. |
| `initial_mid` | `10000` (=$100.00) | Starting mid price, in ticks. |
| `tick` | `1` | Minimum price increment. |
| `depth_levels` | `50` | Spread of liquidity around the mid → tree size. |
| `marketable_prob` | `0.25` | Fraction of submits that cross the spread. |
| `cancel_ratio` | `0.20` | Fraction of commands that are cancels. |
| `mean_qty` / `max_qty` | `20` / `1000` | Order-size distribution. |
| `num_traders` | `16` | Distinct participants (STP). |
| `mid_move_prob` | `0.05` | Volatility of the mid random walk. |

The benchmark exposes `--depth`, `--commands`, and `--seed`; `bench/sweep.sh`
varies `depth` and `commands` to map throughput/latency across the grid.

## Live-id tracking

To make cancels target real orders, the generator keeps a vector of ids it has
submitted and pops a random one (swap-and-pop, `O(1)`) when it issues a cancel.
This is intentionally approximate — an id may already have been filled, in which
case the book treats the cancel as a harmless no-op — which itself mirrors the
real race between a cancel in flight and a fill.
