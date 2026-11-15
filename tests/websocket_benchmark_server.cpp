#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>
#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>

namespace {
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using Json = nlohmann::json;
constexpr int kSymbols = 32;
constexpr int kFirstBid = 10000;
constexpr int kSpread = 9;

std::string book(std::size_t index) {
    const auto bid = kFirstBid + 2 * (index / kSymbols);
    const auto ask = bid + kSpread;
    Json bids = Json::array(), asks = Json::array();
    for (int level = 0; level < 5; ++level) {
        bids.push_back({(bid - level) / 100.0, 20 - level});
        asks.push_back({(ask + level) / 100.0, 10 + level});
    }
    return Json{{"type", "book_snapshot"}, {"symbol", std::to_string(index % kSymbols)},
                {"mid_price", (bid + ask) / 200.0}, {"microprice", (ask - 1) / 100.0},
                {"obi", 0.5}, {"bids", bids}, {"asks", asks}}.dump();
}

// Even offsets identify bids and odd offsets identify asks. Each touch price
// appears exactly once per symbol and side, so neither bot needs a wire change.
std::size_t book_index(const Json& order, std::size_t unmatched) {
    if (!order.contains("price") || !order["price"].is_number()) return unmatched;
    const double price = order["price"].get<double>();
    const auto symbol = order.value("symbol", std::string{});
    if (symbol.empty() || symbol.size() > 2 ||
        symbol.find_first_not_of("0123456789") != std::string::npos ||
        !std::isfinite(price) || price < 100.0 || price > 1e6) return unmatched;
    const int symbol_id = std::stoi(symbol);
    if (symbol_id >= kSymbols || symbol != std::to_string(symbol_id)) return unmatched;
    const double ticks = price * 100.0;
    const auto rounded = std::lround(ticks);
    if (std::abs(ticks - rounded) > 1e-5) return unmatched;
    auto offset = rounded - kFirstBid;
    if (offset % 2) offset -= kSpread;
    if (offset < 0) return unmatched;
    return static_cast<std::size_t>(offset / 2) * kSymbols + symbol_id;
}

struct Window {
    std::size_t accepted_places = 0;
    std::size_t accepted_cancels = 0;
    std::size_t rejected = 0;
    std::size_t saturated_intervals = 0;
};

int run(int rate, int warmup_seconds, int seconds, int quota, int interval_ms) {
    const auto warmup = static_cast<std::size_t>(rate) * warmup_seconds;
    const auto count = warmup + static_cast<std::size_t>(rate) * seconds;
    const auto duration = std::chrono::seconds(warmup_seconds + seconds);
    const auto quota_window = std::chrono::milliseconds(interval_ms);

    std::vector<std::string> frames;
    frames.reserve(count);
    for (std::size_t i = 0; i < count; ++i) frames.push_back(book(i));
    std::vector<Clock::time_point> sent_at(count);
    std::vector<double> latencies;
    latencies.reserve(count - warmup);
    std::vector<bool> sampled(count);
    std::atomic<std::size_t> published{0};

    std::mutex mutex;
    std::condition_variable handshake;
    std::shared_ptr<ix::WebSocket> client;
    Clock::time_point start;
    Window measured;
    std::size_t unmatched = 0, duplicates = 0, drain_messages = 0;
    std::size_t feedback_sent = 0, feedback_failures = 0, malformed = 0;
    std::int64_t previous_interval = -1;
    int used = 0;
    std::uint64_t next_order = 0;

    std::unique_ptr<ix::WebSocketServer> server;
    for (int port = 19120; port < 19140; ++port) {
        auto candidate = std::make_unique<ix::WebSocketServer>(port, "127.0.0.1", 5, 1);
        if (candidate->listen().first) {
            server = std::move(candidate);
            break;
        }
    }
    if (!server) throw std::runtime_error("cannot bind a loopback WebSocket port (19120–19139)");
    server->disablePerMessageDeflate();
    server->setOnConnectionCallback([&](std::weak_ptr<ix::WebSocket> weak, auto) {
        if (auto socket = weak.lock()) {
            socket->setOnMessageCallback([&, weak](const ix::WebSocketMessagePtr& message) {
                if (message->type != ix::WebSocketMessageType::Message) return;
                const auto arrival = Clock::now();
                auto socket = weak.lock();
                if (!socket) return;
                std::lock_guard lock(mutex);
                try {
                    const auto order = Json::parse(message->str);
                    const auto type = order.value("type", std::string{});
                    if (type == "handshake") {
                        if (client) return;
                        if (!socket->sendText(R"({"type":"session_event","event":"SESSION_OPEN","message":"benchmark"})").success)
                            throw std::runtime_error("SESSION_OPEN send failed");
                        start = Clock::now();
                        client = socket;
                        handshake.notify_one();
                        return;
                    }
                    const bool place = type == "place_order";
                    if (socket != client || (!place && type != "cancel_order")) return;
                    const auto elapsed = arrival - start;
                    const bool in_window = elapsed >= std::chrono::seconds(warmup_seconds) &&
                                           elapsed < duration;
                    if (elapsed >= duration) ++drain_messages;

                    if (place && order.value("order_type", std::string{}) == "limit") {
                        const auto index = book_index(order, count);
                        if (index >= published.load(std::memory_order_acquire)) {
                            if (in_window) ++unmatched;
                        } else if (index >= warmup) {
                            if (sampled[index]) ++duplicates;
                            else {
                                sampled[index] = true;
                                latencies.push_back(
                                    std::chrono::duration<double, std::micro>(arrival - sent_at[index]).count());
                            }
                        }
                    }

                    const auto interval = elapsed / quota_window;
                    if (interval != previous_interval) {
                        previous_interval = interval;
                        used = 0;
                    }
                    if (used == quota) {
                        if (in_window) ++measured.rejected;
                        if (socket->sendText(R"({"type":"error","code":"ORDER_QUOTA","message":"benchmark quota exceeded"})").success)
                            ++feedback_sent;
                        else ++feedback_failures;
                        return;
                    }
                    ++used;
                    if (in_window) {
                        if (used == quota) ++measured.saturated_intervals;
                        if (place) ++measured.accepted_places;
                        else ++measured.accepted_cancels;
                    }
                    if (place && socket->sendText(Json{
                            {"type", "order_ack"}, {"order_id", "b" + std::to_string(++next_order)},
                            {"symbol", order.at("symbol")}, {"side", order.at("side")},
                            {"price", order.at("price")}, {"queue_ahead", 4},
                            {"level_qty", 20}}.dump()).success) ++feedback_sent;
                    else if (place) ++feedback_failures;
                } catch (const std::exception&) {
                    ++malformed;
                }
            });
        }
    });
    server->start();
    std::cout << "READY " << server->getPort() << std::endl;
    {
        std::unique_lock lock(mutex);
        if (!handshake.wait_for(lock, 10s, [&] { return client != nullptr; }))
            throw std::runtime_error("bot did not complete the WebSocket handshake");
    }

    auto measured_start = start;
    double max_late_us = 0.0;
    std::size_t behind_tick = 0;
    const auto tick_period = std::chrono::duration<double>(1.0 / rate);
    const auto planned_start = start + std::chrono::seconds(warmup_seconds);
    for (std::size_t i = 0; i < count; ++i) {
        const auto deadline = start + std::chrono::duration_cast<Clock::duration>(tick_period * i);
        std::this_thread::sleep_until(deadline);
        const auto now = Clock::now();
        if (i == warmup) measured_start = now;
        if (i >= warmup) {
            const auto late = now - deadline;
            max_late_us = std::max(max_late_us, std::chrono::duration<double, std::micro>(late).count());
            if (late >= tick_period) ++behind_tick;
        }
        sent_at[i] = now;
        published.store(i + 1, std::memory_order_release);
        if (!client->sendText(frames[i]).success)
            throw std::runtime_error("WebSocket book send failed");
    }
    std::this_thread::sleep_until(start + duration);
    const auto feed_done = Clock::now();
    std::this_thread::sleep_for(500ms);
    if (client->getReadyState() != ix::ReadyState::Open)
        throw std::runtime_error("bot disconnected before the drain window ended");
    server->stop();

    const auto elapsed = std::chrono::duration<double>(feed_done - measured_start).count();
    std::cout << Json{{"snapshots", count - warmup}, {"warmup_snapshots", warmup},
                      {"send_rate", (count - warmup) / elapsed}, {"feed_seconds", elapsed},
                      {"first_book_late_us", std::chrono::duration<double, std::micro>(measured_start - planned_start).count()},
                      {"max_send_late_us", max_late_us}, {"sends_at_least_one_tick_late", behind_tick},
                      {"accepted_places", measured.accepted_places},
                      {"accepted_cancels", measured.accepted_cancels}, {"quota_rejections", measured.rejected},
                      {"saturated_intervals", measured.saturated_intervals}, {"drain_messages", drain_messages},
                      {"unmatched_limits", unmatched}, {"duplicate_limits", duplicates},
                      {"feedback_sent", feedback_sent}, {"feedback_send_failures", feedback_failures},
                      {"malformed_messages", malformed},
                      {"latencies_us", latencies}}.dump() << '\n';
    return malformed || feedback_failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 6) throw std::runtime_error("usage: server rate warmup_seconds seconds quota interval_ms");
        const int rate = std::stoi(argv[1]), warmup = std::stoi(argv[2]), seconds = std::stoi(argv[3]);
        const int quota = std::stoi(argv[4]), interval = std::stoi(argv[5]);
        if (rate <= 0 || warmup < 0 || seconds <= 0 || quota <= 0 || interval <= 0 ||
            (static_cast<std::int64_t>(warmup) + seconds) * rate > 1'000'000)
            throw std::runtime_error("invalid workload or more than 1,000,000 books per run");
        return run(rate, warmup, seconds, quota, interval);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
