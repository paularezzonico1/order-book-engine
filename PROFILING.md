# Profiling Pass

A full **measure → profile → fix → re-measure** cycle, performed for real inside
the project's Docker container with **Valgrind callgrind** (instruction-level
profiling) plus the benchmark's own wall-clock timing.

> Honesty note: an earlier exploratory pass on macOS used the `sample` profiler
> and concluded that hash-table *rehashing* "dominated" `submit()`. The
> instruction-level callgrind data below shows that was **overstated** — rehash
> is a small fraction of instructions. The fix still helps (it removes
> latency-spiking rehash bursts, improving the tail and mean wall-clock latency),
> but the genuinely dominant engine cost is **heap allocation from container
> nodes**. The numbers and conclusions here are the verified ones.

## Environment

| | |
|---|---|
| Host | Docker container (`docker build -t obe . && docker run ...`) |
| OS / arch | Ubuntu 24.04.4 LTS, aarch64 |
| Compiler | GCC 13.3.0, profiling build `-O3 -g -DNDEBUG` |
| Profiler | Valgrind 3.22.0 `--tool=callgrind --cache-sim=no --branch-sim=no` |
| Wall-clock | `obe_bench` native timing (callgrind distorts absolute time ~50×, so timings are taken *outside* callgrind) |

`perf` is also installed in the image, but hardware PMU access is not available
to a container running on a macOS Docker host, so callgrind (which needs no PMU)
is the tool that was actually used here. On a bare-metal Linux host the same
capture is available via `perf record -g ./build/obe_bench && perf report`.

Reproduce:

```sh
docker build -t obe .
docker run --rm obe bash -c '
  cmake -S . -B build-prof -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF \
        -DCMAKE_CXX_FLAGS_RELEASE="-O3 -g -DNDEBUG" &&
  cmake --build build-prof --target obe_bench -j$(nproc) &&
  valgrind --tool=callgrind --cache-sim=no --branch-sim=no \
           ./build-prof/obe_bench --commands 300000 --depth 50 &&
  callgrind_annotate --auto=no callgrind.out.* | head -30'
```

## Step 1 — Profile (identify the hotspot)

callgrind over `obe_bench --commands 300000 --depth 50`, top functions by `Ir`
(instructions retired). `'2` suffixes are the second thread (the SPSC consumer):

```
   Ir          %      function
42,482,533   8.85%   obe::OrderGenerator::make_submit()          <- harness (flow generation)
42,430,952   8.84%   obe::OrderBook::submit(...)'2
31,974,664   6.66%   obe::OrderBook::submit(...)
27,450,028   5.72%   std::chrono::steady_clock::now()            <- harness (latency timing)
26,483,732   5.52%   obe::MatchingEngine::apply(...)'2
25,102,325   5.22%   _int_free          ┐
23,072,397   4.81%   log  (libm)         │  <- harness: exponential/geometric draws
21,134,924   4.40%   obe::OrderBook::match<map<...greater...>>(...)
20,587,699   4.29%   obe::OrderGenerator::next()                │
18,674,730   3.89%   malloc              │
13,729,806   2.86%   obe::OrderBook::match<map<...less...>>(...) │
13,354,950   2.78%   _int_malloc         │
10,912,317   2.27%   operator new('2)    │
 8,984,119   1.87%   operator new        ┘
 8,419,898   1.75%   std::_Hashtable<...Order*>::_M_erase(...)'2   <- index erase (match/cancel)
 8,246,846   1.72%   std::_Hashtable<...Order*>::_M_erase(...)
```

Two things stand out:

1. **A large share of the profiled binary is the harness, not the engine.**
   callgrind profiles the *whole process*, so `make_submit`/`next` (≈13%),
   `log` from the exponential/geometric distributions (≈10%), the Mersenne
   twister (≈3%), and `steady_clock::now()` (≈6%) all show up. These are
   command *generation and timing*, not matching.

2. **The dominant engine cost is heap allocation.** Summing the allocator
   functions — `_int_free` (5.2%), `malloc` (3.9%), `_int_malloc` (2.8%),
   `operator new` ×2 (4.1%) — gives **≈16% of the entire program** spent in
   `malloc`/`free`. This traffic comes from the per-node allocation of the
   `std::map` price-level tree and the `unordered_map` id index — *not* from
   `Order` objects, which the `MemoryPool` already keeps off the heap.

By contrast, the rehash machinery (`_Prime_rehash_policy::_M_need_rehash`) is
only **~0.7%** — the macOS `sample` profile had badly over-weighted it.

## Step 2 — Fix

The id index's eventual size is bounded by the order-pool capacity, known at
construction. Reserving it up front avoids the periodic rehash (each of which
re-buckets every live node and causes a latency spike):

```cpp
// OrderBook constructor
index_.reserve(pool_capacity);
```

One line, committed in isolation
(`perf(book): reserve id index to remove hot-path rehashing`).

## Step 3 — Re-measure

### Instruction count (callgrind, 300k commands)

| | before (no reserve) | after (reserve) | change |
|---|---:|---:|---:|
| total `Ir` | 487,429,440 | 480,469,982 | **−1.4%** |

A modest instruction-count win — consistent with rehash being a small fraction
of instructions.

### Wall-clock latency (native, 5M commands, depth 50, median of 3 runs)

| metric | before | after | change |
|--------|-------:|------:|-------:|
| mean latency | 313.4 ns | 278.0 ns | **−11.3%** |
| p50 | 166 ns | 125 ns | −24.7% |
| p99 | 1500 ns | 1375 ns | −8.3% |
| p99.9 | ~6166 ns (4250–8625) | ~4000 ns (3542–4417) | **≈ −35%** |
| throughput (SPSC) | ~5.0 M/s | ~4.8 M/s | within run-to-run noise |

The interesting result: the instruction count barely moves (−1.4%) but
wall-clock **mean latency drops ~11% and the p99.9 tail roughly halves**. A
rehash is rare but, when it fires, it walks and re-buckets the whole table —
bursting cache and the allocator — so it inflates *tail latency* far more than
its instruction share suggests. Reserving removes those bursts. Throughput is
flat within noise (the threaded pipeline is dominated by cross-thread hand-off,
not by index inserts).

**Lesson:** instruction-count profilers (callgrind) and wall-clock measurement
answer different questions. The hotspot list told us *where the instructions
go*; only the wall-clock percentiles revealed that the cheap-looking rehash was
a real tail-latency source.

## Remaining hotspot (next candidate, now quantified)

The ~16% of the program spent in `malloc`/`free` is the real next target. Each
resting order allocates a node inside `unordered_map` (`operator new` per
insert, `_int_free` per erase) and each new price level allocates a `std::map`
node — exactly the heap traffic the `MemoryPool` eliminated for `Order` objects.
Backing `index_` (and ideally the price-level map) with a pooled / flat
open-addressing container would remove most of that 16%. It is left as a
documented follow-up to keep the dependency footprint at zero.
