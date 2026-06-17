# order-book-engine

A low-latency **limit order book (LOB) matching engine** in modern C++17.

It implements strict **price-time priority** matching with full/partial fills,
cancellation, and self-trade prevention; a **free-list memory pool** to keep
order allocation off the heap on the hot path; and a **lock-free SPSC ring
buffer** decoupling an order-generator thread from the matching thread. It ships
with a GoogleTest suite, a throughput/latency benchmark, a parameter-sweep
script, a profiling write-up, a reproducible Docker build, and CI.

```
$ obe_bench --commands 1000000 --depth 20
--- latency (single-thread, ns/command) ---
mean   : 134   p50 : 83   p99 : 958   p99.9 : 1375
--- throughput (SPSC pipeline) ---
ops/sec: ~9.4 million
```

---

## Architecture

```
                 producer thread                      consumer thread
       ┌──────────────────────────┐         ┌─────────────────────────────────┐
       │      OrderGenerator       │         │          MatchingEngine          │
       │  (synthetic market flow)  │         │                                  │
       │                           │         │   pop()    ┌──────────────────┐  │
       │   next() -> Command  ─────┼──push()─▶│ ─────────▶│     OrderBook    │  │
       │                           │  lock-   │           │                  │  │
       └──────────────────────────┘  free    │           │  bids: map<Price,│  │
                                  SpscRingBuffer<Command> │       PriceLevel> │  │
                                   (std::atomic head/tail,│  asks: map<Price,│  │
                                    cache-line padded)    │       PriceLevel> │  │
                                                          │  index: hash      │  │
                                                          │       OrderId→*   │  │
                                                          │  pool: MemoryPool │  │
                                                          │       <Order>     │  │
                                                          └──────────────────┘  │
                                                          └─────────────────────┘

  Each PriceLevel is an intrusive FIFO list of Orders (time priority):

     PriceLevel(price=100)
        head ─▶ [Order A] ⇄ [Order B] ⇄ [Order C] ◀─ tail
                (oldest)                  (youngest)
```

**Data-flow.** The generator thread fabricates `Command`s (submit/cancel) and
pushes them across the lock-free queue. The engine thread pops them and applies
them to the `OrderBook`. The two threads share nothing but the ring buffer, and
the ring buffer's head/tail indices live on separate cache lines so the producer
and consumer never fight over a cache line (no false sharing).

**Matching core.** Inside `OrderBook`:

* **Two price-indexed trees** — `std::map<Price, PriceLevel>`, a red-black tree.
  Bids use `std::greater` and asks `std::less`, so `begin()` is always the best
  price on each side. `O(log n)` to insert/find/erase a price level; ordered
  iteration gives best-price-first matching for free.
* **Per-level FIFO** — each `PriceLevel` threads an *intrusive* doubly-linked
  list directly through the `Order` objects. Append-at-tail and pop-at-head are
  `O(1)`, and because linkage is intrusive, removing an arbitrary order (cancel)
  is an `O(1)` pointer splice — no search, no node allocation.
* **Id index** — `unordered_map<OrderId, Order*>` so cancel finds its order in
  `O(1)` average before splicing it out.
* **Memory pool** — `Order` objects are served from a `MemoryPool<Order>`
  free-list, so a steady-state run performs **zero heap allocations** for
  orders. It is the only component that touches raw allocation.

