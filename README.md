# Project 1: Full-Stack Order Matching Engine

A limit order book (LOB) in C++ with price-time priority matching, LIMIT/IOC/FOK
order types, an SPMC lock-free order-entry queue, and an rdtsc-based latency
benchmark targeting sub-5µs p99 tail latency.

## Platform support

Builds and runs on both x86_64 Linux and ARM64 macOS (Apple Silicon).
`include/platform.hpp` abstracts the one truly architecture-specific piece
(the hardware cycle counter: RDTSC on x86_64, the ARM Generic Timer's
CNTVCT_EL0 register on ARM64) behind `hft::read_cycle_counter()`.

Two things behave differently on macOS, both handled gracefully rather than
failing the build:
- **Core pinning**: Linux's `pthread_setaffinity_np` is a hard guarantee;
  macOS has no equivalent -- the closest thing, Mach's
  `THREAD_AFFINITY_POLICY`, is only a scheduling *hint*. The benchmark uses
  it on macOS but the numbers should be trusted less there than on Linux.
- **Huge-page detection**: Linux-specific (`MAP_HUGETLB`); skipped with a
  note on macOS, which uses a different, less user-controllable superpage
  mechanism.

If you're on Windows/MSVC this hasn't been tested; the CMakeLists.txt and
`-pthread` flag assume a POSIX toolchain (Linux, macOS, or WSL).

## Architecture

```
                 ┌─────────────────┐
  gateway thread │  SPMCQueue<T>    │  N consumer threads (one per
  (producer)  ──▶│  (lock-free ring)│─▶  symbol shard, in production)
                 └─────────────────┘
                                              │
                                              ▼
                                    ┌───────────────────┐
                                    │  MatchingEngine    │
                                    │  ┌───────────────┐ │
                                    │  │  OrderBook     │ │
                                    │  │  bids / asks   │ │
                                    │  └───────────────┘ │
                                    │  ┌───────────────┐ │
                                    │  │  OrderPool     │ │  (no malloc
                                    │  │  (slab alloc)  │ │   on hot path)
                                    │  └───────────────┘ │
                                    └───────────────────┘
```

### Why single-producer, multi-consumer for order entry (and the honest caveat)

The spec asked for an SPMC queue for order entry. Taken completely literally,
that's an unusual choice: real order-entry gateways more commonly need
**many** producers (many client sessions) feeding **one** matching thread per
symbol (MPSC), because price-time priority for a given instrument has to be
resolved by a single serial authority -- you can't have two threads both
deciding who was "first" without a lock or a sequencer in front of them.

So `SPMCQueue` is implemented and tested exactly as specified (one producer,
N racing consumers, verified under real concurrency in `tests/test_matching.cpp`),
and it's wired into the architecture as: **one gateway thread publishes
normalized order requests, and N independent per-symbol `MatchingEngine`
instances each own a consumer** that claims only the requests for its shard.
Because each order is claimed by exactly one consumer (the CAS in `pop()`
enforces that), this gives you fan-out to parallel, independent order books
without ever needing two threads to agree on priority for the *same* book.
If your interviewer pushes on "why not MPSC," this is the answer to give --
and it demonstrates you understand the tradeoff, not just the API.

### The order book: `std::map` of price levels, intrusive FIFO lists

- `bids_`: `std::map<Price, PriceLevel, greater<Price>>` -- highest price first.
- `asks_`: `std::map<Price, PriceLevel, less<Price>>` -- lowest price first.
- Within a `PriceLevel`, orders are a **doubly linked intrusive list**: the
  `prev`/`next` pointers live inside the `Order` struct itself, so there's no
  separate list-node allocation. `head` is always the oldest order at that
  price -- price-time priority falls straight out of "always fill from `head`."

`std::map` is a red-black tree: O(log n) to find/insert/erase a price level.
This is the standard "correct first" choice. A further-optimized version
(worth mentioning in an interview, not necessary to build first) would use a
flat array indexed directly by price tick (when the tick range is bounded, as
it usually is for a single instrument) to get O(1) best-price access and much
better cache locality than tree-node pointer chasing.

### Order types

- **LIMIT**: match against the opposite book while price crosses; whatever
  doesn't fill rests in the book as a new price-time-ordered entry.
- **IOC (Immediate-Or-Cancel)**: match whatever crosses right now; the
  unfilled remainder is discarded, never rests.
- **FOK (Fill-Or-Kill)**: first walk the opposite book (read-only) to check
  whether *all* of the requested quantity is available at an acceptable
  price; if not, do nothing at all -- no partial fill, no state change. If
  yes, execute the match, which is then guaranteed to fully complete.

All three are implemented and covered by dedicated tests in
`tests/test_matching.cpp`, including a test that two resting orders at the
same price fill in arrival order (price-time priority), and a test that a
killed FOK leaves the book completely untouched.

### `OrderPool`: no `malloc` on the hot path

