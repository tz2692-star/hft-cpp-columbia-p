#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "messages.hpp"

namespace arena {

// One symbol's latest REPORTED depth. Omitted depth is not reconstructed.
// The band is centred on the touch; levels beyond it are skipped, not stored.
// This class is single-owner (the strategy thread in the live pipeline);
// it is not a concurrent reader/writer book.
class LocalBook {
public:
    static constexpr std::size_t kPriceSlots = 1024;
    using Tick = std::int64_t;
    using Quantity = std::int64_t;

    enum class Status { NoSnapshot, Ok, InvalidSnapshot, InvalidLevel, DepthTooWide };

    struct Top {
        double bid_price = 0.0;
        double ask_price = 0.0;
        Quantity bid_quantity = 0;
        Quantity ask_quantity = 0;
    };

    explicit LocalBook(double tick_size) : tick_size_(tick_size) {
        if (!std::isfinite(tick_size_) || tick_size_ <= 0.0)
            throw std::invalid_argument("LocalBook requires a positive finite tick size");
    }

    bool update(const BookSnapshotMsg& snapshot) {
        // Failed updates make old data unavailable, but preserve its slot list
        // so that the next successful update can clear it correctly.
        status_ = Status::InvalidSnapshot;
        if (!snapshot.depth_valid ||
            snapshot.bid_count > snapshot.bids.size() ||
            snapshot.ask_count > snapshot.asks.size())
            return false;

        std::array<Tick, kMaxSnapshotLevels> bid_ticks{};
        std::array<Tick, kMaxSnapshotLevels> ask_ticks{};

        // Validate the entire replacement before changing the stored arrays.
        // Track the touch (best bid / best ask): the band is placed around it,
        // not around the full depth, which on the live feed can span ~10x
        // more ticks than the band holds.
        bool have_bid = false, have_ask = false;
        Tick best_bid_tick = 0, best_ask_tick = 0;

        for (std::size_t j = 0; j < snapshot.bid_count; ++j) {
            if (snapshot.bids[j].quantity <= 0 || !to_ticks(snapshot.bids[j].price, bid_ticks[j])) {
                status_ = Status::InvalidLevel;
                return false;
            }
            if (!have_bid || bid_ticks[j] > best_bid_tick) best_bid_tick = bid_ticks[j];
            have_bid = true;
        }
        for (std::size_t j = 0; j < snapshot.ask_count; ++j) {
            if (snapshot.asks[j].quantity <= 0 || !to_ticks(snapshot.asks[j].price, ask_ticks[j])) {
                status_ = Status::InvalidLevel;
                return false;
            }
            if (!have_ask || ask_ticks[j] < best_ask_tick) best_ask_tick = ask_ticks[j];
            have_ask = true;
        }

        Tick next_base = base_tick_;
        bool move_band = false;
        if (have_bid || have_ask) {
            // Set the low/high bounds of the best bid/ask touch; for a one-sided book,
            // both bounds are that side's best price.
            const Tick touch_low  = have_bid && have_ask ? std::min(best_bid_tick, best_ask_tick)
                                  : have_bid ? best_bid_tick : best_ask_tick;
            const Tick touch_high = have_bid && have_ask ? std::max(best_bid_tick, best_ask_tick)
                                  : have_bid ? best_bid_tick : best_ask_tick;
            // Only a spread wider than the band itself is unrepresentable.
            if (touch_high - touch_low >= static_cast<Tick>(kPriceSlots)) {
                status_ = Status::DepthTooWide;
                return false;
            }
            // Reuse the current band only if it has been initialized and both
            // touch prices map to slots within its [base_tick_, base_tick_ + kPriceSlots) range.
            const bool fits = based_ && touch_low >= base_tick_ &&
                touch_high - base_tick_ < static_cast<Tick>(kPriceSlots);
            if (!fits) {
                // Centre the band on the touch: ~kPriceSlots/2 ticks of room
                // each side before the next recenter.
                const Tick centre = touch_low + (touch_high - touch_low) / 2;
                next_base = std::max<Tick>(0, centre - static_cast<Tick>(kPriceSlots) / 2);
                // Slots end at kPriceSlots - 1. For the widest fitting
                // spread, rounding the centre down can put touch_high one
                // slot past the end. Raise the base enough to include it;
                // the width check above guarantees this stays <= touch_low.
                next_base = std::max(next_base,
                    touch_high - static_cast<Tick>(kPriceSlots - 1));
                move_band = true;
            }
        }
        auto in_band = [&](Tick tick) {
            return tick >= next_base && tick - next_base < static_cast<Tick>(kPriceSlots);
        };

        // Clear OLD indexes before changing the base. No whole-band shift.
        for (std::size_t j = 0; j < old_bid_count_; ++j)
            bid_qty_[old_bid_slots_[j]] = 0;
        for (std::size_t j = 0; j < old_ask_count_; ++j)
            ask_qty_[old_ask_slots_[j]] = 0;

        if (move_band) {
            if (based_) ++recenter_count_; // Initial placement is not a recenter.
            base_tick_ = next_base;
            based_ = true;
        }

        // Levels outside the band are skipped (and counted); the touch is
        // always inside it, so top() stays exact.
        best_bid_slot_ = best_ask_slot_ = -1;
        std::size_t kept_bids = 0, kept_asks = 0;
        for (std::size_t j = 0; j < snapshot.bid_count; ++j) {
            if (!in_band(bid_ticks[j])) { ++dropped_levels_; continue; }
            const auto slot = static_cast<std::size_t>(bid_ticks[j] - base_tick_);
            // Duplicate entries at the same tick aggregate within THIS snapshot.
            bid_qty_[slot] += snapshot.bids[j].quantity;
            old_bid_slots_[kept_bids++] = slot;
            best_bid_slot_ = std::max(best_bid_slot_, static_cast<int>(slot));
        }
        for (std::size_t j = 0; j < snapshot.ask_count; ++j) {
            if (!in_band(ask_ticks[j])) { ++dropped_levels_; continue; }
            const auto slot = static_cast<std::size_t>(ask_ticks[j] - base_tick_);
            ask_qty_[slot] += snapshot.asks[j].quantity;
            old_ask_slots_[kept_asks++] = slot;
            if (best_ask_slot_ < 0 || static_cast<int>(slot) < best_ask_slot_)
                best_ask_slot_ = static_cast<int>(slot);
        }
        old_bid_count_ = kept_bids;
        old_ask_count_ = kept_asks;
        status_ = Status::Ok;
        return true;
    }

