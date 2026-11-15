#pragma once
#include <cstdint>
#include <vector>
// HW8 — sliding-window event counter. count() is called with a non-decreasing clock.
//
// Edge case that costs most people a test (see labs/week08.md step 5): ts_ns and
// now_ns are UNSIGNED. Early on, now_ns <= window_ns and the mathematical cutoff
// (now - window) is negative — there is no uint64_t that means that, and there is
// no safe value to clamp it to. Only compute the subtraction when it is
// meaningful; do not expire anything before then.
struct RollingCounter {
    uint64_t window_;
    std::vector<uint64_t> buf_;
    size_t head_ = 0, tail_ = 0;
    static constexpr size_t cap = 1 << 20;
    static_assert((cap & (cap-1)) == 0, "CAP must be a power of two");
    explicit RollingCounter(uint64_t window_ns):window_(window_ns) { 
        buf_.resize(cap);

        // TODO(student): ring/deque of timestamps; amortized O(1).
    }   
    void add(uint64_t ts_ns) {
        buf_[tail_ & (buf_.size() - 1)] = ts_ns;
        ++tail_;
    }
    uint64_t count(uint64_t now_ns) { 
        if (now_ns > window_){
            const uint64_t cutoff = now_ns - window_;
            while (head_ < tail_ && buf_[head_ & (buf_.size()-1)] <= cutoff){
                ++head_;
            }
        }
        return tail_ - head_; /* TODO: events with ts > now-window */
    }
};
