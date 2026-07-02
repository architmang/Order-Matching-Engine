#pragma once
#include "order.hpp"
#include <vector>
#include <cstdlib>
#include <new>
#include <stdexcept>

namespace hft {

// Fixed-capacity free-list allocator for Order objects.
//
// Why this exists: calling malloc/new on every incoming order and
// free/delete on every fill is one of the easiest ways to blow your
// tail latency budget -- the general-purpose allocator can take a lock,
// walk free lists, or (worst case) mmap/munmap. Here we grab one big
// slab up front, hand out pointers from a free list, and never touch
// malloc again on the hot path.
class OrderPool {
public:
    explicit OrderPool(size_t capacity) : capacity_(capacity) {
        storage_ = static_cast<Order*>(std::aligned_alloc(64, sizeof(Order) * capacity));
        if (!storage_) throw std::bad_alloc();
        freeList_.reserve(capacity);
        for (size_t i = capacity; i-- > 0; ) freeList_.push_back(&storage_[i]);
    }

    ~OrderPool() { std::free(storage_); }

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;

    // Returns nullptr if the pool is exhausted -- callers must check.
    Order* acquire() {
        if (freeList_.empty()) return nullptr;
        Order* o = freeList_.back();
        freeList_.pop_back();
        *o = Order{};
        return o;
    }

    void release(Order* o) {
        freeList_.push_back(o);
    }

    size_t capacity() const { return capacity_; }
    size_t available() const { return freeList_.size(); }

private:
    size_t capacity_;
    Order* storage_;
    std::vector<Order*> freeList_;
};

} // namespace hft
