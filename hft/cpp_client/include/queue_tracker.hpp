// ─────────────────────────────────────────────────────────────────────────────
// queue_tracker.hpp — where each of our resting orders sits in the FIFO queue.
//
// The venue is a price-time CLOB. OrderAck tells us queue_ahead (shares that
// fill before ours at our price) and level_qty (the whole level); each
// QueueUpdate refreshes both. This table keeps the latest values per order_id.
//
// Hash map keyed by order_id: O(1) average lookup/insert/erase regardless of
// how many orders are resting. Buckets are reserved up front so the table
// never rehashes on the hot path.
//
// Lifetime of an entry:
//   * stored on an OrderAck with level_qty > 0 (the order is resting);
//   * updated on every QueueUpdate;
//   * removed when a QueueUpdate reports level_qty == 0, when we cancel
//     (remove), or at session close (clear).
// Fill messages carry no order_id, so a fill alone cannot remove an entry.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <cstddef>
#include <string>
#include <unordered_map>

namespace arena
{
    // One resting order and its latest queue standing. The order_id is the
    // map key, so it is not repeated here.
    struct TrackedOrder
    {
        std::string symbol;
        bool is_buy = false;
        double price = 0.0;
        int queue_ahead = 0;   // shares ahead of us right now (0 = front)
        int level_qty = 0;     // total shares resting at our price, ours included
        int initial_ahead = 0; // queue_ahead at the first ack
        int updates = 0;       // QueueUpdates received

        bool at_front() const { return queue_ahead == 0; }
    };

    class QueueTracker
    {
    public:
        using Map = std::unordered_map<std::string, TrackedOrder>;

        explicit QueueTracker(std::size_t expected_orders = 256)
        {
            orders_.reserve(expected_orders); // no rehash until we exceed this
        }

        // nullptr if not tracked. Stays valid until THIS order is erased:
        // unordered_map never moves nodes, even on rehash.
        const TrackedOrder *find(const std::string &order_id) const
        {
            auto it = orders_.find(order_id);
            return it == orders_.end() ? nullptr : &it->second;
        }

        // OrderAck for one of our orders. Returns true if the order is (now)
        // tracked.
        bool on_ack(const std::string &symbol, const std::string &order_id,
                    bool is_buy, double price, int queue_ahead, int level_qty)
        {
            if (queue_ahead == 0 && level_qty == 0)
            {
                orders_.erase(order_id); // filled on arrival: nothing rests
                return false;
            }
            // One hash: finds the existing entry or default-constructs a new one.
            auto [it, inserted] = orders_.try_emplace(order_id);
            TrackedOrder &o = it->second;
            if (inserted)
            {
                o.symbol = symbol;
                o.is_buy = is_buy;
                o.price = price;
                o.initial_ahead = queue_ahead; // only on first insert, so a
                                               // repeated ack can't reset it
            }
            o.queue_ahead = queue_ahead;
            o.level_qty = level_qty;
            return true;
        }

        // QueueUpdate for one of our orders. Returns false if it isn't tracked.
        bool on_queue(const std::string &order_id, int queue_ahead, int level_qty)
        {
            auto it = orders_.find(order_id);
            if (it == orders_.end())
                return false;
            if (level_qty == 0)
            {
                // level_qty includes our own shares, so 0 means we are no
                // longer resting there. Erase by iterator: no second hash.
                orders_.erase(it);
                return true;
            }
            it->second.queue_ahead = queue_ahead;
            it->second.level_qty = level_qty;
            ++it->second.updates;
            return true;
        }

        // We cancelled the order (the venue sends no cancel confirmation).
        void remove(const std::string &order_id) { orders_.erase(order_id); }

        // Session closed: every resting order is gone. Keeps the buckets.
        void clear() { orders_.clear(); }

        std::size_t size() const { return orders_.size(); }

        // for (const auto &[id, o] : tracker) { ... }
        Map::const_iterator begin() const { return orders_.begin(); }
        Map::const_iterator end() const { return orders_.end(); }

    private:
        Map orders_;
    };

} // namespace arena
