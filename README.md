# order-book-engine

A low-latency **limit order book (LOB) matching engine** in modern C++17.

It implements strict **price-time priority** matching with full/partial fills,
cancellation, and self-trade prevention; a **free-list memory pool** and an
**allocation-free open-addressing order index** to keep the hot path off the
heap; a **lock-free SPSC ring buffer** decoupling an order-generator thread from
the matching thread; **pre-trade risk controls** (size/notional/fat-finger
collar/kill switch/duplicate guard); and **event sourcing** — every command is
journaled to an append-only log and the whole session can be replayed bit-for-
bit. It ships with a GoogleTest suite (58 tests), a throughput/latency
benchmark, a parameter sweep, profiling write-ups, a reproducible Docker build,
and CI that runs the suite under **AddressSanitizer/UBSan and ThreadSanitizer**.

```
$ obe_bench --commands 1000000 --depth 20
--- latency (single-thread, ns/command) ---
mean   : ~88   p50 : 83   p99 : ~417   p99.9 : ~1250
--- throughput (SPSC pipeline) ---
ops/sec: ~10.6 million        # after the allocation-free index (see INDEX.md)
```

---

## Architecture

```
            producer thread                         consumer thread
   ┌──────────────────────────┐         ┌──────────────────────────────────────┐
   │      OrderGenerator       │         │            MatchingEngine             │
   │  (synthetic market flow)  │         │                                       │
   │                           │ push()  │  pop()  ┌────────────┐  ┌──────────┐  │
   │  next() -> Command  ──────┼────────▶│ ───────▶│ RiskGate   │─▶│ OrderBook │  │
   │                           │  lock-  │         │ size/notnl │  │           │  │
   └──────────────────────────┘  free   │         │ collar/dup │  │ bids/asks │  │
                          SpscRingBuffer<Command>  │ kill-switch│  │  : map    │  │
                          (atomic head/tail,       └────────────┘  │ index:    │  │
                           cache-line padded)            │         │  FlatHash │  │
                                                         ▼         │ pool:     │  │
                                                  ┌────────────┐   │  MemPool  │  │
                                                  │  EventLog   │  └──────────┘  │
                                                  │ (append-only│                │
                                                  │  journal)   │   atomic stats │
                                                  └────────────┘                 │
                                                  shutdown via Command sentinel  │
                                                  └────────────────────────────┘
  Each PriceLevel is an intrusive FIFO list of Orders (time priority):

     PriceLevel(price=100)
        head ─▶ [Order A] ⇄ [Order B] ⇄ [Order C] ◀─ tail
                (oldest)                  (youngest)
```

**Data-flow.** The generator thread fabricates `Command`s and pushes them across
the lock-free queue. The engine thread pops each one, runs it through the
(optional) pre-trade **risk gate**, journals it to the **event log**, and applies
it to the `OrderBook`. The two threads share nothing but the ring buffer, whose
head/tail indices live on separate cache lines (no false sharing). Shutdown is a
**data-driven sentinel** pushed through the same queue, so the consumer drains
every prior command before exiting — nothing is dropped. The engine's counters
are `std::atomic`, so `stats()` is race-free (verified under ThreadSanitizer).

**Matching core.** Inside `OrderBook`:

* **Two price-indexed trees** — `std::map<Price, PriceLevel>`, a red-black tree.
  Bids use `std::greater` and asks `std::less`, so `begin()` is always the best
  price on each side. `O(log n)` to insert/find/erase a price level; ordered
  iteration gives best-price-first matching for free.
* **Per-level FIFO** — each `PriceLevel` threads an *intrusive* doubly-linked
  list directly through the `Order` objects. Append-at-tail and pop-at-head are
  `O(1)`, and because linkage is intrusive, removing an arbitrary order (cancel)
  is an `O(1)` pointer splice — no search, no node allocation.
* **Id index** — an **allocation-free open-addressing `FlatHashMap<OrderId,
  Order*>`** so cancel finds its order in `O(1)` average with zero heap traffic.
  Replacing `std::unordered_map` here roughly halved mean latency and cut p99 by
  ~3–4× (see [INDEX.md](INDEX.md)).
* **Memory pool** — `Order` objects are served from a `MemoryPool<Order>`
  free-list, so a steady-state run performs **zero heap allocations** for
  orders. It is the only component that touches raw allocation.

**Pre-trade risk** ([RiskControls.hpp](include/obe/RiskControls.hpp)): an
optional gate in front of the book enforcing max order size, max notional, a
fat-finger price collar, a session duplicate-id guard, and a cross-thread
**kill switch**, with categorised rejection metrics — modelling the SEC 15c3-5
market-access checks a real broker must run.

**Event sourcing** ([EVENT_SOURCING.md](EVENT_SOURCING.md)): every applied
command is appended to a binary journal; a fresh engine replays the journal to
reconstruct the session **bit-for-bit** (proven by test, not asserted).

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
| **Cancel** | `O(1)` average | `O(1)` flat-hash index lookup → `O(1)` intrusive splice. Only when the splice empties a level do we pay `O(log n)` to erase that tree node. |
| **Pre-trade risk check** | `O(1)` average | Constant comparisons + one `O(1)` avg duplicate-id set lookup. |
| **Index find / insert / erase** | `O(1)` average | Open-addressing `FlatHashMap`, linear probing + backward-shift delete; no allocation. |
| **Best bid / best ask** | `O(1)` | `map::begin()` (cached leftmost in libstdc++/libc++). |
| **Quantity at a price** | `O(log n)` | One `map::find`; the per-level total is maintained incrementally, so it is not `O(m)`. |
| **PriceLevel enqueue / dequeue** | `O(1)` | Intrusive list head/tail pointer updates. |
| **MemoryPool acquire / release** | `O(1)` amortized | Pop/push a free-list node; geometric block growth amortizes the rare `operator new`. |
| **RingBuffer push / pop** | `O(1)` wait-free | Single atomic load + store with acquire/release ordering; power-of-two mask for wrap. |
| **Event log append** | `O(1)` | Fixed-width record appended to a buffered stream. |
| **Snapshot / replay** | `O(orders)` / `O(commands)` | Snapshot walks every resting order once; replay re-applies each journaled command. |

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

