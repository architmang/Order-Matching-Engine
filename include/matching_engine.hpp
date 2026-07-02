#pragma once
#include "spmc_queue.hpp"
#include "order.hpp"
#include "order_book.hpp"
#include "order_pool.hpp"
#include <cstddef>

namespace hft {

struct OrderRequest {
    OrderId   id = 0;
    Side      side = Side::Buy;
    OrderType type = OrderType::Limit;
    Price     price = 0;
    Quantity  qty = 0;
    uint64_t  timestamp = 0;
};

// Wraps a single-instrument OrderBook and drains an SPMC queue of
// incoming OrderRequests.
//
// In production you would shard many of these -- one MatchingEngine
// per symbol (or per symbol-hash bucket) -- each running its own
// dedicated, core-pinned consumer thread that pops only its own
// slice of order flow off a shared gateway queue. That's exactly the
// architecture the SPMC queue above is built for: one gateway thread
// publishing, N independent matching-engine threads each consuming
// disjoint order streams.
template <size_t QueueCapacity>
class MatchingEngine {
public:
    explicit MatchingEngine(size_t poolCapacity)
        : pool_(poolCapacity), book_(pool_) {}

    bool submit(const OrderRequest& req) { return queue_.push(req); }

    // Drains and processes everything currently available on the queue.
    // Intended to be called from a single consumer thread pinned to a
    // dedicated core.
    size_t drain(const TradeCallback& onTrade) {
        OrderRequest req;
        size_t n = 0;
        while (queue_.pop(req)) {
            book_.submit(req.id, req.side, req.type, req.price, req.qty, req.timestamp, onTrade);
            ++n;
        }
        return n;
    }

    // Bypasses the queue entirely. Used by the latency benchmark so we
    // measure pure matching-engine cost, not queue push/pop overhead
    // (which is a separately-benchmarked, sub-10ns concern -- see
    // Project 3's MPMC queue benchmark).
    void processDirect(const OrderRequest& req, const TradeCallback& onTrade) {
        book_.submit(req.id, req.side, req.type, req.price, req.qty, req.timestamp, onTrade);
    }

    OrderBook& book() { return book_; }

private:
    SPMCQueue<OrderRequest, QueueCapacity> queue_;
    OrderPool pool_;
    OrderBook book_;
};

} // namespace hft
