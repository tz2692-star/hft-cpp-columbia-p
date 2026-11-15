// ─────────────────────────────────────────────────────────────────────────────
// messages.hpp — one plain struct per inbound message type.
//
// These replace the generic nlohmann::json object. Each struct holds only the
// fields we actually use. Text fields are std::string_view: they point into
// the raw message text instead of copying it, so they never allocate memory.
//
// WARNING: a string_view is only valid while the raw message text is alive,
// i.e. during ArenaClient::dispatch(). Never store one for later — copy it
// into a std::string or char array if you need to keep it.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <string_view>
#include <variant>
#include <array>
#include <cstddef>

namespace arena
{
    // Initial capacity per side; verify against the exchange's depth limit.
    inline constexpr std::size_t kMaxSnapshotLevels = 64;

    // One aggregated price level from an incoming snapshot.
    struct PriceLevel {
        double price = 0.0;  // Price in the same units used by the wire protocol.
        int quantity = 0;    // Total shares reported at this price.
    };
    // "book_snapshot" - top of book for one symbol.
    struct BookSnapshotMsg
    {
        std::string_view symbol;
        std::string_view asset_type;
        double best_bid = 0.0;
        double best_ask = 0.0;
        double mid = 0.0;
        double spread = 0.0;
        double ref = 0.0;
        double microprice = 0.0;
        double obi = 0.0;
        // Snapshot levels; only entries below the corresponding count are valid.
        std::array<PriceLevel, kMaxSnapshotLevels> bids{};
        std::array<PriceLevel, kMaxSnapshotLevels> asks{};

        // Number of populated entries on each side.
        std::size_t bid_count = 0;
        std::size_t ask_count = 0;

        // True only after both sides have been decoded without errors or overflow.
        bool depth_valid = false;
    };

    // "trade_execution" - a fill (ours or someone else's).
    struct TradeMsg
    {
        std::string_view symbol;
        std::string_view buyer_id;
        std::string_view seller_id;
        std::string_view maker_id;
        int quantity = 0;
        double price = 0.0;
    };

    // "portfolio_update" - our account state, pushed the exchange.
    struct PortfolioMsg
    {
        double cash = 0.0;
        double net_worth = 0.0;
        double realized_pnl = 0.0;
        double unrealized_pnl = 0.0;
    };

    // "session_event"
    struct SessionMsg
    {
        std::string_view event;
        std::string_view message;
    };

    // "order_ack"
    struct OrderAckMsg
    {
        std::string_view order_id;
        std::string_view symbol;
        std::string_view side;
        double price = 0.0;
        int quantity = 0;
        int queue_ahead = 0;
        int level_qty = 0;
    };

    // "queue_update"
    struct QueueUpdateMsg
    {
        std::string_view order_id;
        std::string_view symbol;
        std::string_view side;
        double price = 0.0;
        int queue_ahead = 0;
        int level_qty = 0;
    };

    // "error"
    struct ErrorMsg
    {
        std::string_view code;
        std::string_view message;
    };

    // Any message type we don't care about (leaderboard, command_ack, ...).
    struct UnknownMsg
    {
    };

    // One box that holds exactly ONE of the message types above.
    using Inbound = std::variant<BookSnapshotMsg, TradeMsg, PortfolioMsg, SessionMsg,
                                 OrderAckMsg, QueueUpdateMsg, ErrorMsg, UnknownMsg>;

} // namespace arena