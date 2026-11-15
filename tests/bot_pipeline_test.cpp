#include <atomic>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ixwebsocket/IXWebSocketServer.h>
#include <nlohmann/json.hpp>
#include "spread_capture_bot.hpp"

using Json = nlohmann::json;
static void check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; std::abort(); }
}

static std::string book(double mid, bool valid = true, std::string symbol = "ABC") {
    return Json{{"type", "book_snapshot"}, {"symbol", symbol},
        {"mid_price", mid}, {"microprice", mid + 0.04}, {"obi", 0.8},
        {"bids", {{100.0, valid ? 10 : -1}}}, {"asks", {{100.1, 10}}}}.dump();
}

static std::string fill(bool buy, int quantity) {
    return Json{{"type", "trade_execution"}, {"symbol", "ABC"},
        {"buyer_id", buy ? "part2_test" : "other"},
        {"seller_id", buy ? "other" : "part2_test"},
        {"quantity", quantity}, {"price", 100.0}}.dump();
}

int main() {
    const std::vector<std::string> first = {
        R"({"type":"session_event","event":"SESSION_OPEN","message":"open"})",
        fill(true, 5), book(100.05), fill(false, 5), book(100.05)
    };
    const std::vector<std::string> feedback = {
        R"({"type":"order_ack","symbol":"ABC","order_id":"id-1","side":"buy","price":100,"queue_ahead":4,"level_qty":5})",
        book(100.05),
        R"({"type":"queue_update","symbol":"ABC","order_id":"id-1","queue_ahead":201,"level_qty":300})",
        book(100.05)
    };
    const std::vector<std::string> last = {
        fill(true, 1), book(200.0, false),
        R"({"type":"session_event","event":"SESSION_CLOSED","message":"closed"})"
    };
    arena::ClientConfig cfg{};
    cfg.team_id = "part2_test";
    cfg.url = "ws://127.0.0.1:1"; // Replay must never connect.
    auto replay = std::make_unique<arena::SpreadCaptureBot>(cfg);
    std::stringstream input, output;
    for (const auto& line : first) input << line << '\n';
    for (const auto& line : feedback) input << line << '\n';
    for (const auto& line : last) input << line << '\n';
    input << '\n';
    replay->run_replay(input, output);
    std::vector<Json> expected_orders;
    std::string line;
    std::size_t lines = 0;
    while (std::getline(output, line)) {
        auto response = Json::parse(line);
        check(response.is_array(), "one array per input line");
        if (lines == first.size() - 1 || lines == first.size() + feedback.size() - 1 ||
            lines == first.size() + feedback.size() + last.size() - 1)
            check(response.size() == 1, "order belongs to triggering input");
        else check(response.empty(), "no spurious order");
        for (auto& order : response) expected_orders.push_back(order);
        ++lines;
    }
    check(lines == first.size() + feedback.size() + last.size() + 1, "empty replay line preserved");
    check(expected_orders.size() == 3, "expected place, cancel, and flatten");
    check(expected_orders[0]["side"] == "buy" && expected_orders[0]["price"] == 100.0,
          "join the bid after inventory falls");
    check(expected_orders[1]["type"] == "cancel_order" && expected_orders[1]["order_id"] == "id-1",
          "cancel the acknowledged order when queue position deteriorates");
    check(replay->strategy().find("ABC")->position == 1 &&
          !replay->strategy().find("ABC")->local_book->valid(), "replay final state");

    // Local exchange: never connects to a real venue. Try a small port range
    // because IXWebSocket's server API does not expose the port assigned to 0.
    std::unique_ptr<ix::WebSocketServer> server;
    int port = 0;
    for (int candidate = 19100; candidate < 19120; ++candidate) {
        auto attempt = std::make_unique<ix::WebSocketServer>(candidate, "127.0.0.1");
        if (attempt->listen().first) { server = std::move(attempt); port = candidate; break; }
    }
    check(server != nullptr, "local server listen");
    std::atomic<bool> completed{false};
    std::atomic<bool> bad_case{false};
    std::size_t orders = 0; // Only the server callback writes; inspected after stop.
    server->setOnClientMessageCallback([&](auto, ix::WebSocket& socket, const auto& message) {
        if (message->type != ix::WebSocketMessageType::Message) return;
        const auto value = Json::parse(message->str);
        if (value["type"] == "handshake") {
            if (bad_case.load()) {
                socket.sendText(book(100.0, true, "ABCDEFGHIJKLMNOPQ"));
            } else {
                for (const auto& frame : first) socket.sendText(frame);
            }
        } else if (value["type"] == "place_order" || value["type"] == "cancel_order") {
            check(orders < expected_orders.size() && value == expected_orders[orders],
                  "live order matches synchronous replay");
            ++orders;
            if (orders == 1) {
                for (const auto& frame : feedback) socket.sendText(frame);
            } else if (orders == 2) {
                for (const auto& frame : last) socket.sendText(frame);
            } else completed.store(true);
        }
    });
    server->start();
    cfg.url = "ws://127.0.0.1:" + std::to_string(port);
    auto live = std::make_unique<arena::SpreadCaptureBot>(cfg);
    std::exception_ptr error;
    std::thread run([&] { try { live->run(); } catch (...) { error = std::current_exception(); } });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!completed.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    live->stop();
    run.join();
    check(completed.load() && !error, "live pipeline and shutdown complete");
    check(live->strategy().processed() == first.size() + feedback.size() + last.size(),
          "all events drained");
    check(live->strategy().find("ABC")->position == 1 &&
          !live->strategy().find("ABC")->local_book->valid() &&
          live->strategy().queue().size() == 0, "live final state");

    // Conversion failure must stop the live run and surface failure, not trade.
    bad_case.store(true);
    auto invalid = std::make_unique<arena::SpreadCaptureBot>(cfg);
    std::atomic<bool> fault_finished{false};
    error = nullptr;
    std::thread failed([&] {
        try { invalid->run(); } catch (...) { error = std::current_exception(); }
        fault_finished.store(true);
    });
    const auto fault_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fault_finished.load() && std::chrono::steady_clock::now() < fault_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool stopped_itself = fault_finished.load();
    invalid->stop();
    failed.join();
    check(stopped_itself && error, "conversion fault stops and reports failure");
    server->stop();
    check(orders == 3, "fault produces no additional orders");

    auto stopped = std::make_unique<arena::SpreadCaptureBot>(cfg);
    stopped->stop();
    stopped->run(); // A stop before startup must not be overwritten by run().
    check(stopped->strategy().processed() == 0, "stop before startup");
    std::cout << "bot callback, replay, fault, and shutdown integration tests passed\n";
}
