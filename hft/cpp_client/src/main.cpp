// Live: receive callbacks publish owned events to one strategy worker.
// Replay: the same strategy processes events inline for per-line responses.
// main.cpp — AlgoArena HFT example bot.
//
// This is the file a student starts from. It defines one concrete HFTBot,
// `SpreadCaptureBot`, wires Ctrl-C to a clean shutdown, and runs it.
//
// The example is a deliberately simple SPREAD-CAPTURE / momentum bot so that
// `hft_bot` does something out of the box. Replace the body of on_book() with
// your own edge — that is the whole assignment. Everything below the strategy
// class is plumbing you should not need to touch.
#include <memory>
#include <cstdint>
#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <cmath>
#include <array>
#include <string_view>
#include <cstring>
#include <optional>
#include <stdexcept>
#include "hft_bot.hpp"
#include "local_book.hpp"
#include "symbol_registry.hpp"
#include "queue_tracker.hpp"
#include "spread_capture_bot.hpp"
#include "../../../include/order_book.hpp"
// ═════════════════════════════════════════════════════════════════════════════
// EXAMPLE STRATEGY — replace this with your own.
//
// Spread capture with a momentum tilt: on every book update, if the spread is
// wide enough to be worth crossing and price has ticked in one direction since
// the last snapshot, take the near side for a small clip. Flat inventory cap so
// the demo never runs away. ~30 lines of actual logic.
// ═════════════════════════════════════════════════════════════════════════════
class SpreadCaptureBot : public arena::HFTBot<SpreadCaptureBot> {
public:
    using arena::HFTBot<SpreadCaptureBot>::HFTBot;
    friend class arena::HFTBot<SpreadCaptureBot>;
    friend class arena::ArenaClient<SpreadCaptureBot>;

private:
    struct SymbolState;

private:
    bool on_depth(const arena::BookSnapshotMsg& snapshot){
    // Invalid snapshots must invalidate existing books, but should not
    // consume registry slots for previously unseen symbols.
    if (!snapshot.depth_valid) {
        const std::size_t id = symbol_lookup_.find(snapshot.symbol);

        if (id != SymbolLookup::npos) {
            auto& state = (*states_)[id];

            if (state.local_book)
                state.local_book->update(snapshot);
        }
        if (check_book_) ++book_rejected_;
        return false;
    }

    // Find an existing symbol or register a new one, including an IPO.
    auto* state = state_for(snapshot.symbol);

    if (!state || !state->local_book)
        return false;

    const bool applied = state->local_book->update(snapshot);
    if (check_book_) {
        if (!applied) {
            ++book_rejected_;
        } else {
            ++book_checked_;
            if (state->local_book->matches(snapshot)) {
                ++book_matched_;
            } else if (book_checked_ - book_matched_ <= 5) {
                std::cerr << "[book-check] mismatch " << snapshot.symbol << "\n";
            }
        }
    }
    return applied;
    }

    void on_book(const std::string& symbol, double bid, double ask, double mid,
                 double microprice, double obi) {
        auto* state = state_for(symbol);

        if (!state || !state->local_book || !state->local_book->valid())
            return;

        // Constant-time read: no scan of the depth arrays.
        const auto top = state->local_book->top();

        // Use the prices maintained by our flat book.
        bid = top.bid_price;
        ask = top.ask_price;

        if (bid <= 0.0 || ask <= bid)
        return; // The current strategy needs both sides.

        const double spread = ask - bid;

        // One price-pressure signal:
        // EMA is O(1) per update and per-symbol storage; no history/window scan.
        if (std::isfinite(mid) && std::isfinite(microprice) &&
            std::isfinite(obi) && std::isfinite(spread)) {
            constexpr double kSignalAlpha = 0.1;
            const double raw_signal =
                0.5 * (microprice - mid) + 0.25 * spread * obi;
            if (!state->has_signal) {
                state->signal_ema = raw_signal;
                state->has_signal = true;
            } else {
                state->signal_ema +=
                    kSignalAlpha * (raw_signal - state->signal_ema);
            }
        }
        const int pos = state->position;
        const bool tradable = spread >= kEdge;
        const double margin = kSignalFrac * spread;
        const bool want_buy = tradable && state->has_signal &&
                              state->signal_ema > margin && pos < kMaxPos;
        const bool want_sell = tradable && state->has_signal &&
                               state->signal_ema < -margin && pos > -kMaxPos;

        manage_quote(*state, symbol, true, want_buy, bid, top.bid_quantity);
        manage_quote(*state, symbol, false, want_sell, ask, top.ask_quantity);
    }

