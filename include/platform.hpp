#pragma once
// Cross-platform hardware primitives: a monotonic cycle counter read and a
// "spin-wait hint" instruction, for x86_64 and ARM64 (e.g. Apple Silicon).
//
// x86_64 has RDTSC, a dedicated free-running cycle counter register, read
// with the __rdtsc() intrinsic from <x86intrin.h>.
//
// ARM64 has an equivalent: the "virtual count" register of the ARM Generic
// Timer, CNTVCT_EL0, which is likewise a free-running counter readable
// directly from user space (both Linux and macOS/Apple Silicon permit this
// read without a syscall). There's no compiler intrinsic for it, so it's
// read with a single inline `mrs` (move-from-system-register) instruction.
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  #define HFT_ARCH_X86 1
  #include <x86intrin.h>
#elif defined(__aarch64__) || defined(__arm64__)
  #define HFT_ARCH_ARM64 1
#else
  #error "hft::read_cycle_counter() has no implementation for this CPU architecture"
#endif

namespace hft {

// Reads the CPU's free-running hardware cycle counter. Not comparable
// across different physical cores' counters going out of sync on some
// older x86 chips without an invariant TSC, but fine for the relative,
// same-thread latency measurements this benchmark does.
inline uint64_t read_cycle_counter() {
#if defined(HFT_ARCH_X86)
    _mm_lfence(); // serializes so the counter read isn't reordered around the work being timed
    return __rdtsc();
#elif defined(HFT_ARCH_ARM64)
    uint64_t val;
    asm volatile("isb" ::: "memory");     // serialize, ARM64's equivalent role to the x86 lfence above
    asm volatile("mrs %0, cntvct_el0" : "=r"(val));
    return val;
#endif
}

// Hints to the CPU that we're in a spin-wait loop: reduces power draw and
// gives cache-coherence traffic time to settle before the next retry.
inline void cpu_relax() {
#if defined(HFT_ARCH_X86)
    _mm_pause();
#elif defined(HFT_ARCH_ARM64)
    asm volatile("yield" ::: "memory");
#endif
}

} // namespace hft
