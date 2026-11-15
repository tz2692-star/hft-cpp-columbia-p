// ─────────────────────────────────────────────────────────────────────────────
// arena_client_impl.hpp — ArenaClient<T> member definitions.
//
// ArenaClient is a template, so its definitions must be visible wherever it is
// instantiated. They live here, included at the bottom of arena_client.hpp.
// Do not include this file directly.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "arena_client.hpp"
#include "order_message_pool.hpp"
#include "messages.hpp"
#include "codec.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <type_traits>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <nlohmann/json.hpp>
namespace arena {

template <typename T>
ArenaClient<T>::ArenaClient(ClientConfig cfg) : cfg_(std::move(cfg)) {
    ws_.setUrl(cfg_.url);
    // Keepalive so a silent exchange restart is detected promptly.
    ws_.setPingInterval(15);
    // IXWebSocket auto-reconnects with backoff; cap it so we retry briskly.
    ws_.setMaxWaitBetweenReconnectionRetries(3000);  // ms
    connect_handlers();
}

template <typename T>
ArenaClient<T>::~ArenaClient() {
    ws_.stop();
}


template <typename T>
void ArenaClient<T>::connect_handlers() {
    ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        (void)detail::message_pool_for_thread();

        switch (msg->type) {
            case ix::WebSocketMessageType::Open:
                session_open_.store(false);  // re-gate on every (re)connect
                send_handshake();
                break;

            case ix::WebSocketMessageType::Message:
                dispatch(msg->str);
                break;

            case ix::WebSocketMessageType::Close:
                std::cerr << "[arena] connection closed ("
                          << msg->closeInfo.code << ": "
                          << msg->closeInfo.reason << ") — reconnecting\n";
                session_open_.store(false);
                break;

            case ix::WebSocketMessageType::Error:
                std::cerr << "[arena] socket error: "
                          << msg->errorInfo.reason << " — retrying\n";
                break;

            default:
                break;
        }
    });
}

template <typename T>
void ArenaClient<T>::send_handshake() {
    // Mirrors shared.messages.Handshake:
    //   {"type":"handshake","team_id":..,"role":"trader","level":N,"token":..}
    nlohmann::json hs = {
        {"type",    "handshake"},
        {"team_id", cfg_.team_id},
        {"role",    "trader"},
        {"level",   cfg_.level},
        {"token",   cfg_.token},
    };
    send_raw(hs.dump());
}

template <typename T>
void ArenaClient<T>::send_raw(const std::string& payload) {
    if (replay_mode_) {
        // Offline benchmark: capture the order instead of sending it.
        if (replay_order_count_ == replay_orders_.size() ||
            payload.size() > detail::kMaxWireBytes) {
            replay_overflow_ = true;
            return;
        }
        auto& order = replay_orders_[replay_order_count_++];
        std::memcpy(order.bytes.data(), payload.data(), payload.size());
        order.size = payload.size();
        return;
    }
    // IXWebSocket queues the frame; safe to call from any thread. It silently
    // drops if the socket is not open, which mirrors the Python bot's
    // "connection closed" tolerance.
    ws_.sendText(payload);
}

template <typename T>
void ArenaClient<T>::run_replay(std::istream& in, std::ostream& out) {
    // No socket, no session gating — feed the tape straight through the
    // strategy and time each tick->order path. One output line per input.
    (void)detail::message_pool_for_thread();
    replay_mode_ = true;
    session_open_.store(true);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) { out << "[]\n"; out.flush(); continue; }
        replay_order_count_ = 0;
        replay_overflow_ = false;
        dispatch(line);                 // stamps recv, runs on_book, buffers orders + LAT
        if (replay_overflow_) {
            // Reject the run rather than report a truncated tick as successful.
            throw std::runtime_error("replay order capture capacity exceeded");
        }
        out << "[";
        for (size_t i = 0; i < replay_order_count_; ++i) {
            if (i) out << ",";
            const auto& order = replay_orders_[i];
            out.write(order.bytes.data(),
                      static_cast<std::streamsize>(order.size));
        }
        out << "]\n";
        out.flush();                    // the harness times until this arrives
    }
}