    void manage_quote(SymbolState& state, const std::string& symbol, bool is_buy,
                      bool want, double touch_price, std::int64_t touch_qty) {
        Quote& q = state.quotes[is_buy ? 0 : 1];

        if (q.cooldown > 0) --q.cooldown;

        if (q.waiting) {
            if (++q.wait_ticks <= kAckWaitTicks) return;
            q.waiting = false;
        }

        if (!q.id.empty()) {
            const arena::TrackedOrder* order = queue_.find(q.id);
            if (!order) {
                q.id.clear();
            } else {
                const bool stale =
                    std::abs(order->price - touch_price) > 0.5 * ::TICK;
                const bool deep = order->queue_ahead > kMaxQueueAhead;
                if (want && !stale && !deep) return;
                cancel_order(q.id, symbol);
                queue_.remove(q.id);
                q.id.clear();
                if (deep) {
                    q.cooldown = kRequoteCooldownTicks;
                    return;
                }
            }
        }

        if (!want || q.cooldown > 0 || touch_qty > kMaxJoinQty) return;

        if (is_buy) buy_limit(symbol, kClip, touch_price);
        else        sell_limit(symbol, kClip, touch_price);
        q.waiting = true;
        q.wait_ticks = 0;
    }

    void on_fill(const std::string& side, const std::string& symbol,
                 int quantity, double price, bool maker) {
        if (side != "buy" && side != "sell") return;
        if (quantity <= 0) return;

        auto* state = state_for(symbol);
        if (!state) return;

        state->position += (side == "buy") ? quantity : -quantity;
    }

    void on_ack(const std::string& symbol, const std::string& order_id,
                bool is_buy, double price, int queue_ahead, int level_qty) {
        queue_.on_ack(symbol, order_id, is_buy, price, queue_ahead, level_qty);
        if (auto* state = state_for(symbol)) {
            Quote& q = state->quotes[is_buy ? 0 : 1];
            q.id = order_id;
            q.waiting = false;
        }

        std::cerr << "[ack] " << symbol << " " << order_id
                  << " ahead=" << queue_ahead << " level=" << level_qty
                  << " tracked=" << queue_.size() << "\n";
    }

    void on_queue(const std::string& symbol, const std::string& order_id,
                  int queue_ahead, int level_qty) {
        queue_.on_queue(order_id, queue_ahead, level_qty);
        std::cerr << "[queue] " << symbol << " " << order_id
                  << " ahead=" << queue_ahead << " level=" << level_qty << "\n";
    }

    void on_session(const std::string& event, const std::string& message) {
        std::cerr << "[session] " << event << " — " << message << "\n";
        // TODO(student): decide what to do on SESSION_CLOSED (e.g. flatten).
        if (event == "SESSION_CLOSED") {
            queue_.clear();   // resting orders die with the session
            for (std::size_t i = 0; i < state_count_; ++i)
                (*states_)[i].quotes = {};
            for (std::size_t i = 0; i < state_count_; ++i) {
                const auto& state = (*states_)[i];
                const int qty = state.position;
                if (qty == 0) continue;
                const std::string sym(state.symbol.data(), state.symbol_size);
                if (qty > 0)      sell_market(sym, qty);
                else if (qty < 0) buy_market(sym, -qty);
            }
        }
    }

    static constexpr int kClip = 1;
    static constexpr int kMaxPos = 5;
    static constexpr double kEdge = 0.02;
    static constexpr double kSignalFrac = 0.1;
    static constexpr int kMaxQueueAhead = 200;
    static constexpr std::int64_t kMaxJoinQty = 200;
    static constexpr int kAckWaitTicks = 100;
    static constexpr int kRequoteCooldownTicks = 50;

