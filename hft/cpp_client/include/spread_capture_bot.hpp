#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <cstdint>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif
#include "hft_bot.hpp"
#include "inbound_channel.hpp"
#include "spread_strategy.hpp"
#include "typed_spsc_ring.hpp"

namespace arena {

// Receive callbacks only copy/publish. All strategy state belongs to the
// consumer. Live and replay runs are one-shot and must not run concurrently.
class SpreadCaptureBot : public HFTBot<SpreadCaptureBot> {
    using Base = HFTBot<SpreadCaptureBot>;
public:
    friend class ArenaClient<SpreadCaptureBot>;
    friend class HFTBot<SpreadCaptureBot>;

    explicit SpreadCaptureBot(ClientConfig cfg) : Base(std::move(cfg)) {
        receive_cpu_ = cpu_from_env("HFT_RX_CPU");
        strategy_cpu_ = cpu_from_env("HFT_STRATEGY_CPU");
        sender_cpu_ = cpu_from_env("HFT_SENDER_CPU");
        const bool any_cpu_set =
            receive_cpu_ >= 0 || strategy_cpu_ >= 0 || sender_cpu_ >= 0;
        const bool all_cpus_set =
            receive_cpu_ >= 0 && strategy_cpu_ >= 0 && sender_cpu_ >= 0;
        if (any_cpu_set && !all_cpus_set)
            throw std::invalid_argument(
                "set HFT_RX_CPU, HFT_STRATEGY_CPU, and HFT_SENDER_CPU together, or leave all unset");
        if (all_cpus_set &&
            (receive_cpu_ == strategy_cpu_ || receive_cpu_ == sender_cpu_ ||
             strategy_cpu_ == sender_cpu_))
            throw std::invalid_argument("receive, strategy, and sender threads require distinct CPUs");
        send_symbol_.reserve(16); // Sender thread's transport adapter scratch space.
        send_order_id_.reserve(detail::kMaxOrderIdBytes);
    }

    // Configure before run() or run_replay(); the strategy owns these counters.
    void enable_book_check() noexcept {
        check_book_ = true;
        strategy_.enable_book_check();
    }

    // Call after run() or run_replay() returns, when strategy processing is done.
    void report_book_check() const {
        if (!check_book_) return;
        const auto checked = strategy_.book_checked();
        const auto matched = strategy_.book_matched();
        std::cerr << "[book-check] snapshots=" << checked
                  << " matched=" << matched
                  << " mismatched=" << (checked - matched)
                  << " rejected=" << strategy_.book_rejected() << "\n";
    }
    void run() {
        begin(false);
        std::thread consumer([this] { consume(); });
        std::thread sender;
        try {
            sender = std::thread([this] { send_orders(); });
            // This returns only after IXWebSocket has stopped/joined its RX
            // worker. Keep both consumers alive until the receive producer ends.
            if (!stop_requested_.load(std::memory_order_acquire)) Base::run();
        } catch (...) {
            stop();
            producer_done_.store(true, std::memory_order_release);
            consumer.join();
            strategy_done_.store(true, std::memory_order_release);
            if (sender.joinable()) sender.join();
            throw;
        }
        stop_requested_.store(true, std::memory_order_release);
        producer_done_.store(true, std::memory_order_release);
        consumer.join();
        strategy_done_.store(true, std::memory_order_release);
        sender.join();
        report();
        if (worker_error_) std::rethrow_exception(worker_error_);
        if (sender_error_) std::rethrow_exception(sender_error_);
        if (channel_->faulted() || strategy_.faulted())
            throw std::runtime_error("pipeline fault: reconcile state before restarting");
        if (order_queue_overflow_.load(std::memory_order_acquire))
            throw std::runtime_error("outbound order queue overflow");
        const int affinity_error = receive_affinity_error_.load(std::memory_order_acquire);
        if (affinity_error != 0)
            throw std::system_error(affinity_error, std::generic_category(),
                                    "pin receive thread");
    }

