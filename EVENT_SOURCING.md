# Event Sourcing & Replay

The matching engine is **deterministic**: applying the same ordered sequence of
`Command`s always produces the same book state and the same trades. That single
property is what makes event sourcing work — to recover or audit a session we
do not need to snapshot the book, we only need to journal its *inputs* and
replay them.

## What is logged

`MatchingEngine::enable_logging(path)` installs an `EventLogWriter`. Inside
`apply()`, **every command is journaled exactly once, before it is processed**,
on the single consumer thread (so there is no locking and no contention with the
producer). Two invariants matter:

* **Exactly once.** `apply()` is the one place a command is acted on — both the
  inline path and the threaded consumer route through it — and it appends to the
  log at the top, so there is no double- or missed-logging.
* **Rejected orders are logged too.** A risk-rejected `Submit` is journaled
  before the risk check runs, so replay re-rejects it identically and the
  reconstructed state matches. (This is why a replay engine must use the same
  `RiskConfig`; see *Replay guarantees*.)
* **The shutdown sentinel is never logged.** The `Shutdown` command is
  control-plane: the consumer loop breaks on it *before* calling `apply()`, and
  `EventLogWriter::append()` additionally drops it defensively — so a log can
  never contain, and therefore never replay, a sentinel.

## On-disk format (version 1)

```
+-----------------------------+
| magic "OBEL" (4 bytes)      |  header, written once
| version  uint32 (=1)        |
+-----------------------------+
| record 0  (40 bytes)        |  one fixed-width record per Command,
| record 1  (40 bytes)        |  in apply order
| ...                         |
+-----------------------------+
```

Each 40-byte record encodes a `Command` field-by-field at fixed offsets (the
record is zeroed first, so padding bytes are deterministic):

| offset | size | field | encoding |
|-------:|-----:|-------|----------|
| 0  | 1 | command type | `0`=Submit, `1`=Cancel (`2`=Shutdown is never written) |
| 1  | 1 | order type   | `0`=Limit (reserved; see *Limitations*) |
| 2  | 1 | side         | `0`=Buy, `1`=Sell |
| 3  | 1 | padding      | always `0` |
| 4  | 4 | trader id    | `uint32` |
| 8  | 8 | order id     | `uint64` |
| 16 | 8 | price        | `int64` (ticks) |
| 24 | 8 | quantity     | `uint64` |
| 32 | 8 | trigger price| `int64` (reserved; see *Limitations*) |

Fixed-width records mean the reader needs no variable-length framing and the
byte offset of event *N* is simply `8 + N*40` — random access for free. The
format is **versioned**: `read_event_log()` rejects a file whose magic or
version does not match.

## Replay

```cpp
MatchingEngine engine;                  // fresh, no logging enabled
replay_log("session.bin", engine);      // read journal + apply every command
// engine.book() is now exactly the state the live engine reached
```

`replay()` feeds the recorded commands back through `apply()`. Because the engine
is deterministic, the reconstructed book is identical to the original —
including resting orders, their FIFO/time-priority order, sequence numbers,
trade counts, and top of book.

### Replay guarantees (and preconditions)

Replay reproduces the live state **bit-for-bit** *iff* the replay engine is
configured identically to the live one:

* **Same `SelfTradePolicy`** (an `OrderBook` constructor argument) — it changes
  matching outcomes.
* **Same `RiskConfig`** if the live run used a risk gate — rejected orders are
  journaled and must be re-rejected the same way.
* **No logging on the replay engine** — otherwise it would re-journal the
  replayed events.

"Bit-for-bit" is made concrete by `OrderBook::snapshot()`, a deterministic,
fully-ordered description of every resting order (side, price, id, remaining
quantity, sequence). Two books with equal snapshots are equal in resting state.

## Limitations & crash-safety (honest accounting)

This is a **clean, deterministic journal**, not a hardened exchange WAL. What it
does *not* guarantee:

* **Not durable against power loss.** `flush()` pushes the buffer to the OS; it
  does **not** `fsync`. A crash can lose records the OS had not yet written to
  disk. (Adding durability is an `fsync`/`O_DSYNC` policy decision, deliberately
  left out.)
* **Torn final record is dropped, not repaired.** If the process dies mid-append,
  the trailing partial record is silently discarded on read, leaving a
  consistent *prefix* of the stream — the standard write-ahead-log recovery
  semantics. A command that was not fully written is treated as never having
  happened.
* **No per-record checksums.** A bad header (magic/version) is rejected, but
  corruption *within* an otherwise-complete record is not detected. A production
  log would add a CRC per record.
* **Host byte order.** Integers are written in the writing host's endianness, so
  a log must be replayed on the same architecture. A portable format would fix
  little-endian at the encode/decode boundary.
* **Single-writer.** Journaling happens on the one consumer thread; the format
  assumes a single appender.
* **No rotation / compaction / snapshotting.** The log grows unbounded and
  replay always starts from the beginning; there is no periodic state snapshot to
  truncate it.
* **Reserved fields.** `order_type` and `trigger_price` are encoded for
  forward-compatibility, but the current engine treats every order as a limit
  order, so they are recorded and faithfully round-tripped but not otherwise
  acted upon.

## Proof of correctness

`tests/test_event_log.cpp` *proves* replay equivalence rather than asserting it:

* **`ReplayReconstructsStateBitForBit`** / **`ReplayFromThreadedLiveRun`** — 100k
  generated commands through a journaling engine (inline and through the
  lock-free SPSC pipeline), replayed into a fresh engine, asserting equal
  `snapshot()`, command/trade counts, and top of book.
* **`StressReplayAcrossSeeds`** — the same across multiple generator seeds.
* **Feature coverage** — `ReplayAcrossCancellationsAndPartialFills`,
  `ReplayWithSelfTradePrevention`, `ReplayWithRejectedRiskOrders` exercise the
  full command surface.
* **Robustness** — `MissingFileThrows`, `BadMagicThrows`,
  `UnsupportedVersionThrows`, `ShortHeaderThrows`,
  `HeaderOnlyLogReplaysToEmptyBook`, `TornTrailingRecordIsRecoveredGracefully`,
  and `ShutdownSentinelIsNeverJournaled`.
* **`SerializationRoundTrip`** checks the binary encode/decode directly.