    bool matches(const BookSnapshotMsg& snapshot) const {
        if (!valid()) return false;
        const double half_tick = tick_size_ * 0.5;
        const Top touch = top();
        const double want_bid = snapshot.bid_count > 0 ? snapshot.best_bid : 0.0;
        const double want_ask = snapshot.ask_count > 0 ? snapshot.best_ask : 0.0;
        if (std::abs(touch.bid_price - want_bid) > half_tick ||
            std::abs(touch.ask_price - want_ask) > half_tick)
            return false;
        return side_matches(snapshot.bids, snapshot.bid_count, bid_qty_, old_bid_count_) &&
               side_matches(snapshot.asks, snapshot.ask_count, ask_qty_, old_ask_count_);
    }

    bool valid() const { return status_ == Status::Ok; }
    Status status() const { return status_; }
    std::uint64_t recenter_count() const { return recenter_count_; }
    // Levels skipped because they sat outside the band around the touch.
    std::uint64_t dropped_levels() const { return dropped_levels_; }

    // Constant work: only cached slots are read, even when the band has gaps.
    Top top() const {
        Top result;
        if (!valid()) return result;
        if (best_bid_slot_ >= 0) {
            result.bid_price = price_of(best_bid_slot_);
            result.bid_quantity = bid_qty_[static_cast<std::size_t>(best_bid_slot_)];
        }
        if (best_ask_slot_ >= 0) {
            result.ask_price = price_of(best_ask_slot_);
            result.ask_quantity = ask_qty_[static_cast<std::size_t>(best_ask_slot_)];
        }
        return result;
    }

