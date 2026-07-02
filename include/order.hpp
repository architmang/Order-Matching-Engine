#pragma once
#include <cstdint>
#include <cstddef>

namespace hft {

enum class Side : uint8_t { Buy = 0, Sell = 1 };

enum class OrderType : uint8_t {
    Limit = 0,   // rests in the book if not fully filled
    IOC   = 1,   // Immediate-Or-Cancel: fill what you can right now, cancel the remainder
    FOK   = 2    // Fill-Or-Kill: fill completely right now, or do nothing at all
};

using OrderId  = uint64_t;
using Price    = int64_t;   // integer ticks -- never floats in the hot path
using Quantity = int64_t;

struct Order {
    OrderId    id        = 0;
    Price      price     = 0;
    Quantity   qty       = 0;      // remaining (unfilled) quantity
    Side       side      = Side::Buy;
    OrderType  type      = OrderType::Limit;
    uint64_t   timestamp = 0;      // sequence number / rdtsc, used for FIFO tie-break

    // Intrusive doubly linked list pointers owned by PriceLevel.
    // "Intrusive" means the list node lives inside the Order itself --
    // no separate allocation for a list node, no extra pointer chasing.
    Order* prev = nullptr;
    Order* next = nullptr;
};

struct Trade {
    OrderId  restingOrderId    = 0;
    OrderId  aggressingOrderId = 0;
    Price    price = 0;
    Quantity qty   = 0;
};

} // namespace hft