    void run_replay(std::istream& in, std::ostream& out) {
        begin(true);
        // No worker: dispatch, decisions, and order capture finish before the
        // base replay loop emits its one response per input line.
        Base::run_replay(in, out);
        if (channel_->faulted() || strategy_.faulted())
            throw std::runtime_error("replay pipeline fault");
    }

    // Only atomics: also used by the existing signal-driven entry point.
    void stop() noexcept {
        stop_requested_.store(true, std::memory_order_release);
        Base::stop();
    }

    // Inspect only before run or after it returns (the consumer has joined).
    const SpreadStrategy& strategy() const noexcept { return strategy_; }

private:
    void begin(bool replay) {
        if (started_) throw std::logic_error("SpreadCaptureBot instances are one-shot");
        started_ = true;
        replay_ = replay;
    }

    void input_fault() noexcept {
        channel_->mark_fault(); // Producer is its only writer.
        stop();
    }

    void publish(const pipeline::Event& event) {
        if (replay_) {
            process(event);
            if (channel_->faulted() || strategy_.faulted())
                throw std::runtime_error("replay pipeline fault");
        } else if (!channel_->publish(event)) {
            stop();
        }
    }

    bool on_depth(const BookSnapshotMsg& snapshot) {
        if (!ensure_receive_thread_pinned()) return false;
        if (!pipeline::copy_book(snapshot, clock::now(), session_open(), pending_book_)) {
            input_fault();
            return false;
        }
        pending_book_.session_generation = generation_.load(std::memory_order_relaxed);
        if (!pending_book_.depth_valid) {
            publish(pending_book_); // Invalidation must reach the consumer too.
            return false;
        }
        return true;
    }

    // CRTP dispatch selects this callback instead of HFTBot's synchronous
    // bridge. The original pre-decode timestamp travels with the event.
    void on_book_snapshot(const BookView&, time_point received) {
        if (!ensure_receive_thread_pinned()) return;
        pending_book_.recv_time = received;
        publish(pending_book_);
    }

    void on_fill(const std::string& side, const std::string& symbol,
                 int quantity, double price, bool maker) {
        if (!ensure_receive_thread_pinned()) return;
        if ((side != "buy" && side != "sell") || quantity <= 0) return;
        pipeline::FillEvent event{};
        if (symbol.empty() || !event.symbol.assign(symbol)) { input_fault(); return; }
        event.side = side == "buy" ? pipeline::Side::Buy : pipeline::Side::Sell;
        event.quantity = quantity;
        event.price = price;
        event.maker = maker;
        publish(event);
    }

    void on_ack(const std::string& symbol, const std::string& id,
                bool is_buy, double price, int ahead, int quantity) {
        if (!ensure_receive_thread_pinned()) return;
        pipeline::AckEvent event{};
        if (symbol.empty() || id.empty() || !event.symbol.assign(symbol) ||
            !event.order_id.assign(id)) { input_fault(); return; }
        event.queue_ahead = ahead;
        event.level_qty = quantity;
        event.is_buy = is_buy;
        event.price = price;
        publish(event);
    }

    void on_queue(const std::string& symbol, const std::string& id,
                  int ahead, int quantity) {
        if (!ensure_receive_thread_pinned()) return;
        pipeline::QueueEvent event{};
        if (symbol.empty() || id.empty() || !event.symbol.assign(symbol) ||
            !event.order_id.assign(id)) { input_fault(); return; }
        event.queue_ahead = ahead;
        event.level_qty = quantity;
        publish(event);
    }

    void on_session(const std::string& name, const std::string& message) {
        if (!ensure_receive_thread_pinned()) return;
        pipeline::SessionEvent event{};
        if (name == "SESSION_OPEN" || name == "SESSION_PREOPEN" || name == "SESSION_CLOSED")
            generation_.fetch_add(1, std::memory_order_release);
        event.generation = generation_.load(std::memory_order_relaxed);
        if (!event.event.assign(name) || !event.message.assign(message)) {
            input_fault();
            return;
        }
        publish(event);
    }

