#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <new>
#include <thread>
#include <vector>

#include "../hft/cpp_client/include/spread_strategy.hpp"
#include "../hft/cpp_client/include/inbound_channel.hpp"

// Detect accidental C++ heap allocation in the strategy processor, including
// first symbol admission. Transport sends are replaced by an inline collector.
thread_local bool forbid_allocation = false;
void* operator new(std::size_t n) {
    if (forbid_allocation) std::abort();
    if (auto* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace arena;
using namespace arena::pipeline;
static void check(bool ok) { if (!ok) std::abort(); }

struct Orders {
    std::array<StrategyOrder, 128> values{};
    std::size_t size = 0;
    void operator()(const StrategyOrder& order) {
        check(size < values.size());
        values[size++] = order;
    }
};

static BookEvent book(double mid) {
    BookEvent b{};
    b.symbol.assign("ABC");
    b.depth_valid = b.session_open_at_receive = true;
    b.bid_count = b.ask_count = 1;
    b.bids[0] = {100.0, 10};
    b.asks[0] = {100.1, 10};
    b.mid = mid;
    b.microprice = mid + 0.04;
    b.obi = 0.8;
    b.recv_time = TimePoint{std::chrono::seconds(123)};
    b.session_generation = 2;
    return b;
}

int main() {
    std::vector<Event> tape;
    auto invalid_new = book(100.0);
    invalid_new.symbol.assign("UNSEEN");
    invalid_new.depth_valid = false;
    tape.emplace_back(invalid_new);
    FillEvent fill{};
    fill.symbol.assign("ABC");
    fill.side = Side::Buy;
    fill.quantity = 5;
    tape.emplace_back(fill);
    tape.emplace_back(book(100.05)); // Filled inventory cap suppresses buy.
    fill.side = Side::Sell;
    tape.emplace_back(fill);
    tape.emplace_back(book(100.05)); // Join the bid after inventory falls.
    tape.emplace_back(book(100.05)); // Wait for ACK.
    AckEvent ack{};
    ack.symbol.assign("ABC");
    ack.order_id.assign("order-1");
    ack.queue_ahead = 4;
    ack.level_qty = 5;
    ack.is_buy = true;
    ack.price = 100.0;
    tape.emplace_back(ack);
    tape.emplace_back(book(100.05)); // Hold the acknowledged quote.
    QueueEvent queue{};
    queue.symbol = ack.symbol;
    queue.order_id = ack.order_id;
    queue.queue_ahead = 201;
    queue.level_qty = 300;
    tape.emplace_back(queue);
    auto invalid = book(200.0);
    invalid.depth_valid = false;
    tape.emplace_back(invalid); // Must not trade.
    tape.emplace_back(book(100.05)); // Cancel the deep quote.
    tape.emplace_back(book(100.05)); // Cooldown prevents immediate re-entry.
    fill.side = Side::Buy;
    fill.quantity = 1;
    tape.emplace_back(fill);
    SessionEvent closed{};
    closed.event.assign("SESSION_CLOSED");
    closed.generation = 3;
    tape.emplace_back(closed); // Close flatten attempt, queue cleared.

    SpreadStrategy sync;
    Orders expected;
    forbid_allocation = true;
    for (const auto& event : tape) sync.process(event, true, expected);
    forbid_allocation = false;
    check(!sync.faulted() && sync.symbol_count() == 1 && expected.size == 3);
    check(expected.values[0].side == Side::Buy && expected.values[0].price == 100.0);
    check(expected.values[0].recv_time == book(0).recv_time);
    check(expected.values[1].cancel_id.view() == "order-1");
    check(expected.values[2].market && expected.values[2].quantity == 1);
    check(sync.queue().size() == 0 && sync.find("ABC")->position == 1);

    // Exercise the same engine with actual producer/consumer ownership.
    auto ring = std::make_unique<TypedSpscRing<Event, 4>>();
    SpreadStrategy threaded;
    Orders actual;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    auto wait = [&] {
        check(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    };
    std::thread producer([&] {
        for (const auto& event : tape) while (!ring->push(event)) wait();
    });
    std::thread consumer([&] {
        forbid_allocation = true;
        Event event{};
        for (std::size_t i = 0; i < tape.size(); ++i) {
            while (!ring->pop(event)) wait();
            threaded.process(event, true, actual);
        }
        forbid_allocation = false;
    });
    producer.join();
    consumer.join();
    check(actual.size == expected.size && threaded.processed() == tape.size());
    for (std::size_t i = 0; i < actual.size; ++i) {
        check(actual.values[i].symbol.view() == expected.values[i].symbol.view());
        check(actual.values[i].side == expected.values[i].side);
        check(actual.values[i].price == expected.values[i].price);
        check(actual.values[i].cancel_id.view() == expected.values[i].cancel_id.view());
        check(actual.values[i].session_generation == expected.values[i].session_generation);
    }
    check(threaded.find("ABC")->position == sync.find("ABC")->position);
    check(threaded.find("ABC")->signal_ema == sync.find("ABC")->signal_ema);
    check(threaded.queue().size() == 0);

    // Gate orders during shutdown/fault while continuing to apply state.
    const auto old_count = actual.size;
    threaded.process(Event{book(110.0)}, false, actual);
    check(actual.size == old_count && threaded.find("ABC")->local_book->valid());
    threaded.process(Event{invalid}, true, actual);
    check(!threaded.find("ABC")->local_book->valid() && actual.size == old_count);

    // Fixed tracker overflow, deletion, and slot reuse.
    FixedQueueTracker<1> tracker;
    check(tracker.on_ack(ack));
    ack.order_id.assign("order-2");
    check(!tracker.on_ack(ack));
    queue.level_qty = 0;
    tracker.on_queue(queue);
    check(tracker.size() == 0 && tracker.on_ack(ack));
    tracker.clear();
    check(tracker.size() == 0);
    std::cout << "strategy tests passed (including allocation guard)\n";
}
