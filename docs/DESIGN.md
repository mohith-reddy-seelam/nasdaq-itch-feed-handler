# Design

```
 capture (.bin, mmap)      MoldUDP64 packets (UDP)
          │                         │  mold::Receiver: sequence tracking, gap/duplicate detection
          ▼                         ▼
   FrameReader ──────────► itch::dispatch()  ── zero-copy overlay, compile-time-checked layouts
                                    │
                                    ▼
                         book::BookBuilder<OrderMap, Levels>
                    ┌───────────────┴────────────────┐
         order store: ref -> order          price levels per instrument and side
         (direct-mapped flat table)         (sorted vector, best at the back)
                                    │  top-of-book changed?
                                    ▼
                    feed::Pipeline strategy stage: microprice, imbalance, OFI
```

## 1. Decoding: zero-copy overlays

ITCH 5.0 is a binary, big-endian, unaligned protocol of 23 fixed-length
message types. Every field type in [`wire.hpp`](../include/itch/wire.hpp) is
a byte array (`alignof == 1`), so a message struct has *exactly* the wire
layout. `sizeof(AddOrder) == 36` because the spec says 36, with no packing
pragmas, and all 23 lengths are `static_assert`ed against the spec's table.

A received message is viewed in place with `std::start_lifetime_as<M>` (C++23),
the standard way to say "these bytes are an M" without copying and without
strict-aliasing UB. Fields are decoded only when read: one load plus one
`BSWAP`. The 6-byte timestamp is read as one 8-byte load from inside the header
and masked, rather than six byte loads.

`dispatch()` is one `switch` on the type byte. It checks the length, then calls
`handler.on(msg)` *only if the handler has that overload* (a concept check in
`if constexpr`). A book builder doesn't implement NOII or MWCB, and those
message types cost it a jump and a compare, nothing more.

A malformed message is counted, never fatal: unknown types, bad lengths and
truncated tails all show up in `ReplayStats`.

## 2. Transport: MoldUDP64

[`mold.hpp`](../include/itch/mold.hpp) implements NASDAQ's UDP framing:
session, sequence number of the first message, message count, then
length-prefixed messages. The receiver tracks the next expected sequence number
and, per message, delivers it, drops it as a duplicate, or records a **gap**
(the range to request from the re-request server). Heartbeats
(`count == 0`) reveal gaps even when no data arrives, and `count == 0xFFFF` is
end of session. Tested: in-order delivery, a dropped packet, a duplicated packet,
a heartbeat ahead of us, and malformed packets.

The replay pipeline reads the BinaryFILE capture directly (that is what NASDAQ
publishes). The Mold path is benchmarked on the same messages repacketised at a
1400-byte MTU.

## 3. The order book

Market-by-order semantics, ITCH 5.0 section 4:

| msg | effect |
|---|---|
| `A`/`F` add | new order; level `shares += n`, `orders += 1` |
| `E` executed | order and level `-= executed`; order removed at 0 |
| `C` executed with price | same for the book; the trade price is reported separately |
| `X` cancel | order and level `-= cancelled` |
| `D` delete | remove the order's remaining shares and the order |
| `U` replace | delete the old reference, add the new one (same side and stock) |
| `P`/`Q` hidden trade / cross | volume only: never touches the visible book |

### 3.1 Order store: direct-mapped open addressing

~70 % of ITCH messages (E, C, X, D, U) name an order only by its 64-bit
reference number, so ref → order is the hottest lookup in the system.

`BasicFlatOrderMap<Hash>`: one flat array of 24-byte `Order` slots, linear
probing, **backward-shift deletion** (no tombstones, so probe chains don't decay
over a day of ~10⁸ deletes), sized for a load factor ≤ 1/2 at the expected
peak, and preallocated, so there is no allocation per order.

The hash policy is the interesting choice:

