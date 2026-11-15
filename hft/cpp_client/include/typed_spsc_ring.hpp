#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace arena {

// One producer calls push; one consumer calls pop/empty. No concurrent reset.
// Allocate large instances off the stack before starting either worker.
template<class T, std::size_t Capacity>
class TypedSpscRing {
    static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a nonzero power of two");
    static_assert(std::atomic<std::size_t>::is_always_lock_free);
    // Construction happens at startup. Only hot-path assignment must be noexcept.
    static_assert(std::is_default_constructible_v<T>);
    static_assert(std::is_nothrow_copy_assignable_v<T>);

    // A whole aligned wrapper keeps subsequent members out of the index line.
    struct alignas(64) Index {
        std::atomic<std::size_t> value{0};
    };

public:
    TypedSpscRing() = default;
    TypedSpscRing(const TypedSpscRing&) = delete;
    TypedSpscRing& operator=(const TypedSpscRing&) = delete;

    bool push(const T& value) noexcept {
        const auto tail = tail_.value.load(std::memory_order_relaxed);
        const auto head = head_.value.load(std::memory_order_acquire);
        if (tail - head == Capacity) return false;
        slots_[tail & kMask] = value;
        // Publish the completed copy to the consumer.
        tail_.value.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& out) noexcept {
        const auto head = head_.value.load(std::memory_order_relaxed);
        const auto tail = tail_.value.load(std::memory_order_acquire);
        if (head == tail) return false;
        out = slots_[head & kMask];
        // Permit reuse only after the consumer finishes copying the slot.
        head_.value.store(head + 1, std::memory_order_release);
        return true;
    }

    // Consumer-side snapshot, not a reservation or shutdown protocol.
    bool empty() const noexcept {
        const auto head = head_.value.load(std::memory_order_relaxed);
        return head == tail_.value.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

private:
    static constexpr std::size_t kMask = Capacity - 1;
    Index head_;
    Index tail_;
    alignas(64) std::array<T, Capacity> slots_{};
};

} // namespace arena
