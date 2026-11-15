// spsc_concurrency.cpp — one producer, one consumer, cross-thread SPSC stress.
// Compiled TWICE by the grader: once normally (correctness + throughput),
// once with -fsanitize=thread (data-race check).
// N via env SPSC_N (default 2,000,000). Emits RESULT|concurrent|... and prints
// "THROUGHPUT <ops_per_sec>" for reporting.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>
#include "spsc_ring.hpp"

int main() {
    const char* env = std::getenv("SPSC_N");
    const std::uint64_t count = env ? std::strtoull(env, nullptr, 10) : 2000000ULL;
    SPSCRing ring(1u << 16);

    std::atomic<bool> failed{false};
    std::uint64_t pushed = 0;
    std::uint64_t popped = 0;
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(30);
    const auto stop_waiting = [&](unsigned spins) {
        if ((spins & 0x3FFF) != 0) return false;
        if (failed.load(std::memory_order_relaxed)) return true;
        if (std::chrono::steady_clock::now() < deadline) return false;
        failed.store(true, std::memory_order_relaxed);
        return true;
    };

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < count; ++i) {
            unsigned spins = 0;
            while (!ring.push(i)) {
                if (stop_waiting(++spins)) {
                    pushed = i;
                    return;
                }
            }
        }
        pushed = count;
    });
    std::thread consumer([&] {
        for (std::uint64_t expected = 0; expected < count; ++expected) {
            std::uint64_t value = 0;
            unsigned spins = 0;
            while (!ring.pop(value)) {
                if (stop_waiting(++spins)) {
                    popped = expected;
                    return;
                }
            }
            if (value != expected) {
                popped = expected + 1;
                failed.store(true, std::memory_order_relaxed);
                std::cerr << "FIFO mismatch: expected " << expected
                          << ", got " << value << '\n';
                return;
            }
        }
        popped = count;
    });
    producer.join();
    consumer.join();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    std::uint64_t extra = 0;
    const bool ok = !failed.load() && pushed == count && popped == count &&
                    ring.empty() && !ring.pop(extra);
    const double throughput = ok && seconds > 0 ? static_cast<double>(popped) / seconds : 0.0;

    std::cout << "RESULT|concurrent|" << (ok ? "pass" : "fail") << "|"
              << (ok ? ("moved " + std::to_string(popped) + " items in FIFO order across threads")
                     : "FIFO, count, or final empty check failed, or transfer stalled") << '\n';
    std::cout << "Pushed " << pushed << ", popped " << popped
              << ", elapsed " << seconds << " s\n";
    std::cout << "THROUGHPUT " << static_cast<long long>(throughput) << '\n';
    return ok ? 0 : 1;
}
