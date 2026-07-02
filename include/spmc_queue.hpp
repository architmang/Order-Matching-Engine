#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace hft {

// 64 bytes is the cache line size on essentially every mainstream x86_64
// and ARM64 chip (including Apple Silicon). We hardcode it rather than
// querying std::hardware_destructive_interference_size because that
// feature-test macro is unreliable across standard library
// implementations -- some (e.g. AppleClang's libc++ at the time of
// writing) advertise __cpp_lib_hardware_interference_size as defined
// without actually providing the member, which breaks the build.
constexpr size_t SPMC_CACHE_LINE = 64;

// Single-Producer / Multi-Consumer bounded lock-free ring buffer.
//
// Why SPMC and not the more obvious MPSC: a single gateway thread is
// the only thing allowed to publish new order requests (push), but
// several downstream consumer threads may each own a different shard
// of the book (e.g. matching engines sharded by symbol hash) and pull
// work off the same queue. Exactly one consumer ever gets a given
// order -- the CAS on `head_` in pop() guarantees that.
//
// Design (Vyukov-style ring buffer, specialized for one producer):
//  - Capacity must be a power of two so `& (Capacity - 1)` replaces a
//    modulo with a mask.
//  - push() is called from exactly one thread, so it needs no CAS at
//    all: it just writes the slot and publishes it with a release
//    store on that slot's sequence number.
//  - pop() may be called from any number of threads. Consumers race
//    on the shared `head_` counter and use compare_exchange to claim
//    a slot, so two consumers can never both take the same item.
//  - Every slot carries its own sequence number so a consumer can
//    tell whether the slot it's looking at has actually been
//    published yet (no half-written reads, no ABA on the index alone).
template <typename T, size_t Capacity>
class SPMCQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

    struct alignas(SPMC_CACHE_LINE) Slot {
        std::atomic<size_t> sequence;
        T data;
    };

public:
    SPMCQueue() {
        for (size_t i = 0; i < Capacity; ++i)
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        head_.store(0, std::memory_order_relaxed);
    }

    // Producer side -- must only ever be called from a single thread.
    bool push(const T& item) {
        size_t pos = tail_.load(std::memory_order_relaxed);
        Slot& slot = slots_[pos & (Capacity - 1)];
        if (slot.sequence.load(std::memory_order_acquire) != pos) {
            return false; // full: consumers haven't freed this slot yet
        }
        slot.data = item;
        slot.sequence.store(pos + 1, std::memory_order_release);
        tail_.store(pos + 1, std::memory_order_relaxed);
        return true;
    }

    // Consumer side -- safe to call from any number of threads concurrently.
    bool pop(T& out) {
        size_t pos = head_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[pos & (Capacity - 1)];
            size_t seq = slot.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
            if (diff == 0) {
                if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    out = slot.data;
                    slot.sequence.store(pos + Capacity, std::memory_order_release);
                    return true;
                }
                // CAS failed: `pos` was refreshed to the current head_ by the
                // library, loop and re-check that slot.
            } else if (diff < 0) {
                return false; // empty
            } else {
                pos = head_.load(std::memory_order_relaxed); // another consumer raced ahead
            }
        }
    }

    static constexpr size_t capacity() { return Capacity; }

private:
    alignas(SPMC_CACHE_LINE) std::atomic<size_t> tail_;
    alignas(SPMC_CACHE_LINE) std::atomic<size_t> head_;
    Slot slots_[Capacity];
};

} // namespace hft
