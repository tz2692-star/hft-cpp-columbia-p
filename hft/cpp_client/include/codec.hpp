// ─────────────────────────────────────────────────────────────────────────────
// codec.hpp — turn raw message text into our message structs.
//
// Uses the helpers in json_scan.hpp to pull out only the fields we need.
// No JSON tree is built and no memory is allocated.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <string_view>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <system_error>

#include "json_scan.hpp"    
#include "messages.hpp"

namespace arena
{
    namespace detail {
        // Prefer from_chars when this standard library has floating-point support.
        template<typename Number>
        inline auto parse_book_number(const char* first, const char* last,
                                      Number& value, int)
            -> decltype(std::from_chars(first, last, value))
        {
            return std::from_chars(first, last, value);
        }

        // Older Apple libc++ lacks that overload. Copy only the bounded numeric
        // token to the stack so strtod never reads past the input string_view.
        inline std::from_chars_result parse_book_number(
            const char* first, const char* last, double& value, long)
        {
            if (first == last || *first == '+')
                return {first, std::errc::invalid_argument};

            char token[128]; // Reject unusually long numeric tokens; no heap use.
            std::size_t length = 0;
            while (first + length != last) {
                const char c = first[length];
                if (!((c >= '0' && c <= '9') || c == '.' || c == '-' ||
                      c == '+' || c == 'e' || c == 'E'))
                    break;
                if (length == sizeof(token) - 1)
                    return {first + length, std::errc::result_out_of_range};
                token[length++] = c;
            }
            token[length] = '\0';

            char* end = nullptr;
            errno = 0;
            const double parsed = std::strtod(token, &end);
            const char* next = first + (end - token);
            if (end == token)
                return {first, std::errc::invalid_argument};
            if (errno == ERANGE || !std::isfinite(parsed))
                return {next, std::errc::result_out_of_range};
            value = parsed;
            return {next, std::errc{}};
        }
    } // namespace detail

    // Read the best (first) price from a list like:
    //     "bids": [[100.4, 30], [100.3, 50], ...]
    // Returns 0.0 if the list is empty ("bids": []) or missing.
    inline double read_best_price(std::string_view s, std::string_view quoted_key)
    {
        std::size_t i = find_value(s, quoted_key); // points at the outer '['
        if (i == kNotFound || s[i] != '[')
            return 0.0;

        i = skip_ws(s, i + 1);            // step inside the outer list
        if (i >= s.size() || s[i] != '[') // no inner '[' means "[]" (empty)
            return 0.0;

        i = skip_ws(s, i + 1);    // step inside the first pair
        return read_double(s, i); // read the price
    }

    // Read one side of the snapshot into caller-owned storage.
    // This is a specialized parser for the exchange's level-array format.
    inline bool read_levels(
        std::string_view s,
        std::string_view key,
        std::array<PriceLevel, kMaxSnapshotLevels>& levels,
        std::size_t& count)
    {
        count = 0; // Publish a count only after the entire side succeeds.

        std::size_t i = find_value(s, key);
        if (i == kNotFound || i >= s.size())
            return false;

        // Advance over JSON whitespace at the current cursor.
        auto skip_space = [&]() {
            while (i < s.size() &&
                (s[i] == ' ' || s[i] == '\t' ||
                    s[i] == '\r' || s[i] == '\n'))
                ++i;
        };

        // Consume an expected punctuation character, if present.
        auto take = [&](char expected) {
            skip_space();
            if (i >= s.size() || s[i] != expected)
                return false;
            ++i;
            return true;
        };

        if (!take('['))
            return false;

        if (take(']'))
            return true; // [] is a valid empty side.

        std::size_t used = 0;

        while (true) {
            if (used == levels.size())
                return false; // Reject overflow instead of truncating.

            if (!take('['))
                return false; // Each level must begin with '['.

            skip_space();
            double price = 0.0;
            const auto price_result = detail::parse_book_number(
                s.data() + i, s.data() + s.size(), price, 0);

            if (price_result.ec != std::errc{} ||
                !std::isfinite(price) || price <= 0.0)
                return false;

            i = static_cast<std::size_t>(price_result.ptr - s.data());

            if (!take(','))
                return false;

            skip_space();
            // The venue sends quantities as floats ("10.0"), so parse a
            // double and accept it only if it is a whole, positive int.
            double raw_quantity = 0.0;
            const auto qty_result = detail::parse_book_number(
                s.data() + i, s.data() + s.size(), raw_quantity, 0);

            if (qty_result.ec != std::errc{} ||
                !(raw_quantity >= 1.0 && raw_quantity <= 2147483647.0) ||
                raw_quantity != std::floor(raw_quantity))
                return false;

            const int quantity = static_cast<int>(raw_quantity);

            i = static_cast<std::size_t>(qty_result.ptr - s.data());

            if (!take(']'))
                return false; // Also rejects extra fields in a level.

            levels[used++] = PriceLevel{price, quantity};

            if (take(']')) {
                count = used; // Commit the successful result.
                return true;
            }

            if (!take(','))
                return false; // Another level must follow a comma.
        }
    }