Calling `new`/`delete` per order is one of the easiest ways to blow a latency
budget -- the general-purpose allocator can take a lock, walk free lists, or
in the worst case call into the kernel (`mmap`/`brk`). `OrderPool` grabs one
64-byte-aligned slab up front and hands out/reclaims `Order*` from a plain
`std::vector`-backed free list. Nothing on the matching path calls `malloc`.

### `SPMCQueue`: single producer needs no CAS to push

The queue is a Vyukov-style ring buffer specialized for exactly one producer:
- `push()` (single thread only) just writes the slot and does a release
  store of its sequence number -- no compare-exchange needed, because there's
  no other producer to race against.
- `pop()` (any number of threads) races on a shared `head_` counter using
  `compare_exchange_weak`, so two consumers can never claim the same slot.
- Every slot carries its own sequence number, so a consumer can tell whether
  a slot has actually been published yet before touching its data -- this is
  what prevents torn reads without needing a lock.

## The showpiece benchmark

`benchmark/benchmark_main.cpp`:
1. Pre-generates every order **before** the timed region (RNG never runs on
   the hot path -- you're measuring the matching engine, not `mt19937`).
2. Pins the benchmark thread to a fixed core with `pthread_setaffinity_np`.
3. Probes for 2MB huge page availability via `mmap(MAP_HUGETLB)` and reports
   status (it doesn't fail if unavailable -- it tells you the exact `sysctl`
   command to enable them, matching a real deployment checklist).
4. Times every single call into the engine with `rdtsc` (serialized with
   `lfence` so the CPU can't reorder work around the timestamp), and converts
   ticks to nanoseconds using a `CLOCK_MONOTONIC`-calibrated factor.
5. Reports mean, p50/p90/p99/p99.9/max, and a bucketed latency histogram.

### Running it

```bash
mkdir build && cd build && cmake .. && make
./benchmark 10000000 1    # 10M orders, pin to core 1
```

Or directly with g++ (what was used to validate this in the sandbox):

```bash
g++ -std=c++17 -O3 -march=native -DNDEBUG -pthread benchmark/benchmark_main.cpp -o benchmark
```

### What was actually measured here (single shared vCPU, be skeptical of the absolute numbers)

```
[warn] 2MB huge pages unavailable in this sandbox
[ok]   pinned benchmark thread to core 0
orders processed   : 2,000,000
mean latency       : 117.6 ns
p50                : 95.4 ns
p90                : 187.1 ns
p99                : 371.4 ns
p99.9              : 1165.4 ns
max                : 5,189,672.4 ns   <-- OS scheduling noise on a shared 1-vCPU box, not the engine
```

The p99 (**371 ns**, well under the 5µs target) and the overall shape are
real and meaningful: even on a noisy shared VM, per-order matching cost is
consistently sub-microsecond. The `max` outlier is not the matching engine --
it's this container getting descheduled for a few milliseconds by the host,
which is exactly the kind of noise huge pages, core isolation
(`isolcpus`/`nohz_full`), and a dedicated bare-metal box eliminate. **Rerun
this on real hardware with core isolation before quoting a number in an
interview**; the methodology (pinning, rdtsc, pre-generated input, histogram
not just mean) is the part that's actually being evaluated.

### Getting genuinely closer to "10M orders/sec, <5µs p99" on real hardware

- `sudo sysctl -w vm.nr_hugepages=128` and back the `OrderPool` slab with a
  `MAP_HUGETLB` mmap region instead of `aligned_alloc` (the benchmark already
  detects and reports huge page availability; wiring the pool itself to use
  them is a natural next step, noted here rather than done blindly since it
  requires a machine where hugepages are actually reserved to test against).
- `isolcpus=<core>` + `nohz_full=<core>` on the kernel command line, and pin
  every other process off that core, to remove scheduler jitter entirely.
- Disable frequency scaling (`cpupower frequency-set -g performance`) so the
  core doesn't ramp clocks mid-benchmark.
- Switch the `std::map` price levels to a flat, tick-indexed array once you
  know your instrument's realistic price range, to cut the O(log n) tree
  traversal down to O(1).

## Files

```
include/order.hpp            Order, Trade, Side, OrderType
include/order_pool.hpp       fixed-capacity slab allocator
include/spmc_queue.hpp       single-producer/multi-consumer lock-free ring buffer
include/order_book.hpp       price-time priority book, LIMIT/IOC/FOK matching
include/matching_engine.hpp  wires the queue to the book
src/demo.cpp                 readable walkthrough of matching behavior
benchmark/benchmark_main.cpp the showpiece latency benchmark
benchmark/rdtsc_timer.hpp    rdtsc + TSC calibration helpers
tests/test_matching.cpp      correctness tests, including concurrent queue stress test
```

## Verified in this environment

```
$ g++ -std=c++17 -O2 -pthread tests/test_matching.cpp -o test_matching && ./test_matching
testLimitMatch passed
testPriceTimePriority passed
testIOCCancelsRemainder passed
testFOKAllOrNothing passed
testCancel passed
testSPMCQueueMultiConsumer passed
ALL TESTS PASSED
```