template <typename T>
void ArenaClient<T>::run() {
    running_.store(true);
    ws_.start();  // spins up the background receive/reconnect thread
    // Block until stop() is requested; the socket thread does the real work.
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ws_.stop();
}

template <typename T>
void ArenaClient<T>::stop() {
    running_.store(false);
}

// ── Inbound dispatch ─────────────────────────────────────────────────────────
template <typename T>
void ArenaClient<T>::dispatch(const std::string& raw) {
    const time_point recv_time = clock::now();
    const Inbound message = arena::decode(raw);

    std::visit([&](const auto& msg) {
        using M = std::decay_t<decltype(msg)>;
        auto& bot = static_cast<T&>(*this);

        if constexpr (std::is_same_v<M, BookSnapshotMsg>) {
            // Apply depth before creating the scalar view or invoking on_book().
            if (!bot.on_depth(msg))
                return; // Returns from this std::visit callback.
            BookView bv;
            bv.symbol = std::string(msg.symbol);
            bv.asset_type = msg.asset_type.empty()
                ? "equity" : std::string(msg.asset_type);
            bv.best_bid = msg.best_bid;
            bv.best_ask = msg.best_ask;
            bv.mid = msg.mid;
            bv.spread = msg.spread;
            bv.ref = msg.ref;
            bv.microprice = msg.microprice;
            bv.obi = msg.obi;

            {
                std::lock_guard<std::mutex> lock(book_mtx_);
                books_[bv.symbol] = bv;
            }

            bot.on_book_snapshot(bv, recv_time);

        } else if constexpr (std::is_same_v<M, TradeMsg>) {
            std::string side;
            if (msg.buyer_id == std::string_view(cfg_.team_id))
                side = "buy";
            else if (msg.seller_id == std::string_view(cfg_.team_id))
                side = "sell";

            const bool maker =
                msg.maker_id == std::string_view(cfg_.team_id);

            bot.on_fill(side, std::string(msg.symbol),
                        msg.quantity, msg.price, maker);

        } else if constexpr (std::is_same_v<M, PortfolioMsg>) {
            bot.on_portfolio(msg.cash, msg.net_worth,
                             msg.realized_pnl, msg.unrealized_pnl);

        } else if constexpr (std::is_same_v<M, SessionMsg>) {
            if (msg.event == "SESSION_OPEN" ||
                msg.event == "SESSION_PREOPEN") {
                session_open_.store(true);
            } else if (msg.event == "SESSION_CLOSED") {
                session_open_.store(false);
            }

            bot.on_session(std::string(msg.event),
                           std::string(msg.message));

        } else if constexpr (std::is_same_v<M, OrderAckMsg>) {
            bot.on_order_ack(
                std::string(msg.order_id),
                std::string(msg.symbol),
                std::string(msg.side),
                msg.price, msg.quantity,
                msg.queue_ahead, msg.level_qty);

        } else if constexpr (std::is_same_v<M, QueueUpdateMsg>) {
            bot.on_queue_update(
                std::string(msg.order_id),
                std::string(msg.symbol),
                std::string(msg.side),
                msg.price, msg.queue_ahead, msg.level_qty);

        } else if constexpr (std::is_same_v<M, ErrorMsg>) {
            std::cerr << "[arena] server error " << msg.code
                      << ": " << msg.message << '\n';

            bot.on_error(std::string(msg.code),
                         std::string(msg.message));

        } else {
            // UnknownMsg：
        }
    }, message);
}
// ── Send helpers — exact PlaceOrder / CancelOrder JSON ───────────────────────
template <typename T>
void ArenaClient<T>::place_limit(const std::string& symbol, const std::string& side,
                              int quantity, double price) {
    // Mirrors shared.messages.PlaceOrder (order_type="limit"). stop_price is
    // optional in the schema, so we omit it for wire compatibility.
    auto& pool = detail::message_pool_for_thread();

    if (symbol.empty() ||
        symbol.size() > detail::kMaxSymbolBytes ||
        (side != "buy" && side != "sell") ||
        quantity <= 0 ||
        !std::isfinite(price)) {
        ++pool.rejected;
        return;
    }

    auto* message = pool.try_acquire();
    if (!message) {
        return;
    }

    detail::MessageLease lease(*message);

    message->kind = detail::MessageKind::Limit;
    message->is_buy = (side == "buy");
    message->quantity = quantity;
    message->price = price;

    if (!detail::copy_text(
            symbol, message->symbol, message->symbol_size) ||
        !detail::encode_message(*message, cfg_.team_id)) {
        ++pool.rejected;
        return;
    }

    send_raw(message->wire);
}

