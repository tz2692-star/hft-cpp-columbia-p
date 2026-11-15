#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

struct SPSCRing {
    explicit SPSCRing(std::size_t capacity_pow2)
        : capacity_(checked_capacity(capacity_pow2)),
          mask_(capacity_ - 1),
          buffer_(capacity_) {}

    alignas(64) std::atomic<std::size_t> head_{0};   // consumer writes, producer reads
    alignas(64) std::atomic<std::size_t> tail_{0};   // producer writes, consumer reads

    // Called by the producer. Returns false without modifying the queue when full.
    bool push(std::uint64_t v) {
        // The producer owns tail, so it can read its own index relaxed.
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        // Acquire the consumer's latest head before reusing a freed slot.
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head == capacity_) return false;

        // Mask the sequence number to wrap it into the fixed-size buffer.
        buffer_[tail & mask_] = v;
        // Publish the stored value; the consumer's acquire load of tail sees it.
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // Called by the consumer. Returns false and leaves out unchanged when empty.
    bool pop(std::uint64_t& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (tail == head) return false;

        out = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // The consumer-side snapshot; call from the consumer thread for meaningful
    // SPSC status.
    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    // The producer-side snapshot; call from the producer thread for meaningful
    // SPSC status.
    bool full() const {
        return tail_.load(std::memory_order_acquire) -
               head_.load(std::memory_order_acquire) == capacity_;
    }

private:
    static_assert(std::atomic<std::size_t>::is_always_lock_free);

    static std::size_t checked_capacity(std::size_t capacity) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0)
            throw std::invalid_argument("SPSCRing capacity must be a nonzero power of two");
        return capacity;
    }

    // Number of values the queue can hold. Fixed after construction so storage
    // never grows or allocates during push()/pop().
    const std::size_t capacity_;

    // capacity_ - 1; ANDing a monotonically increasing sequence number with
    // this mask maps it to a buffer slot because capacity_ is a power of two.
    const std::size_t mask_;

    // Preallocated slots. head and tail sequence numbers determine which slots
    // are published and available; an occupied slot may be reused after pop().
    std::vector<std::uint64_t> buffer_;
};
