#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <variant>

#include "messages.hpp"

namespace arena::pipeline {

template<std::size_t Capacity>
struct FixedText {
    std::array<char, Capacity> bytes{};
    std::size_t size = 0;

    // No truncation or terminator requirement. Failure preserves old contents.
    bool assign(std::string_view input) noexcept {
        if (input.size() > Capacity) return false;
        if (!input.empty())
            std::memmove(bytes.data(), input.data(), input.size());
        size = input.size();
        return true;
    }

    // Borrow only while this object is alive and unchanged.
    std::string_view view() const noexcept { return {bytes.data(), size}; }
};

using Symbol = FixedText<16>;
using OrderId = FixedText<128>;
using TimePoint = std::chrono::steady_clock::time_point;

enum class Side : std::uint8_t { Unknown, Buy, Sell };

struct BookEvent {
    Symbol symbol;
    std::array<PriceLevel, kMaxSnapshotLevels> bids{};
    std::array<PriceLevel, kMaxSnapshotLevels> asks{};
    std::size_t bid_count = 0;
    std::size_t ask_count = 0;
    double best_bid = 0.0;
    double best_ask = 0.0;
    double mid = 0.0;
    double spread = 0.0;
    double ref = 0.0;
    double microprice = 0.0;
    double obi = 0.0;
    bool depth_valid = false;
    bool session_open_at_receive = false;
    TimePoint recv_time{};
    std::uint64_t session_generation = 0;
};

struct FillEvent {
    Symbol symbol;
    Side side = Side::Unknown;
    int quantity = 0;
    double price = 0.0;
    bool maker = false;
};

struct AckEvent {
    Symbol symbol;
    OrderId order_id;
    int queue_ahead = 0;
    int level_qty = 0;
    bool is_buy = false;
    double price = 0.0;
};

struct QueueEvent {
    Symbol symbol;
    OrderId order_id;
    int queue_ahead = 0;
    int level_qty = 0;
};

// Application bounds, not exchange guarantees. Callers must handle failed
// assignments by faulting the channel instead of publishing truncated events.
struct SessionEvent {
    FixedText<64> event;
    FixedText<512> message;
    std::uint64_t generation = 0;
};

struct ErrorEvent {
    FixedText<64> code;
    FixedText<512> message;
};

struct PortfolioEvent {
    double cash = 0.0;
    double net_worth = 0.0;
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
};

using Event = std::variant<BookEvent, FillEvent, AckEvent, QueueEvent,
                           SessionEvent, ErrorEvent, PortfolioEvent>;
static_assert(std::is_default_constructible_v<Event>);
static_assert(std::is_nothrow_copy_assignable_v<Event>);
static_assert(std::is_trivially_copyable_v<Event>);

// The decoder's string_views must never cross the thread boundary. Copy all
// fields used by the current strategy; asset_type is intentionally unused.
// Invalid depth is an event too: it invalidates the consumer's previous book.
// A bad symbol fails conversion and leaves out unchanged.
inline bool copy_book(const BookSnapshotMsg& input, TimePoint recv_time,
                      bool session_open, BookEvent& out) noexcept {
    BookEvent result{};
    if (input.symbol.empty() || !result.symbol.assign(input.symbol))
        return false;

    result.recv_time = recv_time;
    result.session_open_at_receive = session_open;
    result.best_bid = input.best_bid;
    result.best_ask = input.best_ask;
    result.mid = input.mid;
    result.spread = input.spread;
    result.ref = input.ref;
    result.microprice = input.microprice;
    result.obi = input.obi;
    result.depth_valid = input.depth_valid &&
        input.bid_count <= input.bids.size() &&
        input.ask_count <= input.asks.size();

    if (result.depth_valid) {
        result.bid_count = input.bid_count;
        result.ask_count = input.ask_count;
        for (std::size_t i = 0; i < input.bid_count; ++i)
            result.bids[i] = input.bids[i];
        for (std::size_t i = 0; i < input.ask_count; ++i)
            result.asks[i] = input.asks[i];
    }

    out = result;
    return true;
}

} // namespace arena::pipeline
