# Profiling Pass

This documents one end-to-end profiling cycle: **measure → profile → identify a
hotspot → fix → re-measure**, with before/after numbers.

## Environment

| | |
|---|---|
| CPU | Apple M2 (8 cores) |
| Compiler | Apple clang 16, `-O3 -DNDEBUG` (Release) |
| Profiler (local) | `sample` (macOS sampling profiler) |
| Profiler (Linux) | `valgrind --tool=callgrind` and `perf` — installed in the [Dockerfile](Dockerfile) to reproduce on Linux |
| Workload | `obe_bench --commands 5000000 --depth 50` (fixed seed) |

> On Linux the equivalent capture is:
> ```sh
> valgrind --tool=callgrind --callgrind-out-file=callgrind.out ./build/obe_bench --commands 1000000
> callgrind_annotate callgrind.out | head -40
> # or, with perf:
> perf record -g ./build/obe_bench --commands 5000000 && perf report
> ```
> macOS lacks `perf`/`valgrind`, so the local capture below used `sample`; the
> conclusion (and the fix) is profiler-independent.

## Step 1 — Baseline

`obe_bench --csv --commands 5000000 --depth 50`, median of 3 runs:

| metric | value |
|--------|-------|
| throughput (SPSC pipeline) | ~4.81 M ops/sec |
| mean latency | 261.7 ns/command |
| p50 | 84 ns |
| p99 | 2834 ns |
| p99.9 | 4000 ns |

## Step 2 — Profile

Sampling the hot benchmark process (`sample <pid> 3`) gave this dominant stack
(samples elided for brevity):

```
1340  obe::MatchingEngine::apply(Command const&)
 826   obe::OrderBook::submit(...)
 776     std::__hash_table<...Order*...>::__emplace_unique_key_args(...)   <-- HOT
  17       std::__hash_table<...>::__do_rehash<true>(unsigned long)        <-- rehashing
 228     obe::OrderBook::match<std::map<...>>(...)
  76       std::__hash_table<...>::__erase_unique(...)                     <-- index erase
```

The single biggest cost in `submit()` was **not** the matching logic or the
`std::map` price-level tree — it was insertion into `index_`, the
`unordered_map<OrderId, Order*>` used for `O(1)` cancel lookup. Worse, the
profile showed `__do_rehash` firing *during* the run: the table was being grown
and rehashed repeatedly as resting orders accumulated, periodically re-bucketing
every existing entry. That is wasted work and a source of latency spikes (it
inflates the p99/p99.9 tail).

## Step 3 — Fix

The table's eventual size is bounded by the order pool capacity, which is known
at construction time. Reserve it up front so the run never rehashes:

```cpp
// OrderBook constructor
index_.reserve(pool_capacity);   // remove incremental rehashing from hot path
```

One line, committed in isolation
(`perf(book): reserve id index to remove hot-path rehashing`).

## Step 4 — Re-measure

Same command, median of 3 runs:

| metric | before | after | change |
|--------|-------:|------:|-------:|
| mean latency | 261.7 ns | 234.0 ns | **−10.6%** |
| p99 | 2834 ns | 2625 ns | **−7.4%** |
| p99.9 | 4000 ns | 3917 ns | −2.1% |
| throughput | 4.81 M/s | 4.88 M/s | +1.5% |

Removing the periodic rehash cut mean per-command latency by ~11% and, as
expected, tightened the tail (the rehash bursts that inflated p99 are gone).

## Remaining hotspot (next candidate)

Even after the fix, `__emplace_unique_key_args` is still the largest single
cost: each resting order triggers a **node allocation** inside the
`unordered_map` (one `operator new` per insert, `operator delete` per erase) —
ironically the very heap traffic the `MemoryPool` was built to eliminate for
`Order` objects. The natural next step is to back `index_` with a pooled /
open-addressing hash map (e.g. a flat `robin_hood`/`absl::flat_hash_map`-style
table) so id indexing is allocation-free too. That is left as a documented
follow-up rather than pulled in here, to keep the dependency footprint zero.
