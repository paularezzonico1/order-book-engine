# Event Sourcing & Replay

The matching engine is **deterministic**: applying the same ordered sequence of
`Command`s always produces the same book state and the same trades. That single
property is what makes event sourcing work — to recover or audit a session we
do not need to snapshot the book, we only need to journal its *inputs* and
replay them.

## What is logged

`MatchingEngine::enable_logging(path)` installs an `EventLogWriter`. On the
consumer thread, **every applied command** (`Submit`, `Cancel`) is appended to
an append-only binary log *before* it is processed. The `Shutdown` sentinel is
control-plane and is not journaled. Because logging happens on the single
consumer thread, there is no locking and no contention with the producer.

## On-disk format

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

Each 40-byte record encodes a `Command` field-by-field at fixed offsets:

| offset | size | field |
|-------:|-----:|-------|
| 0 | 1 | command type (Submit/Cancel) |
| 1 | 1 | order type |
| 2 | 1 | side |
| 3 | 1 | padding |
| 4 | 4 | trader id |
| 8 | 8 | order id |
| 16 | 8 | price |
| 24 | 8 | quantity |
| 32 | 8 | trigger price |

Fixed-width records mean the reader needs no variable-length framing and the
byte offset of event *N* is simply `8 + N*40` — random access for free.

**Endianness:** integers are written in the host's byte order. This is fine for
the project's use (write and replay on the same architecture) and is the common
choice for an internal journal; a cross-architecture format would fix a byte
order (e.g. little-endian) at the encode/decode boundary. This is called out
explicitly rather than left as a silent assumption.

## Replay

```cpp
MatchingEngine engine;                  // fresh, no logging
replay_log("session.bin", engine);      // read journal + apply every command
// engine is now in exactly the state the live engine was in
```

`replay()` simply feeds the recorded commands back through `apply()`. Because
the engine is deterministic, the reconstructed book is identical to the original
— including resting orders, their FIFO/time-priority order, sequence numbers,
trade counts, and top of book.

## Proof of correctness

`tests/test_event_log.cpp` proves replay equivalence rather than asserting it:

* **`ReplayReconstructsStateBitForBit`** runs 100k generated commands through a
  live (journaling) engine, replays the log into a fresh engine, and asserts the
  two `OrderBook::snapshot()`s are **equal element-for-element** (every resting
  order's side, price, id, remaining qty, and sequence), plus equal command and
  trade counts and identical top of book.
* **`ReplayFromThreadedLiveRun`** does the same where the live run goes through
  the lock-free SPSC pipeline (journaling on the consumer thread), proving the
  log captures the exact applied order even under the threaded hand-off.
* **`SerializationRoundTrip`** checks the binary encode/decode directly.

`OrderBook::snapshot()` is the deterministic, fully-ordered state description
that makes "bit-for-bit equal" a concrete, testable claim.
