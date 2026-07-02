#pragma once
#include "../include/platform.hpp"
#include <cstdint>
#include <time.h>

namespace hft {

// Kept as `rdtsc()` for readability even though on ARM64 it actually reads
// CNTVCT_EL0, not the x86-specific RDTSC register -- see platform.hpp for
// the architecture-specific implementation.
inline uint64_t rdtsc() {
    return read_cycle_counter();
}

// Calibrates cycle-counter ticks -> nanoseconds by comparing a busy-loop's
// tick delta against CLOCK_MONOTONIC over the same window. Good enough for
// relative latency reporting on a modern chip with a stable counter
// frequency (true of both x86_64's invariant TSC and ARM64's generic
// timer). For bulletproof absolute timing on x86 specifically, you could
// instead read the TSC frequency directly from CPUID leaf 0x15.
inline double calibrateTscToNs() {
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    uint64_t t0 = rdtsc();

    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < 200000000ULL; ++i) x += i;

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    uint64_t t1 = rdtsc();

    double elapsedNs = (ts1.tv_sec - ts0.tv_sec) * 1e9 + (ts1.tv_nsec - ts0.tv_nsec);
    double ticks = static_cast<double>(t1 - t0);
    return elapsedNs / ticks;
}

} // namespace hft