    inline BookSnapshotMsg decode_book(std::string_view s)
    {
        BookSnapshotMsg b;

        // Preserve the existing metadata and supplied signal inputs.
        b.symbol = read_string(s, find_value(s, "\"symbol\""));
        b.asset_type = read_string(s, find_value(s, "\"asset_type\""));
        b.mid = read_double(s, find_value(s, "\"mid_price\""));
        b.spread = read_double(s, find_value(s, "\"spread\""));
        b.ref = read_double(s, find_value(s, "\"ref_price\""));
        b.microprice = read_double(s, find_value(s, "\"microprice\""));
        b.obi = read_double(s, find_value(s, "\"obi\""));

        // Decode both sides into the message's fixed-capacity arrays.
        const bool bids_ok = read_levels(s, "\"bids\"", b.bids, b.bid_count);
        const bool asks_ok = read_levels(s, "\"asks\"", b.asks, b.ask_count);
        b.depth_valid = bids_ok && asks_ok;

        // An invalid snapshot must not expose a partially decoded book.
        if (!b.depth_valid) {
            b.bid_count = 0;
            b.ask_count = 0;
            return b;
        }

        // Derive the best prices without assuming the levels are sorted.
        for (std::size_t j = 0; j < b.bid_count; ++j) {
            if (b.bids[j].price > b.best_bid)
                b.best_bid = b.bids[j].price;
        }

        for (std::size_t j = 0; j < b.ask_count; ++j) {
            if (b.best_ask == 0.0 || b.asks[j].price < b.best_ask)
                b.best_ask = b.asks[j].price;
        }

        return b;
    }

    // "trade_execution" → TradeMsg
    inline TradeMsg decode_trade(std::string_view s)
    {
        TradeMsg t;
        t.symbol = read_string(s, find_value(s, "\"symbol\""));
        t.buyer_id = read_string(s, find_value(s, "\"buyer_id\""));
        t.seller_id = read_string(s, find_value(s, "\"seller_id\""));
        t.maker_id = read_string(s, find_value(s, "\"maker_id\""));
        t.quantity = read_int(s, find_value(s, "\"quantity\""));
        t.price = read_double(s, find_value(s, "\"price\""));
        return t;
    }

    // "portfolio_update" → PortfolioMsg
    inline PortfolioMsg decode_portfolio(std::string_view s)
    {
        PortfolioMsg p;
        p.cash = read_double(s, find_value(s, "\"cash\""));
        p.net_worth = read_double(s, find_value(s, "\"net_worth\""));
        p.realized_pnl = read_double(s, find_value(s, "\"realized_pnl\""));
        p.unrealized_pnl = read_double(s, find_value(s, "\"unrealized_pnl\""));
        return p;
    }

    // "session_event" → SessionMsg
    inline SessionMsg decode_session(std::string_view s)
    {
        SessionMsg e;
        e.event = read_string(s, find_value(s, "\"event\""));
        e.message = read_string(s, find_value(s, "\"message\""));
        return e;
    }

    // "order_ack" → OrderAckMsg
    inline OrderAckMsg decode_ack(std::string_view s)
    {
        OrderAckMsg a;
        a.order_id = read_string(s, find_value(s, "\"order_id\""));
        a.symbol = read_string(s, find_value(s, "\"symbol\""));
        a.side = read_string(s, find_value(s, "\"side\""));
        a.price = read_double(s, find_value(s, "\"price\""));
        a.quantity = read_int(s, find_value(s, "\"quantity\""));
        a.queue_ahead = read_int(s, find_value(s, "\"queue_ahead\""));
        a.level_qty = read_int(s, find_value(s, "\"level_qty\""));
        return a;
    }

    // "queue_update" → QueueUpdateMsg
    inline QueueUpdateMsg decode_queue(std::string_view s)
    {
        QueueUpdateMsg q;
        q.order_id = read_string(s, find_value(s, "\"order_id\""));
        q.symbol = read_string(s, find_value(s, "\"symbol\""));
        q.side = read_string(s, find_value(s, "\"side\""));
        q.price = read_double(s, find_value(s, "\"price\""));
        q.queue_ahead = read_int(s, find_value(s, "\"queue_ahead\""));
        q.level_qty = read_int(s, find_value(s, "\"level_qty\""));
        return q;
    }

    // "error" → ErrorMsg
    inline ErrorMsg decode_error(std::string_view s)
    {
        ErrorMsg e;
        e.code = read_string(s, find_value(s, "\"code\""));
        e.message = read_string(s, find_value(s, "\"message\""));
        return e;
    }

    // Read the "type" field and call the matching decoder.
    // Returns an Inbound — a box holding exactly one message struct.
    inline Inbound decode(std::string_view s)
    {
        std::string_view type = read_string(s, find_value(s, "\"type\""));

        if (type == "book_snapshot")
            return decode_book(s); // most common → check first
        if (type == "order_ack")
            return decode_ack(s);
        if (type == "queue_update")
            return decode_queue(s);
        if (type == "trade_execution")
            return decode_trade(s);
        if (type == "portfolio_update")
            return decode_portfolio(s);
        if (type == "session_event")
            return decode_session(s);
        if (type == "error")
            return decode_error(s);
        return UnknownMsg{}; // leaderboard etc. → ignore
    }

} // namespace arena
