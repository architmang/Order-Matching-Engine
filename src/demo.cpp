#include "../include/matching_engine.hpp"
#include <cstdio>

using namespace hft;

int main() {
    MatchingEngine<1024> engine(1024);
    auto onTrade = [](const Trade& t) {
        std::printf("TRADE resting=%llu aggressor=%llu price=%lld qty=%lld\n",
            (unsigned long long)t.restingOrderId, (unsigned long long)t.aggressingOrderId,
            (long long)t.price, (long long)t.qty);
    };

    engine.submit({1, Side::Buy,  OrderType::Limit, 100, 10, 1});
    engine.submit({2, Side::Sell, OrderType::Limit, 101, 5,  2});
    engine.submit({3, Side::Sell, OrderType::Limit, 100, 3,  3}); // crosses -> trades with order 1
    engine.submit({4, Side::Buy,  OrderType::IOC,   101, 20, 4}); // takes order 2, cancels rest
    engine.submit({5, Side::Buy,  OrderType::FOK,   100, 100, 5}); // not enough liquidity -> killed

    size_t processed = engine.drain(onTrade);
    std::printf("processed %zu order requests\n", processed);
    std::printf("best bid=%lld best ask=%lld\n",
        (long long)engine.book().bestBid(), (long long)engine.book().bestAsk());
    return 0;
}
