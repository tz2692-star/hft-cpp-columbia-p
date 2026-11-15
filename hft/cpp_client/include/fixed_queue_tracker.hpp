#pragma once

#include <array>
#include <cstddef>
#include <string_view>

#include "pipeline_events.hpp"

namespace arena {

// Strategy-thread-owned bounded table. Linear scans are bounded by Capacity;
// there are no nodes, allocations, tombstones, or rehashes on insertion/erase.
template<std::size_t Capacity = 256>
class FixedQueueTracker {
public:
    struct Entry {
        pipeline::OrderId order_id;
        pipeline::Symbol symbol;
        double price = 0.0;
        int queue_ahead = 0;
        int level_qty = 0;
        int initial_ahead = 0;
        std::size_t updates = 0;
        bool used = false;
    };

    // False means capacity/identifier failure; a fully filled ack is successful.
    bool on_ack(const pipeline::AckEvent& event) noexcept {
        if (event.order_id.view().empty() || event.symbol.view().empty()) return false;
        auto* entry = find_mutable(event.order_id.view());
        if (event.queue_ahead == 0 && event.level_qty == 0) {
            if (entry) erase(*entry);
            return true;
        }
        if (!entry) {
            for (auto& slot : entries_) {
                if (!slot.used) { entry = &slot; break; }
            }
            if (!entry) return false;
            *entry = Entry{};
            entry->order_id = event.order_id;
            entry->symbol = event.symbol;
            entry->price = event.price;
            entry->initial_ahead = event.queue_ahead;
            entry->used = true;
            ++size_;
        }
        entry->queue_ahead = event.queue_ahead;
        entry->level_qty = event.level_qty;
        return true;
    }

    void on_queue(const pipeline::QueueEvent& event) noexcept {
        auto* entry = find_mutable(event.order_id.view());
        if (!entry) return;
        if (event.level_qty == 0) { erase(*entry); return; }
        entry->queue_ahead = event.queue_ahead;
        entry->level_qty = event.level_qty;
        ++entry->updates;
    }

    const Entry* find(std::string_view id) const noexcept {
        for (const auto& entry : entries_)
            if (entry.used && entry.order_id.view() == id) return &entry;
        return nullptr;
    }

    void remove(std::string_view id) noexcept {
        if (auto* entry = find_mutable(id)) erase(*entry);
    }

    void clear() noexcept {
        for (auto& entry : entries_) entry.used = false;
        size_ = 0;
    }
    std::size_t size() const noexcept { return size_; }

private:
    Entry* find_mutable(std::string_view id) noexcept {
        for (auto& entry : entries_)
            if (entry.used && entry.order_id.view() == id) return &entry;
        return nullptr;
    }
    void erase(Entry& entry) noexcept { entry.used = false; --size_; }
    std::array<Entry, Capacity> entries_{};
    std::size_t size_ = 0;
};

} // namespace arena