* `FibonacciHash` (multiply by 2⁶⁴/φ) spreads keys uniformly. Textbook-correct,
  but it turns NASDAQ's nearly sequential reference numbers into random slots
  across a 200-400 MB table. Every lookup is then a cache miss and a TLB miss.
* `IdentityHash` (`ref & mask`) is direct-mapped. Consecutive orders land in
  consecutive slots, and since most activity is on recently added orders, the
  working set is a few hot cache lines near the "front" of the reference
  sequence.

Measured on the full NASDAQ day, the direct-mapped table is 14 % faster
(21.9 s vs 24.9 s). On the first 1.2 M pre-market messages it was 61 % faster;
the difference is a lesson in itself, because small samples flatter
locality tricks (RESULTS.md §5). It would be the wrong choice for
adversarial keys; for an exchange's sequential reference numbers it is the
right one.

### 3.2 Price levels: sorted vector, best at the back

Per instrument and side, a `std::vector<Level>` sorted so that **the best price
is at the back**:

* best bid/ask is `levels.back()`: O(1);
* finding a price scans from the back, and real order flow is concentrated
  within a few ticks of the touch, so it is usually a handful of compares, all
  in one or two cache lines;
* inserting or erasing near the touch moves only the few elements behind it.

Benchmarked against binary search on the same array and against `std::map`
(a node allocation per level and a pointer chase per lookup). Linear-from-the-touch
wins on real data.

### 3.3 Top-of-book change detection

After every book event the builder recomputes the instrument's best bid/offer
(O(1)) and compares it with the cached one. Only changes are forwarded to the
strategy: about 46 % of book events on the full day (124.8 M of 268.7 M).

### 3.4 Hiding memory latency: prefetching and huge pages

The baseline `perf` profile of a full-day build put **~47 % of all time in the
order table** (`insert` 28.6 %, `find` 15.6 %, `erase` 3 %): not computation,
cache misses. An add's slot was last touched ~capacity orders earlier, so it
is always cold, and a delete hits an order that may have rested for hours.

* **Software-pipelined prefetching.** `replay_prefetched<8>()` frames message
  *i + 8* and calls `book.prefetch()` on it while message *i* is applied.
  `prefetch` reads only the header and the order reference(s) at fixed offsets,
  and issues `__builtin_prefetch` for the order-table slot(s) and the
  instrument's book, top and statistics. By the time the message is applied,
  its lines are (mostly) in L1/L2. In the pipeline, the book stage does the
  same over each drained batch ("group prefetching"). Lookahead was tuned on
  the full day: 8, 16 and 32 are equal within noise, and 64 is worse (lines are
  evicted before use). **+28 % single-thread.**
* **2 MB pages** for the order table (`madvise(MADV_HUGEPAGE)` on Linux):
  512× fewer pages, so far fewer TLB misses. **+4 %.** Small, because
  prefetching already hides most of the page walks too, but free.

On the pre-market sample (5,800 live orders, everything in cache), prefetching
was 16 % *slower*: pure overhead with nothing to hide. Optimisations that
attack memory latency must be measured on realistic working sets.

## 4. Pipeline

[`feed/pipeline.hpp`](../include/feed/pipeline.hpp): three threads pinned to
three cores, joined by the wait-free SPSC ring from the companion project
(vendored in `third_party/spsc`, slot-sequence signalling, 128-byte padding).

* **feed** frames messages out of the mapped capture and pushes
  `{pointer, length, arrival_tsc}` (24 bytes, no copy of the message). Pacing:
  flat out, a fixed rate, or the exchange's own timestamps scaled by a speed
  factor. Paced arrival stamps are the *scheduled* time (coordinated-omission
  safe).
* **book** drains up to 64 events per synchronisation (`try_pop_n`), applies
  each, and forwards top-of-book changes.
* **strategy** maintains per-instrument microprice, queue imbalance and the
  Cont-Kukanov-Stoikov order-flow imbalance.