    void on_error(const std::string& code, const std::string& message) {
        if (!ensure_receive_thread_pinned()) return;
        pipeline::ErrorEvent event{};
        if (!event.code.assign(code) || !event.message.assign(message)) {
            input_fault();
            return;
        }
        publish(event);
    }

    void on_portfolio(double cash, double worth, double realized, double unrealized) {
        if (!ensure_receive_thread_pinned()) return;
        publish(pipeline::PortfolioEvent{cash, worth, realized, unrealized});
    }

    bool can_submit() const noexcept {
        return !stop_requested_.load(std::memory_order_acquire) && !channel_->faulted();
    }

    void process(const pipeline::Event& event) {
        strategy_.process(event, can_submit(), [this](const StrategyOrder& order) {
            if (replay_) submit(order);
            else enqueue_order(order);
        });
        if (strategy_.faulted()) stop();
    }

    void enqueue_order(const StrategyOrder& order) noexcept {
        if (!can_submit()) return;
        if (!outbound_orders_->push(order)) {
            order_queue_overflow_.store(true, std::memory_order_release);
            stop();
        }
    }

    void submit(const StrategyOrder& order) {
        if (!can_submit() || order.session_generation != generation_.load(std::memory_order_acquire))
            return;
        if (!order.session_close && !session_open()) return;
        send_symbol_.assign(order.symbol.bytes.data(), order.symbol.size);
        if (order.cancel_id.size != 0) {
            send_order_id_.assign(order.cancel_id.bytes.data(), order.cancel_id.size);
            cancel(send_order_id_, send_symbol_);
            return;
        }
        const auto& side = order.side == pipeline::Side::Buy ? buy_ : sell_;
        // Transport calls run only on the sender thread, never the strategy path.
        if (order.market) place_market(send_symbol_, side, order.quantity);
        else place_limit(send_symbol_, side, order.quantity, order.price);

        if (!order.market) {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                clock::now() - order.recv_time).count();
            if (replay_) {
                record_latency(send_symbol_, micros);
            } else if (latency_count_ < latency_.size()) {
                latency_[latency_count_++] = {order.symbol, micros};
            } else {
                ++dropped_latency_;
            }
        }
    }