    Quantity bid_quantity(double price) const {
        std::size_t slot = 0;
        return valid() && slot_of(price, slot) ? bid_qty_[slot] : 0;
    }
    Quantity ask_quantity(double price) const {
        std::size_t slot = 0;
        return valid() && slot_of(price, slot) ? ask_qty_[slot] : 0;
    }

private:
    bool side_matches(const std::array<PriceLevel, kMaxSnapshotLevels>& levels,
                      std::size_t count,
                      const std::array<Quantity, kPriceSlots>& stored,
                      std::size_t stored_count) const {
        std::size_t in_band = 0;
        for (std::size_t j = 0; j < count; ++j) {
            std::size_t slot = 0;
            if (!slot_of(levels[j].price, slot)) continue;
            ++in_band;
            Quantity expected = 0;
            for (std::size_t k = 0; k < count; ++k) {
                std::size_t other = 0;
                if (slot_of(levels[k].price, other) && other == slot)
                    expected += levels[k].quantity;
            }
            if (stored[slot] != expected) return false;
        }
        return in_band == stored_count;
    }

    // The wire format uses doubles. The internal grid uses integer ticks.
    // Assumes a zero-origin tick grid; tolerance is measured in ticks.
    bool to_ticks(double price, Tick& tick) const {
        if (!std::isfinite(price) || price <= 0.0) return false;
        const double scaled = price / tick_size_;
        constexpr double max_exact_ticks = 4503599627370495.0;
        if (!std::isfinite(scaled) || scaled < 0.0 || scaled > max_exact_ticks)
            return false;
        const double rounded = std::round(scaled);
        if (rounded < 1.0 || std::abs(scaled - rounded) > 1e-6) return false;
        tick = static_cast<Tick>(rounded);
        return true;
    }

    bool slot_of(double price, std::size_t& slot) const {
        Tick tick = 0;
        if (!based_ || !to_ticks(price, tick) || tick < base_tick_) return false;
        const Tick offset = tick - base_tick_;
        if (offset >= static_cast<Tick>(kPriceSlots)) return false;
        slot = static_cast<std::size_t>(offset);
        return true;
    }
    double price_of(int slot) const {
        return static_cast<double>(base_tick_ + static_cast<Tick>(slot)) * tick_size_;
    }

    static_assert(kPriceSlots <= static_cast<std::size_t>(std::numeric_limits<int>::max()));
    static_assert(kMaxSnapshotLevels <= static_cast<std::size_t>(
        std::numeric_limits<Quantity>::max() / std::numeric_limits<int>::max()));

    // Price-grid size provided at construction; prices are converted to integer
    // ticks using this value before indexing the arrays.
    double tick_size_;

    // Tick represented by slot zero. A slot represents base_tick_ + its index.
    // based_ stays false until the first non-empty touch establishes this band.
    Tick base_tick_ = 0;
    bool based_ = false;

    // Result of the latest update; queries expose data only while this is Ok.
    Status status_ = Status::NoSnapshot;

    // Cumulative diagnostics for band moves and depth levels skipped outside it.
    std::uint64_t recenter_count_ = 0;
    std::uint64_t dropped_levels_ = 0;

    // Cached offsets of the current best bid and ask, or -1 if that side is empty.
    int best_bid_slot_ = -1;
    int best_ask_slot_ = -1;

    // Quantities indexed by price offset from base_tick_. Keeping both sides in
    // fixed arrays makes top-of-book and level reads bounded and allocation-free.
    std::array<Quantity, kPriceSlots> bid_qty_{};
    std::array<Quantity, kPriceSlots> ask_qty_{};

    // Slot indices touched by the previous snapshot, used to clear only those
    // quantities before applying the next snapshot instead of clearing the band.
    std::array<std::size_t, kMaxSnapshotLevels> old_bid_slots_{};
    std::array<std::size_t, kMaxSnapshotLevels> old_ask_slots_{};

    // Number of entries in the corresponding old_*_slots_ arrays to clear.
    std::size_t old_bid_count_ = 0;
    std::size_t old_ask_count_ = 0;
};

} // namespace arena
