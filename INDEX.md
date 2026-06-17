# Order Index: from `std::unordered_map` to an allocation-free flat hash

The order index maps `OrderId -> Order*` so that **cancel** can locate a resting
order in O(1) before splicing it out. It started as `std::unordered_map`. This
documents replacing it with a purpose-built allocation-free table, the design
evaluation behind that choice, and the measured impact.

## Why change it

`PROFILING.md` (the callgrind pass) found the engine spending **~16% of total
instructions in `malloc`/`free`**. The two sources were node allocations in the
`std::map` price-level tree and in the `std::unordered_map` order index:
`unordered_map` allocates **one heap node per insert and frees one per erase**,
plus a pointer-chase per lookup. That is precisely the heap traffic the
`MemoryPool` was built to eliminate for `Order` objects — yet the index was
quietly re-introducing it on every resting order.

## Evaluation: slot table vs. flat hash map

| | **Slot table** (`vector<Order*>` indexed by id) | **Flat hash map** (open addressing) |
|---|---|---|
| Lookup | O(1), single load, perfect locality | O(1) avg, 1–2 cache-line probes |
| Allocation | none after reserve | none after reserve |
| Assumes dense ids? | **Yes** — wastes one slot per *unused* id; fails for sparse/large/sharded ids | **No** — any id distribution works |
| Memory if ids sparse | unbounded / pathological | bounded by live count / load factor |
| Robustness | brittle (depends on id policy) | general-purpose |

A slot table is the *fastest possible* index **if** order ids are dense and
bounded — and in this project's own generator they happen to be (`1, 2, 3, …`).
But order-id allocation is an upstream policy decision an engine should not bake
in: real venues use sparse, sharded, or client-supplied ids. Choosing the slot
table would optimise for the benchmark's id scheme rather than for a real one.

**Decision: flat open-addressing hash map.** It is allocation-free and cache-
friendly like the slot table, but makes no assumption about id density. (If a
deployment *guaranteed* dense ids, swapping in a slot table behind the same
interface would be a further win — the interface is small on purpose.)

## Design (`include/obe/FlatHashMap.hpp`)

* **Open addressing, linear probing.** Entries live in three parallel,
  contiguous arrays (`keys` / `values` / `state`). Probes walk adjacent slots —
  cache-line-friendly, unlike node-chasing.
* **Separate `state` byte** instead of a reserved sentinel key, so *any* key
  value (including 0) is valid.
* **Backward-shift deletion** (Knuth 6.4 Algorithm R) instead of tombstones, so
  probe sequences stay short under heavy insert/erase churn (the order index's
  exact workload).
* **SplitMix64 finalizer** as the hash, so sequential ids don't degrade linear
  probing into long runs.
* **`reserve()` up front** sizes the arrays once; steady-state operations never
  call the allocator.

Correctness is covered by `tests/test_flat_hash_map.cpp`, including a 200k-step
**randomized oracle test** against `std::unordered_map` (the deletion logic is
the easiest place to introduce a bug, so it is fuzzed directly).

## Measured impact

### Latency / throughput (native, Apple M2, `-O3`, 5M commands, median of 3)

| depth | metric | before (`unordered_map`) | after (`FlatHashMap`) | change |
|------:|--------|-------------------------:|----------------------:|-------:|
| 50  | throughput (SPSC) | 4.94 M ops/s | **7.63 M ops/s** | **+55%** |
| 50  | mean latency | 248 ns | **128 ns** | **−48%** |
| 50  | p99 | 2833 ns | **625 ns** | **−78%** |
| 50  | p99.9 | 4000 ns | **1334 ns** | **−67%** |
| 100 | throughput (SPSC) | 4.52 M ops/s | **7.19 M ops/s** | **+49%** |
| 100 | mean latency | 244 ns | **138 ns** | **−43%** |
| 100 | p99 | 2667 ns | **667 ns** | **−75%** |
| 100 | p99.9 | 4083 ns | **1400 ns** | **−66%** |

The tail improvement is the headline: removing per-operation heap allocation
(and its occasional `malloc` slow paths / allocator locking across the two
pipeline threads) roughly **halves mean latency and cuts p99 by ~3–4×**.

### Allocation (callgrind, Docker/Linux GCC 13, 300k commands)

| | before | after |
|---|---:|---:|
| total instructions (`Ir`) | 480.5 M | **421.8 M** (−12%) |
| share in `malloc`/`free`/`operator new` | **~16%** | **~2%** |

The order index's allocator traffic is gone; `FlatHashMap::insert` now shows up
as pure compute (~4%, no allocation). The residual ~2% is the `std::map`
price-level tree still allocating a node per *new* price level — the documented
next target (a tick-indexed flat price-level array), out of scope here.

### Memory

Both are O(entries). The flat map trades a little headroom (it holds
`capacity = next_pow2(live / load_factor)` slots of `key(8)+value(8)+state(1)`
≈ 17 B/slot at ≤ 7/8 load) for **one contiguous allocation** instead of one
heap node per entry plus a bucket array — eliminating per-node malloc headers
(~16 B each) and the fragmentation/pointer-chasing of the node-based map.