template <typename T>
void ArenaClient<T>::place_market(const std::string& symbol, const std::string& side,
                               int quantity) {
    // price is ignored by the venue for market orders, but the schema requires
    // the field, so we send 0.0 (as the Python trader does).
    auto& pool = detail::message_pool_for_thread();

    if (symbol.empty() ||
        symbol.size() > detail::kMaxSymbolBytes ||
        (side != "buy" && side != "sell") ||
        quantity <= 0) {
        ++pool.rejected;
        return;
    }

    auto* message = pool.try_acquire();
    if (!message) {
        return;
    }

    detail::MessageLease lease(*message);

    message->kind = detail::MessageKind::Market;
    message->is_buy = (side == "buy");
    message->quantity = quantity;
    message->price = 0.0;

    if (!detail::copy_text(
            symbol, message->symbol, message->symbol_size) ||
        !detail::encode_message(*message, cfg_.team_id)) {
        ++pool.rejected;
        return;
    }

    send_raw(message->wire);
}

template <typename T>
void ArenaClient<T>::cancel(const std::string& order_id, const std::string& symbol) {
    auto& pool = detail::message_pool_for_thread();

    if (symbol.empty() ||
        symbol.size() > detail::kMaxSymbolBytes ||
        order_id.empty() ||
        order_id.size() > detail::kMaxOrderIdBytes) {
        ++pool.rejected;
        return;
    }

    auto* message = pool.try_acquire();
    if (!message) {
        return;
    }

    detail::MessageLease lease(*message);

    message->kind = detail::MessageKind::Cancel;

    if (!detail::copy_text(
            symbol, message->symbol, message->symbol_size) ||
        !detail::copy_text(
            order_id, message->order_id, message->order_id_size) ||
        !detail::encode_message(*message, cfg_.team_id)) {
        ++pool.rejected;
        return;
    }

    send_raw(message->wire);
}

// ── Book accessor ────────────────────────────────────────────────────────────
template <typename T>
bool ArenaClient<T>::book(const std::string& symbol, BookView& out) const {
    std::lock_guard<std::mutex> lk(book_mtx_);
    auto it = books_.find(symbol);
    if (it == books_.end()) return false;
    out = it->second;
    return true;
}

// ── Latency instrumentation ──────────────────────────────────────────────────
template <typename T>
void ArenaClient<T>::record_latency(const std::string& symbol, long long micros) {
    std::lock_guard<std::mutex> lk(lat_mtx_);
    if (lat_count_ == 0) {
        lat_min_ = lat_max_ = micros;
    } else {
        lat_min_ = std::min(lat_min_, micros);
        lat_max_ = std::max(lat_max_, micros);
    }
    lat_sum_ += micros;
    ++lat_count_;

    // Canonical latency line the Python harness parses (scripts/latency_report
    // .py): `LAT <symbol> <micros>`. Every 50 samples also print a running
    // summary so the tail is visible without a full log.
    std::cerr << "LAT " << symbol << " " << micros << "\n";
    if (lat_count_ % 50 == 0) {
        std::cerr << "[latency] n=" << lat_count_
                  << " avg=" << (lat_sum_ / lat_count_)
                  << " min=" << lat_min_
                  << " max=" << lat_max_ << " us\n";
    }
}

}  // namespace arena