```sh
# ThreadSanitizer: verifies the lock-free queue, atomic stats, and shutdown
# handshake are race-free (cannot be combined with ASan, hence its own tree).
cmake -S . -B build-tsan -DOBE_TSAN=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build-tsan -j
ctest --test-dir build-tsan --output-on-failure
```

### Docker

```sh
docker build -t obe .
docker run --rm obe                       # runs the test suite (default CMD)
docker run --rm obe ./build/obe_bench     # runs the benchmark
```

---

## Tests

`ctest` runs 58 tests across 11 suites:

| Suite | Covers |
|-------|--------|
| `PriceTimePriority` | best-price-first, FIFO within a level, multi-level sweep |
| `PartialFills` | resting partial, aggressor partial + rest, multi-maker, rejects |
| `Cancellation` | removal, level cleanup, mid-queue splice, pool reclaim, idempotency |
| `SelfTrade` | `CancelResting`, `CancelAggressing`, and `None` policies |
| `MemoryPool` | acquire/release, slot reuse, growth, distinctness |
| `RingBuffer` | FIFO, full/empty edges, power-of-two sizing, **2-thread** run |
| `FlatHashMap` | basic ops, growth, key-0 validity, **200k-step randomized oracle** vs `unordered_map` |
| `RiskControls` | size/notional/collar/kill-switch/duplicate limits, metrics, engine integration |
| `Concurrency` | shutdown-handshake completeness, concurrent `stats()` reads, cross-thread kill switch (run under **TSan**) |
| `EventLog` | serialization round-trip, sentinel-never-journaled, malformed/truncated logs, **replay == live bit-for-bit** across cancels / partial fills / STP / rejected-risk / multi-seed stress (inline + threaded) |
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
[PROFILING.md](PROFILING.md), run for real with **Valgrind callgrind** inside
the Docker container (GCC 13, Ubuntu 24.04). Summary: callgrind showed the
engine's dominant cost is **heap allocation from container nodes** (`malloc`/
`free` ≈ 16% of the program, from the `std::map` tree and `unordered_map` index
— not from `Order`s, which the pool already keeps off-heap). Reserving the id
index up front (one line) removes the periodic `unordered_map` rehash; that
barely moves the instruction count (−1.4% `Ir`) but cuts wall-clock **mean
latency ~11%** and roughly **halves the p99.9 tail**, because a rehash is a rare
but expensive burst. The write-up named the remaining heap-allocation hotspot
(the node-based index) as a follow-up — **since done**: replacing it with an
allocation-free flat hash map cut mean latency ~48% and p99 ~78%, with the full
evaluation and callgrind evidence (malloc share 16% → ~2%) in [INDEX.md](INDEX.md).

---

## Event sourcing & replay

The engine is deterministic, so the only thing needed to recover or audit a
session is its **input stream**. `MatchingEngine::enable_logging(path)` journals
every applied command to an **append-only binary log** (8-byte versioned header +
fixed-width 40-byte records). `replay_log(path, engine)` feeds those commands
back into a fresh engine and reconstructs the book **bit-for-bit** — same resting
orders, FIFO order, sequence numbers, and trade counts.

```cpp
MatchingEngine live;  live.enable_logging("session.bin");
/* ... run the session ... */            live.flush_log();

MatchingEngine recovered;                 // same SelfTradePolicy / RiskConfig
replay_log("session.bin", recovered);     // recovered.book() == live.book()
```

`tests/test_event_log.cpp` *proves* equivalence (via `OrderBook::snapshot()`)
across cancellations, partial fills, self-trade prevention, and rejected risk
orders, plus a multi-seed generator stress run, and covers malformed/truncated
logs. **Crash-safety caveats** (no `fsync`; host-endian format; torn-tail
recovery) are documented honestly in [EVENT_SOURCING.md](EVENT_SOURCING.md).

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
  Types.hpp        domain primitives (Price, Side, OrderType, STP policy)
  Order.hpp        order = intrusive FIFO node; OrderSnapshot
  PriceLevel.hpp   O(1) intrusive FIFO queue at one price
  OrderBook.hpp    the matching book (RB-trees + flat-hash index + pool)
  Trade.hpp        execution report / submit result types
  Command.hpp      producer→consumer message (+ shutdown sentinel)
  MemoryPool.hpp   free-list allocator
  RingBuffer.hpp   lock-free SPSC queue
  FlatHashMap.hpp  allocation-free open-addressing order index
  RiskControls.hpp pre-trade risk gate + metrics + kill switch
  EventLog.hpp     append-only command journal + replay
  MatchingEngine.hpp  consumer thread + book + risk + journal
  OrderGenerator.hpp  synthetic flow
src/             out-of-line implementations
tests/           GoogleTest suites (one file per category)
bench/           benchmark.cpp + sweep.sh
Dockerfile       reproducible Linux build/test/profile image
.github/workflows/ci.yml   Release tests + ASan/UBSan + ThreadSanitizer
SYNTHETIC_DATA.md  how the order generator models market flow
PROFILING.md       the profiling pass with before/after numbers
INDEX.md           order-index redesign: slot-table vs flat-hash + measurements
EVENT_SOURCING.md  the binary journal format and replay proof
```
