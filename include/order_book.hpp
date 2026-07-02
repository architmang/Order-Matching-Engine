#pragma once
#include "order.hpp"
#include "order_pool.hpp"
#include <map>
#include <unordered_map>
#include <functional>
#include <algorithm>
#include <cassert>

namespace hft {

// Doubly linked FIFO list of resting orders at one price level.
// This is what gives price-time priority: within a price level, the
// order that arrived first is always at `head` and gets filled first.
struct PriceLevel {
    Order* head = nullptr;
    Order* tail = nullptr;
    Quantity totalQty = 0;

    void pushBack(Order* o) {
        o->prev = tail;
        o->next = nullptr;
        if (tail) tail->next = o; else head = o;
        tail = o;
        totalQty += o->qty;
    }

    void remove(Order* o) {
        totalQty -= o->qty;
        if (o->prev) o->prev->next = o->next; else head = o->next;
        if (o->next) o->next->prev = o->prev; else tail = o->prev;
        o->prev = o->next = nullptr;
    }

    bool empty() const { return head == nullptr; }
};

using TradeCallback = std::function<void(const Trade&)>;

// Price-time priority limit order book for a single instrument.
//   Bids: highest price first  -> std::map<Price, PriceLevel, greater<>>
//   Asks: lowest price first   -> std::map<Price, PriceLevel, less<>>
//
// std::map is a red-black tree: O(log n) best-bid/ask access and
// insertion. This is the standard choice for correctness-first LOBs;
// a further-optimized version would replace it with a flat sorted
// vector of price levels (or a direct-indexed array keyed by price
// tick, if the tick range is bounded) to get O(1) best-price access
// and much better cache locality -- see the README for that discussion.
class OrderBook {
public:
    explicit OrderBook(OrderPool& pool) : pool_(pool) {}

    // Returns false only if the order pool is exhausted.
    bool submit(OrderId id, Side side, OrderType type, Price price, Quantity qty,
                uint64_t timestamp, const TradeCallback& onTrade) {
        switch (type) {
            case OrderType::Limit: return submitLimit(id, side, type, price, qty, timestamp, onTrade);
            case OrderType::IOC:   return submitIOC(id, side, type, price, qty, timestamp, onTrade);
            case OrderType::FOK:   return submitFOK(id, side, type, price, qty, timestamp, onTrade);
        }
        return false;
    }

    bool cancel(OrderId id) {
        auto it = liveOrders_.find(id);
        if (it == liveOrders_.end()) return false;
        Order* o = it->second;
        removeFromBook(o);
        pool_.release(o);
        liveOrders_.erase(it);
        return true;
    }

    Price bestBid() const { return bids_.empty() ? 0 : bids_.begin()->first; }
    Price bestAsk() const { return asks_.empty() ? 0 : asks_.begin()->first; }
    size_t bidLevels() const { return bids_.size(); }
    size_t askLevels() const { return asks_.size(); }
    size_t liveOrderCount() const { return liveOrders_.size(); }

private:
    // Total quantity available at prices that would satisfy `limitPrice`,
    // used to answer "can this FOK be fully filled right now?" without
    // mutating any state.
    Quantity availableAgainstAsks(Price limitPrice, Quantity needed) const {
        Quantity total = 0;
        for (auto& [price, level] : asks_) {
            if (price > limitPrice) break;
            total += level.totalQty;
            if (total >= needed) break;
        }
        return total;
    }
    Quantity availableAgainstBids(Price limitPrice, Quantity needed) const {
        Quantity total = 0;
        for (auto& [price, level] : bids_) {
            if (price < limitPrice) break;
            total += level.totalQty;
            if (total >= needed) break;
        }
        return total;
    }

    template <typename Book>
    void matchAgainst(Book& book, Side aggressorSide, OrderId aggressorId, Price limit,
                       Quantity& remaining, const TradeCallback& onTrade) {
        while (remaining > 0 && !book.empty()) {
            auto it = book.begin();
            Price levelPrice = it->first;
            bool crosses = (aggressorSide == Side::Buy) ? (levelPrice <= limit)
                                                          : (levelPrice >= limit);
            if (!crosses) break;

            PriceLevel& level = it->second;
            while (remaining > 0 && level.head) {
                Order* resting = level.head;
                Quantity fill = std::min(remaining, resting->qty);

                Trade t{resting->id, aggressorId, levelPrice, fill};
                if (onTrade) onTrade(t);

                remaining      -= fill;
                resting->qty   -= fill;
                level.totalQty -= fill;

                if (resting->qty == 0) {
                    level.remove(resting);
                    liveOrders_.erase(resting->id);
                    pool_.release(resting);
                }
            }
            if (level.empty()) book.erase(it);
        }
    }

    bool submitLimit(OrderId id, Side side, OrderType type, Price price, Quantity qty,
                      uint64_t ts, const TradeCallback& onTrade) {
        Quantity remaining = qty;
        if (side == Side::Buy) matchAgainst(asks_, side, id, price, remaining, onTrade);
        else                   matchAgainst(bids_, side, id, price, remaining, onTrade);

        if (remaining > 0) {
            Order* o = pool_.acquire();
            if (!o) return false; // pool exhausted
            o->id = id; o->side = side; o->type = type;
            o->price = price; o->qty = remaining; o->timestamp = ts;
            restOrder(o);
        }
        return true;
    }

    bool submitIOC(OrderId id, Side side, OrderType type, Price price, Quantity qty,
                    uint64_t ts, const TradeCallback& onTrade) {
        (void)ts; (void)type;
        Quantity remaining = qty;
        if (side == Side::Buy) matchAgainst(asks_, side, id, price, remaining, onTrade);
        else                   matchAgainst(bids_, side, id, price, remaining, onTrade);
        // Whatever is left after matching is cancelled -- it never rests.
        return true;
    }

    bool submitFOK(OrderId id, Side side, OrderType type, Price price, Quantity qty,
                    uint64_t ts, const TradeCallback& onTrade) {
        (void)ts; (void)type;
        Quantity available = (side == Side::Buy) ? availableAgainstAsks(price, qty)
                                                   : availableAgainstBids(price, qty);
        if (available < qty) return true; // killed: not enough liquidity, zero state change
        Quantity remaining = qty;
        if (side == Side::Buy) matchAgainst(asks_, side, id, price, remaining, onTrade);
        else                   matchAgainst(bids_, side, id, price, remaining, onTrade);
        assert(remaining == 0);
        return true;
    }

    void restOrder(Order* o) {
        liveOrders_[o->id] = o;
        if (o->side == Side::Buy) bids_[o->price].pushBack(o);
        else                      asks_[o->price].pushBack(o);
    }

    void removeFromBook(Order* o) {
        if (o->side == Side::Buy) {
            auto it = bids_.find(o->price);
            it->second.remove(o);
            if (it->second.empty()) bids_.erase(it);
        } else {
            auto it = asks_.find(o->price);
            it->second.remove(o);
            if (it->second.empty()) asks_.erase(it);
        }
    }

    OrderPool& pool_;
    std::map<Price, PriceLevel, std::greater<Price>> bids_;
    std::map<Price, PriceLevel, std::less<Price>>    asks_;
    std::unordered_map<OrderId, Order*> liveOrders_;
};

} // namespace hft
