#pragma once

#include <atomic>
#include <cstddef>

#include "pipeline_events.hpp"
#include "typed_spsc_ring.hpp"

namespace arena::pipeline {

// One receive-thread producer; one strategy-thread consumer. After a dropped
// event, state may be incomplete: latch a fault until external reconciliation.
template<std::size_t Capacity>
class InboundChannel {
    static_assert(std::atomic<bool>::is_always_lock_free);

public:
    // Producer only. Never spins or overwrites an occupied slot.
    bool publish(const Event& event) noexcept {
        if (faulted_.load(std::memory_order_relaxed)) return false;
        if (ring_.push(event)) return true;
        mark_fault();
        return false;
    }

    // Producer only; also call when conversion/identifier copying fails.
    void mark_fault() noexcept {
        faulted_.store(true, std::memory_order_release);
    }

    // Consumer only. Existing events can still be drained after a fault.
    bool pop(Event& event) noexcept { return ring_.pop(event); }

    bool faulted() const noexcept {
        return faulted_.load(std::memory_order_acquire);
    }

private:
    TypedSpscRing<Event, Capacity> ring_;
    alignas(64) std::atomic<bool> faulted_{false};
};

} // namespace arena::pipeline
