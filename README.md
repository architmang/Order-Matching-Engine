# Order Matching Engine

A limit order book written in C++, with price-time priority matching, LIMIT/IOC/FOK order types, a lock-free order-entry queue, and a latency benchmark I use to keep myself honest about performance. I built this because I wanted to actually implement the stuff I'd only read about: cache-friendly data layouts, lock-free concurrency, and what really happens at the hardware level when an order gets pushed through a matching engine.

## Platform support

Runs on x86_64 Linux and ARM64 macOS (Apple Silicon). The only genuinely architecture-specific bit is the cycle counter used for timing, which I abstracted behind `hft::read_cycle_counter()`: RDTSC on x86, the ARM Generic Timer's CNTVCT_EL0 register on ARM64. Everything else is portable.

Two things behave differently on macOS, and I'd rather flag that than pretend otherwise:

- **Core pinning.** Linux's `pthread_setaffinity_np` is a real, hard guarantee. macOS doesn't have an equivalent — the closest thing is Mach's `THREAD_AFFINITY_POLICY`, which is more of a hint to the scheduler than a pin. The benchmark still tries it on macOS, but take those numbers with more of a grain of salt than the Linux ones.
- **Huge page detection.** Linux has `MAP_HUGETLB`; macOS uses a different, less controllable superpage mechanism, so the benchmark just skips the check on macOS with a note instead of faking a result.

Haven't tested on Windows. Everything assumes a POSIX toolchain (Linux, macOS, or WSL).

## How it fits together

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

### Why an SPMC queue for order entry

Worth explaining, because it's not the obvious choice. Taken literally, "single producer, multiple consumers" is a bit unusual for order entry — most real gateways need many producers (client sessions) feeding one matching thread per symbol, because price-time priority for a given instrument has to be resolved by one serial authority. Two threads can't independently decide who arrived first without a lock or a sequencer sitting in front of them.

So I built it exactly as an SPMC queue: one producer, any number of consumers racing to pop, verified under real concurrency in the tests. The way it's wired into the architecture is that one gateway thread publishes normalized order requests, and separate per-symbol matching engines each own a consumer that only claims requests for their own shard. The CAS inside `pop()` guarantees each order is claimed by exactly one consumer, so you get fan-out across parallel, independent order books without ever needing two threads to agree on priority within the same book.

### The order book itself

Bids are a `std::map<Price, PriceLevel, greater<Price>>` (highest price first), asks are the same with `less<Price>` (lowest first). Within a price level, orders sit in a doubly linked intrusive list — the `prev`/`next` pointers live inside the `Order` struct itself, so there's no separate allocation for list nodes. `head` is always the oldest order at that price, which is what gives you price-time priority for free: you always fill from `head`.

`std::map` means O(log n) to find, insert, or erase a price level. That's the right choice to start with — get it correct first. If I wanted to push further, the obvious next step is a flat array indexed directly by price tick (since the tick range for a single instrument is usually bounded), which gets you O(1) best-price access and much better cache locality than walking a tree.

### Order types

- **LIMIT** — matches against the opposite book while price crosses; whatever's left over rests in the book.
- **IOC** — matches whatever crosses immediately; anything unfilled is discarded, never rests.
- **FOK** — walks the opposite book read-only first to check whether the full requested quantity is available; if not, nothing happens at all, no partial fill, no state change. If it is available, the match executes and is guaranteed to fully complete.

All three have dedicated tests, including one that checks two resting orders at the same price fill in arrival order, and one that checks a killed FOK leaves the book completely untouched.

### Keeping malloc off the hot path

Calling `new`/`delete` per order is a fast way to blow a latency budget — the general allocator can take a lock, walk free lists, or in the worst case call into the kernel. `OrderPool` grabs one 64-byte-aligned slab up front and hands out/reclaims `Order*` pointers from a free list. Nothing in the matching path touches malloc.

### The SPMC queue mechanics

It's a Vyukov-style ring buffer, specialized for one producer:

- `push()` only ever runs on one thread, so it just writes the slot and does a release store on its sequence number — no compare-exchange needed, since there's nothing else to race against.
- `pop()` can run on any number of threads. They race on a shared `head_` counter with `compare_exchange_weak`, so two consumers can never claim the same slot.
- Every slot carries its own sequence number, so a consumer can tell whether a slot has actually been published before touching its data. That's what avoids torn reads without needing a lock.

## The benchmark

`benchmark/benchmark_main.cpp`:

1. Pre-generates every order before the timed region starts, so the RNG never runs on the hot path — you're measuring the matching engine, not `mt19937`.
2. Tries to pin the benchmark thread to a fixed core.
3. Checks for 2MB huge page availability and reports status without failing the build if they're unavailable.
4. Times every call into the engine with the cycle counter, converts ticks to nanoseconds using a `CLOCK_MONOTONIC`-calibrated factor.
5. Prints mean, p50/p90/p99/p99.9/max, and a bucketed histogram.

```bash
mkdir build && cd build && cmake .. && make
./benchmark 10000000 1    # 10M orders, try to pin to core 1
```

### What it actually measured, on my machine (M-series MacBook, no core isolation, no huge pages — this is a laptop, not tuned bare metal)

```
[info] huge-page probing is Linux-specific; skipped on macOS
[warn] could not pin to core 1 -- results may be noisier
orders processed   : 10,000,000
mean latency       : 87.9 ns
p50                : 83.3 ns
p90                : 125.0 ns
p99                : 291.7 ns
p99.9              : 2916.7 ns
max                : 30,215,231.5 ns
```

p99 sits at 291.7 ns, well inside a 5µs target, and the shape holds up across 10 million orders on an ordinary laptop with none of the usual tuning (no core isolation, no huge pages, no pinning that macOS can actually enforce). The `max` outlier is scheduler noise, not the engine — the OS descheduling the thread for a moment, which is exactly what core isolation and a dedicated box would eliminate.

I wouldn't quote these numbers as representative of production hardware. What they do show is that the methodology holds — pre-generated input, cycle-accurate timing, percentiles instead of just an average — and that per-order cost stays consistently sub-microsecond even without any of the usual tricks.

### What I'd do next to get closer to production numbers

- Reserve huge pages on Linux (`vm.nr_hugepages`) and back `OrderPool`'s slab with a `MAP_HUGETLB` region instead of `aligned_alloc`.
- Isolate a core with `isolcpus`/`nohz_full` and keep everything else off it.
- Lock CPU frequency (`cpupower frequency-set -g performance`) so the core doesn't ramp mid-run.
- Replace the `std::map` price levels with a flat, tick-indexed array once the instrument's realistic price range is known.

## Layout

```
include/order.hpp            Order, Trade, Side, OrderType
include/order_pool.hpp       fixed-capacity slab allocator
include/spmc_queue.hpp       single-producer/multi-consumer lock-free ring buffer
include/order_book.hpp       price-time priority book, LIMIT/IOC/FOK matching
include/matching_engine.hpp  wires the queue to the book
include/platform.hpp         cross-platform cycle counter (x86_64 / ARM64)
src/demo.cpp                 small runnable walkthrough of matching behavior
benchmark/benchmark_main.cpp the latency benchmark
benchmark/rdtsc_timer.hpp    cycle counter + calibration helpers
tests/test_matching.cpp      correctness tests, including a concurrent queue stress test
```

## Tests

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