See [the design choices](#design-notes) below and the per-file header comments,
which explain the *why* behind each structure.

---

## Big-O complexity

`n` = number of distinct price levels on a side, `m` = orders at a price level,
`k` = price levels an aggressing order sweeps through.

| Operation | Complexity | Why |
|-----------|------------|-----|
| **Submit — rest (no match)** | `O(log n)` | Find/create the price level in the RB-tree (`O(log n)`), then `O(1)` FIFO append + `O(1)` avg index insert + `O(1)` pool acquire. |
| **Submit — match** | `O(k·log n + f)` | Each of the `k` levels consumed is the tree's `begin()`; erasing an emptied level is `O(log n)`. `f` = number of fills produced (each `O(1)`). |
| **Cancel** | `O(1)` average | `O(1)` index lookup → `O(1)` intrusive splice. Only when the splice empties a level do we pay `O(log n)` to erase that tree node. |
| **Best bid / best ask** | `O(1)` | `map::begin()` (cached leftmost in libstdc++/libc++). |
| **Quantity at a price** | `O(log n)` | One `map::find`; the per-level total is maintained incrementally, so it is not `O(m)`. |
| **PriceLevel enqueue / dequeue** | `O(1)` | Intrusive list head/tail pointer updates. |
| **MemoryPool acquire / release** | `O(1)` amortized | Pop/push a free-list node; geometric block growth amortizes the rare `operator new`. |
| **RingBuffer push / pop** | `O(1)` wait-free | Single atomic load + store with acquire/release ordering; power-of-two mask for wrap. |

---

## Build

Requires CMake ≥ 3.16 and a C++17 compiler. GoogleTest is fetched automatically.

```sh
# Release (-O3): the production / benchmark build
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

ctest --test-dir build --output-on-failure   # run the test suite
./build/obe_bench --commands 1000000          # run the benchmark
```

```sh
# Debug: AddressSanitizer + UndefinedBehaviorSanitizer
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure
```

### Docker

```sh
docker build -t obe .
docker run --rm obe                       # runs the test suite (default CMD)
docker run --rm obe ./build/obe_bench     # runs the benchmark
```

---

## Tests

`ctest` runs 29 tests across 7 suites:

| Suite | Covers |
|-------|--------|
| `PriceTimePriority` | best-price-first, FIFO within a level, multi-level sweep |
| `PartialFills` | resting partial, aggressor partial + rest, multi-maker, rejects |
| `Cancellation` | removal, level cleanup, mid-queue splice, pool reclaim, idempotency |
| `SelfTrade` | `CancelResting`, `CancelAggressing`, and `None` policies |
| `MemoryPool` | acquire/release, slot reuse, growth, distinctness |
| `RingBuffer` | FIFO, full/empty edges, power-of-two sizing, **2-thread** run |
| `Stress` | 200k randomized commands; invariants; **threaded == inline** equivalence |

The stress suite is the headline correctness check: it asserts the book is never
crossed, that no `Order` leaks the pool, and that running the identical command
stream through the lock-free pipeline yields bit-for-bit the same result as
single-threaded execution — proving the SPSC hand-off preserves ordering and
semantics.

---

## Benchmarks

`obe_bench` reports two things from a pre-generated command stream:

* **Latency** — single-threaded, a `steady_clock` measurement around each
  `apply()`; mean / p50 / p99 / p99.9 in ns/command.
* **Throughput** — end-to-end through the SPSC pipeline (producer + consumer
  threads); commands/sec over wall-clock.

`bench/sweep.sh` walks a grid of book depth × order volume and writes a CSV.
Representative results (Apple M2, clang 16, `-O3`):

| depth | commands | throughput (ops/s) | mean (ns) | p50 | p99 | p99.9 |
|------:|---------:|-------------------:|----------:|----:|----:|------:|
| 1 | 1,000,000 | 12.0 M | 76 | 42 | 333 | 1209 |
| 5 | 1,000,000 | 9.7 M | 96 | 83 | 500 | 1250 |
| 20 | 1,000,000 | 7.1 M | 135 | 83 | 958 | 1375 |
| 50 | 1,000,000 | 6.3 M | 173 | 84 | 1333 | 1916 |
| 100 | 1,000,000 | 5.6 M | 198 | 84 | 1750 | 2625 |
| 250 | 1,000,000 | 6.2 M | 246 | 125 | 2334 | 4583 |

The trend is the `O(log n)` price-level cost made visible: as the book deepens,
each match/insert touches more tree nodes (and larger working set / more cache
misses), so latency rises and throughput falls. At depth 1 (everything at one
price) the tree is trivial and the engine sustains ~12 M ops/s.

---

## Profiling

A full **measure → profile → fix → re-measure** cycle is documented in
[PROFILING.md](PROFILING.md). Summary: profiling revealed that the
`unordered_map` id-index was **rehashing during the run**, dominating `submit()`.
Reserving the table up front (one line) removed the rehash and cut mean latency
**~11%** with a tighter p99 tail. The write-up also names the next remaining
hotspot (per-insert node allocation inside the hash map) as a documented
follow-up.

---

## Design notes

* **Integer prices.** Prices are `int64_t` ticks, never floating point — float
  rounding silently breaks price-time priority and equality.
* **Intrusive FIFO.** Threading the queue through the `Order` itself is what
  makes cancel `O(1)` and avoids a per-order list-node allocation.
* **Taker needs no allocation.** A marketable order is matched using locals; an
  `Order` object is materialized from the pool *only* if a remainder must rest,
  sparing an acquire/release on the fully-filled common path.
* **RAII / no raw new-delete.** The `MemoryPool` is the sole owner of raw
  storage (`operator new`/`delete` with proper alignment, freed in its
  destructor). Everything else is value types and owning containers.
* **Self-trade prevention** is pluggable (`SelfTradePolicy`): cancel the resting
  order, cancel the aggressor's remainder, or allow the trade.

## Project layout

```
include/obe/     public headers (one component each, heavily commented)
  Types.hpp        domain primitives (Price, Side, STP policy)
  Order.hpp        order = intrusive FIFO node
  PriceLevel.hpp   O(1) intrusive FIFO queue at one price
  OrderBook.hpp    the matching book (RB-trees + index + pool)
  Trade.hpp        execution report / submit result types
  Command.hpp      producer→consumer message
  MemoryPool.hpp   free-list allocator
  RingBuffer.hpp   lock-free SPSC queue
  MatchingEngine.hpp  consumer thread + book
  OrderGenerator.hpp  synthetic flow
src/             out-of-line implementations
tests/           GoogleTest suites (one file per category)
bench/           benchmark.cpp + sweep.sh
Dockerfile       reproducible Linux build/test/profile image
.github/workflows/ci.yml   Release tests + ASan/UBSan on every push
SYNTHETIC_DATA.md  how the order generator models market flow
PROFILING.md       the profiling pass with before/after numbers
```
