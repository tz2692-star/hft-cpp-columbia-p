// ─────────────────────────────────────────────────────────────────────────────
// hft_bot.hpp — the STUDENT-FACING base class for an AlgoArena HFT bot.
//
// This is the C++ analogue of trader/trader.py's Strategy class: it is the one
// surface a student is meant to edit. Subclass HFTBot, override the hooks you
// care about, and call run().
//
// HFTBot wires the transport-layer callbacks in ArenaClient to three clean
// strategy hooks and handles the two pieces of plumbing every bot needs:
//   * it will not trade until the session is open (SESSION_OPEN gate);
//   * it measures tick-to-order latency for you — every time you send an order
//     from inside on_book(), the elapsed microseconds from snapshot-arrival to
//     order-send are recorded (this is the graded axis of the course).
//
// To send orders from a hook, call the protected helpers:
//   buy_limit / sell_limit / buy_market / sell_market / cancel_order.
// Sending from on_book() is what gets latency-stamped; sending elsewhere is
// fine but is not part of the tick-to-order measurement.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <string>

#include "arena_client.hpp"
#include "arena_client_impl.hpp"

namespace arena {


template <class Derived>
class HFTBot : public ArenaClient <Derived> {
using Base = ArenaClient<Derived>;

public:
    friend class ArenaClient<Derived>;

    explicit HFTBot(ClientConfig cfg) : Base(std::move(cfg)) {}

    // Non-virtual entry point: connect and run forever (reconnecting on drop).
    // Do not override — override the hooks below instead.
    void run() { Base::run(); }

protected:
    // ═════════════════════════════════════════════════════════════════════════
    // STRATEGY HOOKS — OVERRIDE THESE. This is your competitive edge.
    // ═════════════════════════════════════════════════════════════════════════

    // Top-of-book update for `symbol`. bid/ask are 0.0 when that side is empty;
    // `mid` is the exchange-reported mid price. Send orders from here to have
    // them latency-stamped. Do not block — you are on the receive thread.
    //
    // TODO(student): put your signal + order logic here. See src/main.cpp for a
    // worked spread-capture example you can replace.
    // microprice = depth-weighted touch; obi = order-book imbalance in [-1,1]
    // (+ = bids heavy). Cheap alpha inputs — but every microsecond you spend
    // computing on them costs queue position (the M6 "signals in a budget"
    // tradeoff). bid/ask are 0.0 when that side is empty.
    //
    //
    using typename Base::clock;
    using typename Base::time_point;
    using Base::place_limit;
    using Base::place_market;
    using Base::cancel;
    using Base::session_open;
    using Base::record_latency;
    //
    //
    bool on_depth(const BookSnapshotMsg& snapshot){
    return snapshot.depth_valid;
    }

    void on_book(const std::string& symbol, double bid, double ask,
                         double mid, double microprice, double obi) {
        (void)symbol; (void)bid; (void)ask; (void)mid; (void)microprice; (void)obi;
    }


    // A fill involving us. `side` is "buy"/"sell" from our perspective, or ""
    // for a market print that is not ours.
    //
    // TODO(student): track inventory / realised P&L here if your strategy needs
    // it beyond the authoritative on_portfolio() push.
    void on_fill(const std::string& side, const std::string& symbol,
                         int quantity, double price, bool maker) {
        (void)side; (void)symbol; (void)quantity; (void)price; (void)maker;
    }

    // Session lifecycle and market events (SESSION_OPEN, SESSION_CLOSED, SHOCK,
    // CALENDAR, DIVIDEND, …). The base class already flattens is NOT automatic
    // here — decide for yourself what to do at close.
    //
    // TODO(student): react to shocks / calendar events, or flatten on close.
    void on_session(const std::string& event, const std::string& message) {
        (void)event; (void)message;
    }

    // Our order rested — where we landed in the FIFO queue at our price level.
    // queue_ahead shares fill before us; level_qty is the whole level. Front of
    // the queue is queue_ahead == 0. Both 0 if the order fully filled.
    //
    // TODO(student): use queue position to decide hold vs reprice — repricing
    // (cancel + new) sends you to the BACK of the queue.
    void on_ack(const std::string& symbol, const std::string& order_id,
                        bool is_buy, double price, int queue_ahead, int level_qty) {
        (void)symbol; (void)order_id; (void)is_buy; (void)price;
        (void)queue_ahead; (void)level_qty;
    }

    // Our queue standing moved: a fill or cancel ahead of us advanced our order.
    //
    // TODO(student): watch this to know when you have reached the front (and are
    // about to get filled — which is also when adverse selection bites).
    void on_queue(const std::string& symbol, const std::string& order_id,
                          int queue_ahead, int level_qty) {
        (void)symbol; (void)order_id; (void)queue_ahead; (void)level_qty;
    }

    // ── Order helpers (call these from your hooks) ───────────────────────────
    // The *_limit helpers on the book path are the ones that get latency-timed.

    void buy_limit(const std::string& symbol,
               int qty, double price) {
    stamp_and_send([&] {
        place_limit(symbol, buy_side_, qty, price);
    });
    }

    void sell_limit(const std::string& symbol,
                    int qty, double price) {
        stamp_and_send([&] {
            place_limit(symbol, sell_side_, qty, price);
        });
    }

    void buy_market(const std::string& symbol, int qty) {
        stamp_and_send([&] {
            place_market(symbol, buy_side_, qty);
        });
    }

    void sell_market(const std::string& symbol, int qty) {
        stamp_and_send([&] {
            place_market(symbol, sell_side_, qty);
        });
    }
    void cancel_order(const std::string& order_id, const std::string& symbol) {
        cancel(order_id, symbol);
    }

private:
    // Bridge ArenaClient's book callback to on_book() and drive the latency
    // instrumentation: we remember the snapshot arrival time for the duration
    // of the on_book() call, and any order sent through the helpers above times
    // itself against that stamp.
    void on_book_snapshot(const BookView& bv, time_point recv_time) {
        tick_recv_    = recv_time;
        tick_symbol_  = bv.symbol;
        in_book_call_ = true;
        if (session_open()) {
            static_cast<Derived&>(*this).on_book(bv.symbol, bv.best_bid, bv.best_ask, bv.mid,
                    bv.microprice, bv.obi);
        }
        in_book_call_ = false;
    }

    // Bridge the queue callbacks to the student-facing on_ack / on_queue.
    void on_order_ack(const std::string& order_id, const std::string& symbol,
                      const std::string& side, double price, int /*quantity*/,
                      int queue_ahead, int level_qty) {
        static_cast<Derived&>(*this).on_ack(symbol, order_id, side == "buy", price,
                                            queue_ahead, level_qty);
    }
    void on_queue_update(const std::string& order_id, const std::string& symbol,
                         const std::string& /*side*/, double /*price*/,
                         int queue_ahead, int level_qty) {
        static_cast<Derived&>(*this).on_queue(symbol, order_id, queue_ahead, level_qty);
    }

    // Time the order send against the current tick's arrival stamp.
    template <typename SendFn>
    void stamp_and_send(SendFn&& send) {
        if (in_book_call_) {
            send();
            const auto now   = clock::now();
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                    now - tick_recv_).count();
            record_latency(tick_symbol_, static_cast<long long>(micros));
        } else {
            // Sent outside the book path (e.g. from on_session) — no tick to
            // measure against, so just send.
            send();
        }
    }

    time_point   tick_recv_{};
    std::string  tick_symbol_;
    bool       in_book_call_ = false;
    const std::string buy_side_{"buy"};
    const std::string sell_side_{"sell"};
};

}  // namespace arena
