#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>

#include "fixed_queue_tracker.hpp"
#include "local_book.hpp"
#include "pipeline_events.hpp"
#include "symbol_registry.hpp"
#include "../../../include/order_book.hpp"

namespace arena {

struct StrategyOrder {
    pipeline::Symbol symbol;
    pipeline::OrderId cancel_id;
    pipeline::Side side = pipeline::Side::Unknown;
    int quantity = 0;
    double price = 0.0;
    bool market = false;
    bool session_close = false;
    pipeline::TimePoint recv_time{};
    std::uint64_t session_generation = 0;
};

// Exactly one owner: the live strategy worker, or the replay caller. No socket,
// logging, locks, or allocation in process(). Live order callbacks enqueue
// fixed-size records; a separate sender thread handles transport work.
class SpreadStrategy {
    struct Quote {
        pipeline::OrderId id;
        bool waiting = false;
        int wait_ticks = 0;
        int cooldown = 0;
    };

public:
    static constexpr std::size_t kMaxSymbols = 64;
    struct SymbolState {
        pipeline::Symbol symbol;
        std::int64_t position = 0;
        double signal_ema = 0.0;
        bool has_signal = false;
        std::array<Quote, 2> quotes{};
        std::optional<LocalBook> local_book;
    };

    template<class Send>
    void process(const pipeline::Event& event, bool allow_orders, Send&& send) {
        ++processed_;
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, pipeline::BookEvent>) {
                process_book(value, allow_orders, send);
            } else if constexpr (std::is_same_v<T, pipeline::FillEvent>) {
                if (value.quantity <= 0 || value.side == pipeline::Side::Unknown) return;
                if (auto* state = state_for(value.symbol.view()))
                    state->position += value.side == pipeline::Side::Buy
                        ? value.quantity : -static_cast<std::int64_t>(value.quantity);
            } else if constexpr (std::is_same_v<T, pipeline::AckEvent>) {
                if (!queue_.on_ack(value)) {
                    faulted_ = true;
                } else if (auto* state = state_for(value.symbol.view())) {
                    auto& quote = state->quotes[value.is_buy ? 0 : 1];
                    quote.id = value.order_id;
                    quote.waiting = false;
                }
            } else if constexpr (std::is_same_v<T, pipeline::QueueEvent>) {
                queue_.on_queue(value);
            } else if constexpr (std::is_same_v<T, pipeline::SessionEvent>) {
                if (value.event.view() == "SESSION_CLOSED") {
                    queue_.clear();
                    for (std::size_t i = 0; i < state_count_; ++i)
                        (*states_)[i].quotes = {};
                    // Preserve the old strategy's explicit close-time flatten
                    // attempt. The venue may reject orders after session close.
                    if (!allow_orders || faulted_) return;
                    for (std::size_t i = 0; i < state_count_; ++i) {
                        const auto& state = (*states_)[i];
                        if (state.position == 0) continue;
                        if (state.position > INT32_MAX || state.position < -INT32_MAX) {
                            faulted_ = true;
                            return;
                        }
                        StrategyOrder order{};
                        order.symbol = state.symbol;
                        order.side = state.position > 0 ? pipeline::Side::Sell : pipeline::Side::Buy;
                        order.quantity = static_cast<int>(state.position > 0
                            ? state.position : -state.position);
                        order.market = order.session_close = true;
                        order.session_generation = value.generation;
                        send(order);
                    }
                }
            } else if constexpr (std::is_same_v<T, pipeline::PortfolioEvent>) {
                portfolio_ = value;
            } else if constexpr (std::is_same_v<T, pipeline::ErrorEvent>) {
                ++errors_;
            }
        }, event);
    }

    // Read only from the owner, or after that owner has joined.
    const SymbolState* find(std::string_view symbol) const noexcept {
        const auto id = lookup_.find(symbol);
        return id == Lookup::npos ? nullptr : &(*states_)[id];
    }
    const FixedQueueTracker<>& queue() const noexcept { return queue_; }
    bool faulted() const noexcept { return faulted_; }
    std::uint64_t processed() const noexcept { return processed_; }
    std::uint64_t errors() const noexcept { return errors_; }
    std::size_t symbol_count() const noexcept { return state_count_; }
    void enable_book_check() noexcept { check_book_ = true; }
    std::uint64_t book_checked() const noexcept { return book_checked_; }
    std::uint64_t book_matched() const noexcept { return book_matched_; }
    std::uint64_t book_rejected() const noexcept { return book_rejected_; }

