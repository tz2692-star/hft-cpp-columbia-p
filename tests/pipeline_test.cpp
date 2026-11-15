#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "spsc_ring.hpp" // Course interface, selected by -Iinclude.
#include "../hft/cpp_client/include/inbound_channel.hpp"

using namespace arena;
using namespace arena::pipeline;

static void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::abort();
    }
}

static void ownership_and_validation() {
    BookEvent owned{};
    const auto stamp = std::chrono::steady_clock::now();
    {
        std::string symbol = "ABC";
        BookSnapshotMsg snapshot{};
        snapshot.symbol = symbol;
        snapshot.depth_valid = true;
        snapshot.bid_count = snapshot.ask_count = 1;
        snapshot.bids[0] = PriceLevel{100.0, 7};
        snapshot.asks[0] = PriceLevel{101.0, 9};
        snapshot.mid = 100.5;
        snapshot.microprice = 100.4;
        snapshot.obi = -0.125;
        require(copy_book(snapshot, stamp, true, owned), "copy book");
        symbol[0] = 'Z';
        snapshot.bids[0].quantity = 999;
    }
    require(owned.symbol.view() == "ABC", "owned symbol lifetime");
    require(owned.bids[0].quantity == 7 && owned.asks[0].quantity == 9,
            "owned depth lifetime");
    require(owned.recv_time == stamp && owned.session_open_at_receive &&
            owned.mid == 100.5 && owned.microprice == 100.4 &&
            owned.obi == -0.125, "metadata survives copy");

    BookSnapshotMsg snapshot{};
    snapshot.symbol = "ABC";
    BookEvent result{};
    require(copy_book(snapshot, stamp, false, result) && !result.depth_valid,
            "invalid snapshot must be delivered");
    snapshot.depth_valid = true;
    require(copy_book(snapshot, stamp, false, result) && result.depth_valid &&
            result.bid_count == 0 && result.ask_count == 0, "valid empty book");
    snapshot.bid_count = kMaxSnapshotLevels + 1;
    require(copy_book(snapshot, stamp, false, result) && !result.depth_valid &&
            result.bid_count == 0 && result.ask_count == 0, "oversized depth");
    snapshot.symbol = "ABCDEFGHIJKLMNOPQ";
    require(!copy_book(snapshot, stamp, false, owned) &&
            owned.symbol.view() == "ABC" && owned.bids[0].quantity == 7,
            "reject oversized symbol without modifying output");
    snapshot.symbol = {};
    require(!copy_book(snapshot, stamp, false, owned), "reject empty symbol");

    Symbol text{};
    require(text.assign("ABCDEFGHIJKLMNOP"), "exact text capacity");
    require(!text.assign("ABCDEFGHIJKLMNOPQ") && text.size == 16,
            "no truncation");
    require(text.assign(text.view().substr(1)) &&
            text.view() == "BCDEFGHIJKLMNOP", "overlapping text assignment");
    require(text.assign({}) && text.view().empty(), "empty text");

    for (std::size_t capacity : {std::size_t{0}, std::size_t{3}}) {
        bool rejected = false;
        try { SPSCRing invalid(capacity); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "course ring rejects invalid capacity");
    }
    SPSCRing single(1);
    std::uint64_t value = 0;
    require(single.push(7) && single.full() && !single.push(8) &&
            single.pop(value) && value == 7 && single.empty(), "capacity one");
}

