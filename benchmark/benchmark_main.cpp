// The showpiece benchmark for Project 1.
//
// Pre-generates every order before the timed region (so the RNG never
// runs on the hot path), pins the benchmark thread to a fixed core,
// checks whether huge pages are available, then times every single
// call into the matching engine with rdtsc, converts ticks -> ns using
// a CLOCK_MONOTONIC-calibrated conversion factor, and reports a full
// percentile breakdown plus a bucketed histogram.
#include "../include/matching_engine.hpp"
#include "rdtsc_timer.hpp"
#include <vector>
#include <algorithm>
#include <random>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

#if defined(__linux__)
  #include <sched.h>
  #include <sys/mman.h>
#elif defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/thread_policy.h>
#endif

using namespace hft;

// Core pinning is a Linux-specific concept (pthread_setaffinity_np +
// cpu_set_t are glibc extensions); macOS deliberately does not expose hard
// core pinning to user space -- the closest equivalent, Mach's
// THREAD_AFFINITY_POLICY, is only a scheduling *hint* ("try to co-locate
// threads that share an affinity tag"), not a guarantee a thread stays on
// one core. We apply it on macOS since it's better than nothing, but it's
// not equivalent to Linux's real pinning -- rerun latency-sensitive
// benchmarks on Linux if you need to trust core-pinned numbers.
static bool pinToCore(int core) {
#if defined(__linux__)
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#elif defined(__APPLE__)
    thread_affinity_policy_data_t policy = { core };
    kern_return_t kr = thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
                                          (thread_policy_t)&policy, THREAD_AFFINITY_POLICY_COUNT);
    return kr == KERN_SUCCESS;
#else
    (void)core;
    return false;
#endif
}

// Probes for 2MB huge page availability and reports whether the pool's
// backing memory could use them. We don't hard-fail if unavailable --
// we just tell you how to enable them, matching a real deployment
// checklist rather than silently degrading. This check is Linux-specific
// (MAP_HUGETLB doesn't exist on macOS, which uses a different superpage
// mechanism); on macOS we just note that and move on.
static void reportHugePageStatus() {
#if defined(__linux__)
    void* p = mmap(nullptr, 2 * 1024 * 1024, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (p == MAP_FAILED) {
        std::fprintf(stderr,
            "[warn] 2MB huge pages unavailable (mmap MAP_HUGETLB: %s).\n"
            "       Enable with: sudo sysctl -w vm.nr_hugepages=128\n"
            "       Continuing on regular 4K pages -- expect more TLB\n"
            "       misses and slightly noisier tail latency.\n", std::strerror(errno));
    } else {
        std::fprintf(stderr, "[ok]   2MB huge pages available on this system.\n");
        munmap(p, 2 * 1024 * 1024);
    }
#else
    std::fprintf(stderr,
        "[info] huge-page probing is Linux-specific (MAP_HUGETLB); skipping on this OS.\n"
        "       macOS uses a different superpage mechanism and doesn't expose the\n"
        "       same knob to userspace the way Linux's vm.nr_hugepages sysctl does.\n");
#endif
}

int main(int argc, char** argv) {
    size_t numOrders = 10'000'000;
    int core = 1;
    if (argc > 1) numOrders = std::strtoull(argv[1], nullptr, 10);
    if (argc > 2) core = std::atoi(argv[2]);

    reportHugePageStatus();

    if (pinToCore(core)) {
        std::fprintf(stderr, "[ok]   pinned benchmark thread to core %d\n", core);
    } else {
        std::fprintf(stderr, "[warn] could not pin to core %d (%s) -- results may be noisier\n",
                      core, std::strerror(errno));
    }

    double nsPerTick = calibrateTscToNs();
    std::fprintf(stderr, "[info] TSC calibration: %.4f ns/tick\n\n", nsPerTick);

    // Pre-generate all order requests. Nothing here runs inside the timed loop.
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int64_t> priceDist(9900, 10100); // ticks around 10000
    std::uniform_int_distribution<int64_t> qtyDist(1, 100);
    std::uniform_int_distribution<int> sideDist(0, 1);
    std::uniform_int_distribution<int> typeDist(0, 9); // 80% limit, 10% IOC, 10% FOK

    std::vector<OrderRequest> orders(numOrders);
    for (size_t i = 0; i < numOrders; ++i) {
        OrderRequest& r = orders[i];
        r.id    = i + 1;
        r.side  = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
        r.price = priceDist(rng);
        r.qty   = qtyDist(rng);
        int t = typeDist(rng);
        r.type  = (t < 8) ? OrderType::Limit : (t == 8 ? OrderType::IOC : OrderType::FOK);
        r.timestamp = i;
    }

    MatchingEngine<1 << 16> engine(numOrders + 1024);

    std::vector<uint64_t> latencyTicks(numOrders);
    auto onTrade = [](const Trade&) { /* no-op sink -- isolates matching cost */ };

    std::fprintf(stderr, "[info] running %zu orders...\n", numOrders);
    for (size_t i = 0; i < numOrders; ++i) {
        uint64_t t0 = rdtsc();
        engine.processDirect(orders[i], onTrade);
        uint64_t t1 = rdtsc();
        latencyTicks[i] = t1 - t0;
    }

    std::vector<double> latNs(numOrders);
    for (size_t i = 0; i < numOrders; ++i) latNs[i] = latencyTicks[i] * nsPerTick;
    std::sort(latNs.begin(), latNs.end());

    auto pct = [&](double p) { return latNs[static_cast<size_t>(p * (numOrders - 1))]; };
    double sum = 0;
    for (double v : latNs) sum += v;

    std::printf("=== Order Matching Engine Benchmark ===\n");
    std::printf("orders processed   : %zu\n", numOrders);
    std::printf("mean latency       : %.1f ns\n", sum / numOrders);
    std::printf("p50                : %.1f ns\n", pct(0.50));
    std::printf("p90                : %.1f ns\n", pct(0.90));
    std::printf("p99                : %.1f ns\n", pct(0.99));
    std::printf("p99.9              : %.1f ns\n", pct(0.999));
    std::printf("max                : %.1f ns\n", latNs.back());
    std::printf("best bid / ask     : %lld / %lld\n",
                 (long long)engine.book().bestBid(), (long long)engine.book().bestAsk());

    std::printf("\n--- latency histogram ---\n");
    struct Bucket { double lo, hi; };
    std::vector<Bucket> buckets = {{0,50},{50,100},{100,200},{200,500},{500,1000},
                                    {1000,2000},{2000,5000},{5000,1e9}};
    for (auto& b : buckets) {
        size_t count = std::count_if(latNs.begin(), latNs.end(),
            [&](double v){ return v >= b.lo && v < b.hi; });
        double pctOfTotal = 100.0 * count / numOrders;
        std::printf("[%6.0f - %8.0f ns) : %10zu  (%6.3f%%)\n", b.lo, b.hi, count, pctOfTotal);
    }
    return 0;
}