    static constexpr std::size_t kMaxSymbols = 64;
    static constexpr std::size_t kMaxSymbolBytes = 16;

    using SymbolLookup = arena::SymbolRegistry<128, kMaxSymbolBytes>;

    struct Quote {
        std::string id;
        bool waiting = false;
        int wait_ticks = 0;
        int cooldown = 0;
    };

    struct SymbolState {
        std::array<char, kMaxSymbolBytes> symbol{};
        std::size_t symbol_size = 0;
        int position = 0;
        double signal_ema = 0.0;
        bool has_signal = false;
        std::array<Quote, 2> quotes;
        double tick_size = 0.0;
        // Storage is inside SymbolState; optional does not allocate on the heap.
        // The book is constructed only after we know this symbol's price grid.
        std::optional<arena::LocalBook> local_book;
    };


    // Allocate all state storage once during bot construction, off the stack.
    // The optional books are constructed in place when symbols are first seen.
    std::unique_ptr<std::array<SymbolState, kMaxSymbols>> states_ =
    std::make_unique<std::array<SymbolState, kMaxSymbols>>();

    SymbolLookup symbol_lookup_;
    arena::QueueTracker queue_;   // our resting orders' FIFO standing
    std::size_t state_count_ = 0;
    std::uint64_t symbol_registration_failures_ = 0;
    bool check_book_ = false;
    std::uint64_t book_checked_ = 0;
    std::uint64_t book_matched_ = 0;
    std::uint64_t book_rejected_ = 0;

    SymbolState* state_for(std::string_view symbol) {
        if (symbol.empty() || symbol.size() > kMaxSymbolBytes){
            ++symbol_registration_failures_;
            return nullptr;
        }

        const std::size_t existing = symbol_lookup_.find(symbol);
        if (existing != SymbolLookup::npos)
            return &(*states_)[existing];

        // Reject new symbols because every per-symbol state slot is in use.
        if (state_count_ == states_->size()) {
            ++symbol_registration_failures_;
            return nullptr;
        }

        // A previously unseen symbol, including an IPO: reserve the next state.
        const std::size_t id = state_count_;
        auto& state = (*states_)[id];
        state.local_book.emplace(::TICK);
        state.tick_size = ::TICK;

        // Publish the ID only after the book has been successfully constructed.
        if (!symbol_lookup_.insert(symbol, id)) {
            state.local_book.reset();
            state.tick_size = 0.0;
            ++symbol_registration_failures_;
            return nullptr;
        }

        std::memcpy(state.symbol.data(), symbol.data(), symbol.size());
        state.symbol_size = symbol.size();
        ++state_count_;
        return &state;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Plumbing: signal-driven shutdown + entry point. Students need not edit below.
// ─────────────────────────────────────────────────────────────────────────────

namespace {
std::atomic<arena::SpreadCaptureBot*> g_bot{nullptr};
static_assert(decltype(g_bot)::is_always_lock_free);
struct SignalRegistration {
    explicit SignalRegistration(arena::SpreadCaptureBot& bot) { g_bot.store(&bot); }
    ~SignalRegistration() { g_bot.store(nullptr); }
};
void handle_signal(int) {
    if (auto* b = g_bot.load()) b->stop();
}
}  // namespace

int main(int argc, char** argv) {
    const arena::ClientConfig cfg = arena::ClientConfig::from_env();
    bool replay = false;
    bool check_book = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--replay") replay = true;
        if (std::string(argv[i]) == "--check-book") check_book = true;
    }

    arena::SpreadCaptureBot bot(cfg);
    if (check_book) bot.enable_book_check();

    if (replay) {
        // Offline latency benchmark: driven by scripts/latency_replay.py over
        // stdin/stdout. No socket, no signals — just the tape through the bot.
        bot.run_replay(std::cin, std::cout);
        bot.report_book_check();
        return 0;
    }

    std::cerr << "[main] AlgoArena HFT bot — team=" << cfg.team_id
              << " url=" << cfg.url << "\n";
    SignalRegistration registration(bot);
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    bot.run();   // blocks until Ctrl-C
    g_bot.store(nullptr);
    std::cerr << "[main] shutting down\n";
    bot.report_book_check();
    return 0;
}