private:
    static constexpr int kClip = 1;
    static constexpr int kMaxPos = 5;
    static constexpr double kEdge = 0.02;
    static constexpr double kSignalFrac = 0.1;
    static constexpr int kMaxQueueAhead = 200;
    static constexpr std::int64_t kMaxJoinQty = 200;
    static constexpr int kAckWaitTicks = 100;
    static constexpr int kRequoteCooldownTicks = 50;

    using Lookup = SymbolRegistry<128, 16>;
    SymbolState* state_for(std::string_view symbol) {
        const auto id = lookup_.find(symbol);
        if (id != Lookup::npos) return &(*states_)[id];
        if (symbol.empty() || symbol.size() > 16 || state_count_ == kMaxSymbols) {
            faulted_ = true;
            return nullptr;
        }
        auto& state = (*states_)[state_count_];
        state.local_book.emplace(::TICK);
        if (!lookup_.insert(symbol, state_count_)) {
            state.local_book.reset();
            faulted_ = true;
            return nullptr;
        }
        state.symbol.assign(symbol);
        ++state_count_;
        return &state;
    }

    template<class Send>
    void process_book(const pipeline::BookEvent& event, bool allow_orders, Send& send) {
        BookSnapshotMsg snapshot{};
        snapshot.symbol = event.symbol.view();
        snapshot.bids = event.bids;
        snapshot.asks = event.asks;
        snapshot.bid_count = event.bid_count;
        snapshot.ask_count = event.ask_count;
        snapshot.best_bid = event.best_bid;
        snapshot.best_ask = event.best_ask;
        snapshot.depth_valid = event.depth_valid;
        if (!event.depth_valid) {
            const auto id = lookup_.find(snapshot.symbol);
            if (id != Lookup::npos && (*states_)[id].local_book)
                (*states_)[id].local_book->update(snapshot);
            if (check_book_) ++book_rejected_;
            return;
        }
        auto* state = state_for(snapshot.symbol);
        if (!state || !state->local_book) {
            if (check_book_) ++book_rejected_;
            return;
        }
        if (!state->local_book->update(snapshot)) {
            if (check_book_) ++book_rejected_;
            return;
        }
        if (check_book_) {
            ++book_checked_;
            if (state->local_book->matches(snapshot)) ++book_matched_;
        }
        if (!event.session_open_at_receive) return;
        const auto top = state->local_book->top();
        if (top.bid_price <= 0.0 || top.ask_price <= top.bid_price) return;
        const double spread = top.ask_price - top.bid_price;
        if (std::isfinite(event.mid) && std::isfinite(event.microprice) &&
            std::isfinite(event.obi) && std::isfinite(spread)) {
            const double raw = 0.5 * (event.microprice - event.mid) + 0.25 * spread * event.obi;
            if (!state->has_signal) {
                state->signal_ema = raw;
                state->has_signal = true;
            } else {
                state->signal_ema += 0.1 * (raw - state->signal_ema);
            }
        }
        if (!allow_orders || faulted_) return;
        const bool tradable = spread >= kEdge && state->has_signal;
        const double margin = kSignalFrac * spread;
        const bool want_buy = tradable && state->signal_ema > margin && state->position < kMaxPos;
        const bool want_sell = tradable && state->signal_ema < -margin && state->position > -kMaxPos;
        manage_quote(state->quotes[0], event, pipeline::Side::Buy, want_buy,
                     top.bid_price, top.bid_quantity, send);
        manage_quote(state->quotes[1], event, pipeline::Side::Sell, want_sell,
                     top.ask_price, top.ask_quantity, send);
    }

    template<class Send>
    void manage_quote(Quote& quote, const pipeline::BookEvent& event, pipeline::Side side,
                      bool want, double touch_price, std::int64_t touch_qty, Send& send) {
        if (quote.cooldown > 0) --quote.cooldown;
        if (quote.waiting) {
            if (++quote.wait_ticks <= kAckWaitTicks) return;
            quote.waiting = false;
        }

        StrategyOrder order{};
        order.symbol = event.symbol;
        order.recv_time = event.recv_time;
        order.session_generation = event.session_generation;

        if (quote.id.size != 0) {
            if (const auto* entry = queue_.find(quote.id.view())) {
                const bool stale = std::abs(entry->price - touch_price) > 0.5 * ::TICK;
                const bool deep = entry->queue_ahead > kMaxQueueAhead;
                if (want && !stale && !deep) return;
                order.cancel_id = quote.id;
                send(order);
                queue_.remove(quote.id.view());
                order.cancel_id.size = 0;
                if (deep) quote.cooldown = kRequoteCooldownTicks;
            }
            quote.id.size = 0;
        }

        if (!want || quote.cooldown > 0 || touch_qty > kMaxJoinQty) return;
        order.side = side;
        order.quantity = kClip;
        order.price = touch_price;
        send(order);
        quote.waiting = true;
        quote.wait_ticks = 0;
    }

    std::unique_ptr<std::array<SymbolState, kMaxSymbols>> states_ =
        std::make_unique<std::array<SymbolState, kMaxSymbols>>();
    Lookup lookup_;
    FixedQueueTracker<> queue_;
    pipeline::PortfolioEvent portfolio_{};
    std::size_t state_count_ = 0;
    std::uint64_t processed_ = 0;
    std::uint64_t errors_ = 0;
    bool faulted_ = false;
    bool check_book_ = false;
    std::uint64_t book_checked_ = 0;
    std::uint64_t book_matched_ = 0;
    std::uint64_t book_rejected_ = 0;
};

} // namespace arena
