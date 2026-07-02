#include "../include/matching_engine.hpp"
#include "../include/spmc_queue.hpp"
#include <cassert>
#include <cstdio>
#include <thread>
#include <atomic>

using namespace hft;

static void testLimitMatch() {
    MatchingEngine<64> e(64);
    int trades = 0;
    e.submit({1, Side::Sell, OrderType::Limit, 100, 5, 1});
    e.submit({2, Side::Buy,  OrderType::Limit, 100, 5, 2});
    e.drain([&](const Trade& t){ trades++; assert(t.price == 100); assert(t.qty == 5); });
    assert(trades == 1);
    printf("testLimitMatch passed\n");
}

static void testPriceTimePriority() {
    MatchingEngine<64> e(64);
    // two resting sells at the same price, order 1 first -> must fill first
    e.submit({1, Side::Sell, OrderType::Limit, 100, 5, 1});
    e.submit({2, Side::Sell, OrderType::Limit, 100, 5, 2});
    e.submit({3, Side::Buy,  OrderType::Limit, 100, 5, 3});
    OrderId firstFilled = 0;
    e.drain([&](const Trade& t){ if (firstFilled == 0) firstFilled = t.restingOrderId; });
    assert(firstFilled == 1);
    printf("testPriceTimePriority passed\n");
}

static void testIOCCancelsRemainder() {
    MatchingEngine<64> e(64);
    e.submit({1, Side::Sell, OrderType::Limit, 100, 5, 1});
    e.submit({2, Side::Buy,  OrderType::IOC, 100, 20, 2}); // only 5 available
    int trades = 0;
    e.drain([&](const Trade& t){ trades++; assert(t.qty == 5); });
    assert(trades == 1);
    assert(e.book().bestBid() == 0); // unfilled remainder of the IOC never rests
    printf("testIOCCancelsRemainder passed\n");
}

static void testFOKAllOrNothing() {
    MatchingEngine<64> e(64);
    e.submit({1, Side::Sell, OrderType::Limit, 100, 5, 1});
    e.submit({2, Side::Buy,  OrderType::FOK, 100, 20, 2}); // needs 20, only 5 available -> killed
    int trades = 0;
    e.drain([&](const Trade&){ trades++; });
    assert(trades == 0);
    assert(e.book().bestAsk() == 100); // resting sell order was never touched

    e.submit({3, Side::Buy, OrderType::FOK, 100, 5, 3}); // exactly enough -> fills completely
    e.drain([&](const Trade& t){ trades++; assert(t.qty == 5); });
    assert(trades == 1);
    printf("testFOKAllOrNothing passed\n");
}

static void testCancel() {
    MatchingEngine<64> e(64);
    e.submit({1, Side::Buy, OrderType::Limit, 99, 10, 1});
    e.drain([](const Trade&){});
    assert(e.book().bestBid() == 99);
    assert(e.book().cancel(1));
    assert(e.book().bestBid() == 0);
    assert(!e.book().cancel(1)); // already gone
    printf("testCancel passed\n");
}

static void testSPMCQueueMultiConsumer() {
    constexpr size_t N = 200000;
    SPMCQueue<int, 1 << 12> q;
    std::atomic<size_t> consumedSum{0};
    std::atomic<size_t> consumedCount{0};

    std::thread producer([&]{
        for (size_t i = 0; i < N; ++i) { while (!q.push((int)i)) { std::this_thread::yield(); } }
    });

    auto consumer = [&]{
        int v;
        while (consumedCount.load(std::memory_order_relaxed) < N) {
            if (q.pop(v)) {
                consumedSum.fetch_add(v, std::memory_order_relaxed);
                consumedCount.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };
    std::thread c1(consumer), c2(consumer), c3(consumer);
    producer.join();
    c1.join(); c2.join(); c3.join();

    size_t expected = N * (N - 1) / 2;
    assert(consumedCount.load() == N);
    assert(consumedSum.load() == expected); // every item consumed exactly once: no dupes, no loss
    printf("testSPMCQueueMultiConsumer passed\n");
}

int main() {
    testLimitMatch();
    testPriceTimePriority();
    testIOCCancelsRemainder();
    testFOKAllOrNothing();
    testCancel();
    testSPMCQueueMultiConsumer();
    printf("ALL TESTS PASSED\n");
    return 0;
}