    void send_orders() noexcept {
        try {
            if (sender_cpu_ >= 0) {
                const int error = pin_current_thread(sender_cpu_);
                if (error != 0)
                    throw std::system_error(error, std::generic_category(),
                                            "pin order-sender thread");
                std::cerr << "[affinity] order-sender thread pinned to CPU "
                          << sender_cpu_ << "\n";
            }
            (void)detail::message_pool_for_thread();
            StrategyOrder order{};
            for (;;) {
                if (outbound_orders_->pop(order)) {
                    submit(order);
                } else if (strategy_done_.load(std::memory_order_acquire)) {
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
        } catch (...) {
            sender_error_ = std::current_exception();
            stop();
        }
    }

    void consume() noexcept {
        try {
            if (strategy_cpu_ >= 0) {
                const int error = pin_current_thread(strategy_cpu_);
                if (error != 0)
                    throw std::system_error(error, std::generic_category(),
                                            "pin strategy thread");
                std::cerr << "[affinity] strategy thread pinned to CPU "
                          << strategy_cpu_ << "\n";
            }
            pipeline::Event event{};
            for (;;) {
                // Repeated stop handles a signal/fault arriving just before
                // Base::run() stores its own running flag true.
                if (stop_requested_.load(std::memory_order_acquire)) Base::stop();
                const bool done = producer_done_.load(std::memory_order_acquire);
                if (channel_->pop(event)) process(event);
                else if (done) break;
                else std::this_thread::yield();
            }
        } catch (...) {
            worker_error_ = std::current_exception();
            stop();
            // The producer must finish before run() joins/destroys anything.
            while (!producer_done_.load(std::memory_order_acquire)) {
                Base::stop();
                std::this_thread::yield();
            }
        }
    }

    void report() const {
        // Both workers are joined: bounded telemetry is formatted off the hot path.
        for (std::size_t i = 0; i < latency_count_; ++i)
            std::cerr << "LAT " << latency_[i].symbol.view() << ' '
                      << latency_[i].micros << '\n';
        std::cerr << "[pipeline] processed=" << strategy_.processed()
                  << " errors=" << strategy_.errors()
                  << " dropped_latency=" << dropped_latency_ << '\n';
    }

    static int cpu_from_env(const char* name) {
        const char* value = std::getenv(name);
        if (value == nullptr || *value == '\0') return -1;
        std::size_t parsed = 0;
        int cpu = -1;
        try {
            cpu = std::stoi(value, &parsed);
        } catch (const std::exception&) {
            throw std::invalid_argument(std::string(name) + " must be a nonnegative CPU ID");
        }
        if (parsed != std::string(value).size() || cpu < 0)
            throw std::invalid_argument(std::string(name) + " must be a nonnegative CPU ID");
        return cpu;
    }

    static int pin_current_thread(int cpu) noexcept {
#if defined(__linux__)
        if (cpu < 0 || cpu >= CPU_SETSIZE) return EINVAL;
        cpu_set_t cpus;
        CPU_ZERO(&cpus);
        CPU_SET(cpu, &cpus);
        return pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
#else
        (void)cpu;
        return ENOTSUP;
#endif
    }

    bool ensure_receive_thread_pinned() noexcept {
        if (replay_ || receive_cpu_ < 0) return true;
        int state = receive_affinity_state_.load(std::memory_order_acquire);
        if (state == 2) return true;
        if (state < 0) return false;
        int expected = 0;
        if (receive_affinity_state_.compare_exchange_strong(
                expected, 1, std::memory_order_acq_rel)) {
            const int error = pin_current_thread(receive_cpu_);
            receive_affinity_error_.store(error, std::memory_order_release);
            if (error != 0) {
                receive_affinity_state_.store(-1, std::memory_order_release);
                std::cerr << "[affinity] failed to pin receive thread to CPU "
                          << receive_cpu_ << ": " << std::strerror(error) << "\n";
                stop();
                return false;
            }
            receive_affinity_state_.store(2, std::memory_order_release);
            std::cerr << "[affinity] receive thread pinned to CPU "
                      << receive_cpu_ << "\n";
            return true;
        }
        while ((state = receive_affinity_state_.load(std::memory_order_acquire)) == 1)
            std::this_thread::yield();
        return state == 2;
    }

    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::unique_ptr<pipeline::InboundChannel<1024>> channel_ =
        std::make_unique<pipeline::InboundChannel<1024>>();
    std::unique_ptr<TypedSpscRing<StrategyOrder, 1024>> outbound_orders_ =
        std::make_unique<TypedSpscRing<StrategyOrder, 1024>>();
    SpreadStrategy strategy_; // Consumer-owned, including the fixed queue table.
    pipeline::BookEvent pending_book_{}; // Producer-owned staging only.
    std::atomic<std::uint64_t> generation_{0};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> producer_done_{false};
    std::atomic<bool> strategy_done_{false};
    std::atomic<bool> order_queue_overflow_{false};
    std::atomic<int> receive_affinity_state_{0};
    std::atomic<int> receive_affinity_error_{0};
    int receive_cpu_ = -1;
    int strategy_cpu_ = -1;
    int sender_cpu_ = -1;
    bool started_ = false;
    bool replay_ = false; // Set before worker creation, never changed afterward.
    std::exception_ptr worker_error_; // Written by worker, read after join.
    std::exception_ptr sender_error_; // Written by sender, read after join.
    std::string send_symbol_;
    std::string send_order_id_;
    const std::string buy_{"buy"};
    const std::string sell_{"sell"};
    struct LatencySample { pipeline::Symbol symbol; long long micros = 0; };
    std::array<LatencySample, 4096> latency_{};
    std::size_t latency_count_ = 0;
    std::uint64_t dropped_latency_ = 0;
    bool check_book_ = false;
};

} // namespace arena