static void fifo_and_overflow() {
    auto ring = std::make_unique<TypedSpscRing<Event, 8>>();
    BookEvent book{};
    require(book.symbol.assign("ABC"), "book symbol");
    FillEvent fill{};
    fill.quantity = 42;
    AckEvent ack{};
    require(ack.order_id.assign("order-1"), "ack ID");
    QueueEvent queue{};
    queue.queue_ahead = 3;
    SessionEvent session{};
    require(session.event.assign("SESSION_CLOSED"), "session name");
    ErrorEvent error{};
    require(error.code.assign("REJECTED"), "error code");
    PortfolioEvent portfolio{};
    portfolio.cash = 1234.0;
    const Event events[] = {book, fill, ack, queue, session, error, portfolio, fill};

    for (int cycle = 0; cycle < 1000; ++cycle) {
        for (const auto& event : events)
            require(ring->push(event), "push to full capacity");
        require(!ring->push(Event{book}), "reject full ring");
        Event out{};
        for (const auto& expected : events) {
            require(ring->pop(out) && out.index() == expected.index(),
                    "FIFO variant tag");
            std::visit([&](const auto& payload) {
                using T = std::decay_t<decltype(payload)>;
                const auto& actual = std::get<T>(out);
                if constexpr (std::is_same_v<T, BookEvent>)
                    require(actual.symbol.view() == "ABC", "book payload");
                else if constexpr (std::is_same_v<T, FillEvent>)
                    require(actual.quantity == 42, "fill payload");
                else if constexpr (std::is_same_v<T, AckEvent>)
                    require(actual.order_id.view() == "order-1", "ack payload");
                else if constexpr (std::is_same_v<T, QueueEvent>)
                    require(actual.queue_ahead == 3, "queue payload");
                else if constexpr (std::is_same_v<T, SessionEvent>)
                    require(actual.event.view() == "SESSION_CLOSED", "session payload");
                else if constexpr (std::is_same_v<T, ErrorEvent>)
                    require(actual.code.view() == "REJECTED", "error payload");
                else
                    require(actual.cash == 1234.0, "portfolio payload");
            }, expected);
        }
        require(ring->empty() && !ring->pop(out) &&
                std::get<FillEvent>(out).quantity == 42, "empty leaves out unchanged");
    }

    auto channel = std::make_unique<InboundChannel<2>>();
    require(!channel->faulted(), "initial channel state");
    require(channel->publish(Event{book}) && channel->publish(Event{fill}),
            "publish to capacity");
    require(!channel->publish(Event{ack}) && channel->faulted(), "overflow faults");
    Event out{};
    require(channel->pop(out) && std::holds_alternative<BookEvent>(out),
            "overflow does not overwrite first event");
    require(!channel->publish(Event{ack}), "fault stays latched after draining");
    require(channel->pop(out) && std::get<FillEvent>(out).quantity == 42,
            "overflow does not overwrite second event");
    auto bad_input = std::make_unique<InboundChannel<2>>();
    bad_input->mark_fault();
    require(bad_input->faulted() && !bad_input->publish(Event{book}),
            "conversion failure faults channel");
}

static void concurrent_payloads() {
    auto ring = std::make_unique<TypedSpscRing<Event, 64>>();
    constexpr int count = 100000;
    std::atomic<bool> failed{false};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto stop_waiting = [&] {
        if (failed.load(std::memory_order_relaxed)) return true;
        if (std::chrono::steady_clock::now() < deadline) return false;
        failed.store(true, std::memory_order_relaxed);
        return true;
    };
    std::thread producer([&] {
        for (int i = 0; i < count; ++i) {
            BookEvent book{};
            book.symbol.assign("ABC");
            book.depth_valid = true;
            book.bid_count = book.ask_count = kMaxSnapshotLevels;
            for (std::size_t j = 0; j < kMaxSnapshotLevels; ++j) {
                book.bids[j] = PriceLevel{100.0 + j, i + 1};
                book.asks[j] = PriceLevel{200.0 + j, count - i};
            }
            book.mid = i;
            FillEvent fill{};
            fill.quantity = i;
            // Switch alternatives to exercise variant lifetime changes too.
            const Event event = (i % 2 == 0) ? Event{book} : Event{fill};
            while (!ring->push(event)) {
                if (stop_waiting()) return;
                std::this_thread::yield();
            }
        }
    });
    std::thread consumer([&] {
        Event event{};
        for (int i = 0; i < count; ++i) {
            while (!ring->pop(event)) {
                if (stop_waiting()) return;
                std::this_thread::yield();
            }
            bool valid = false;
            if (i % 2 == 0) {
                const auto* b = std::get_if<BookEvent>(&event);
                valid = b && b->symbol.view() == "ABC" && b->mid == i &&
                    b->depth_valid && b->bid_count == kMaxSnapshotLevels &&
                    b->ask_count == kMaxSnapshotLevels;
                for (std::size_t j = 0; valid && j < kMaxSnapshotLevels; ++j)
                    valid = b->bids[j].price == 100.0 + j &&
                        b->bids[j].quantity == i + 1 &&
                        b->asks[j].price == 200.0 + j &&
                        b->asks[j].quantity == count - i;
            } else {
                const auto* f = std::get_if<FillEvent>(&event);
                valid = f && f->quantity == i;
            }
            if (!valid) { failed.store(true); return; }
        }
    });
    producer.join();
    consumer.join();
    require(!failed.load() && ring->empty(), "concurrent payload integrity");
}

int main() {
    ownership_and_validation();
    fifo_and_overflow();
    concurrent_payloads();
    std::cout << "pipeline tests passed\n";
}