**Sharding by instrument.** With `--cpus=F,B1,B2,...,S` the book is split
across several book threads. The feed routes each message by
`stock_locate % shards`. Every ITCH message header carries the instrument, so
an order's whole lifecycle (add, executions, cancels, delete, replace) lands on
the same shard. Each shard owns a private order table and private books: the
shards share nothing, synchronise with nothing, and scale with cores. The
strategy thread polls one ring per shard. A test checks that a 3-shard run
ends with exactly the books and top-of-book changes of a single-threaded
replay. Measured on the full day: 1 → 2 → 3 shards = 1.0× → 2.04× → 3.07×
(slightly superlinear, because each shard's order table is a third the size
and stays warmer).

**Window mode** is how real-time latency is measured on a full day. Everything
before the window is replayed flat out to build the book (not measured). The
feed then waits for both queues to drain, and replays the window at the
exchange's own pace. The 09:29-09:32 window contains the opening cross and the
first two minutes of continuous trading, the busiest stretch of the day.

Each stage's statistics live in their own 128-byte-aligned block, so per-message
bookkeeping never false-shares across cores. Latency is recorded in a
fixed-size log-linear histogram (32 sub-buckets per power of two, 2.2 %
resolution), because one sample per message would be 1.6 GB per stage on a
full day.

**Profile-guided optimisation** is available as a build step
(`scripts/pgo_build.sh`: instrument, train on 30 M real messages, rebuild). It
gives a consistent but small +5.5 % (three runs each side, no overlap). It is
not the default, because the profile is trained on the same day it is
evaluated on, and it complicates the build.

## 5. Correctness

* **Layout:** `static_assert(sizeof(M) == spec length)` for all 23 types, and a
  golden test that decodes a hand-assembled `AddOrder` byte-by-byte from the
  spec's offsets (independently of our encoder).
* **Differential testing:** four book implementations (direct and Fibonacci
  hashing, linear and binary level search, and the `std::unordered_map` +
  `std::map` textbook book) must agree on the touched instrument's top of
  book **after every message**, and on full depth for every instrument at the
  end. Run on a 2 M-message synthetic feed in the unit tests, and on the first
  50 M messages of the real NASDAQ day by `run_all.sh`.
* **Integrity counters on real data:** unknown message types, bad lengths,
  references to unknown orders, and level mismatches are all counted and
  reported; on a complete day they must all be zero.
* **External check:** the official NASDAQ closing cross price for each
  instrument (`Q` message, cross type `C`) is decoded and reported, so it can
  be compared against published closing prices.

## 6. C++26 (and 23) features used

| Feature | Where | Why |
|---|---|---|
| `std::start_lifetime_as` (C++23) | `parser.hpp`, `mold.hpp` | Zero-copy message overlays without aliasing UB |
| `std::byteswap` (C++23) | `wire.hpp` | Big-endian fields: one `BSWAP` |
| Static reflection (`^^T`, `std::meta`) | `itch_replay.cpp`, `describe.hpp` | Message-type names (`kTypeName['A'] == "AddOrder"`) from the struct names; `summary.json` / `machine.json` from the structs |
| Expansion statements (`template for`) | `bench_main.cpp`, `describe.hpp` | The book-variant matrix is a tuple of types expanded at compile time |
| Contracts (`pre`) | vendored ring buffer | Checked in tests, compiled out of measured binaries |
| `= delete("reason")` | `MappedFile` | Says *why* a mapping can't be copied |

## 7. Not done

* **Live UDP multicast receive** (socket, kernel-bypass): the Mold decoder is
  ready for it; on this Windows desktop the socket stack would dominate every
  number.
* **Gap recovery** (re-request server, buffering until the gap fills): gaps are
  detected and reported, not repaired.
* **Load-aware sharding:** shards are assigned by `stock_locate % N`, which
  gave a 29 / 35 / 36 % split. Assigning the heaviest instruments (SPY, QQQ,
  IWM...) to shards by yesterday's message counts would even it out.
