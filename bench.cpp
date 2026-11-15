// Throwaway HW7 benchmark. Run from the repository root:
// g++ -std=c++20 -O2 -DNDEBUG -Iinclude -Itests bench.cpp -o /tmp/hw7_bench
// /tmp/hw7_bench
// Requires GCC or Clang: the barriers below stop lookup elimination/hoisting.
#include "order_book.hpp"
#include "bench.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using Clock = std::chrono::steady_clock;

// Make the object's memory observable and potentially changed to the compiler.
// This executes no CPU instruction; it prevents caching a fixed BBO outside
// the loop. Both alternatives use identical barriers.
template <class T>
inline void observe(const T& object) {
    asm volatile("" : : "g"(&object) : "memory");
}

template <class T>
inline void consume(const T& value) {
    asm volatile("" : : "g"(value) : "memory");
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "Correctness check failed: %s\n", message);
        std::exit(EXIT_FAILURE);
    }
}

bool same_price(double a, double b) {
    return std::abs(a - b) < 1e-9;
}

struct MapBook {
    std::map<int, uint32_t> bids, asks;

    void add(char side, double price, uint32_t quantity) {
        const int tick = static_cast<int>(std::llround(price * 100.0));
        (side == 'B' ? bids : asks)[tick] += quantity;
    }

    double best_bid() const {
        return bids.empty() ? 0.0 : bids.rbegin()->first * 0.01;
    }

    double best_ask() const {
        return asks.empty() ? 0.0 : asks.begin()->first * 0.01;
    }
};

// Warm up, choose a batch lasting at least 20 ms, then take the median of
// five batches. All initialization and correctness checks happen elsewhere.
template <class Operation>
double ns_per_op(Operation operation) {
    for (std::size_t i = 0; i < 4096; ++i) operation(i);

    const auto elapsed_ns = [&](std::size_t count) {
        const auto start = Clock::now();
        for (std::size_t i = 0; i < count; ++i) operation(i);
        return std::chrono::duration<double, std::nano>(
            Clock::now() - start).count();
    };

    std::size_t count = 1u << 16;
    while (elapsed_ns(count) < 20'000'000.0 && count < (1u << 28))
        count *= 2;

    std::array<double, 5> samples{};
    for (double& sample : samples) sample = elapsed_ns(count) / count;
    std::sort(samples.begin(), samples.end());
    return samples[samples.size() / 2];
}

void result(const char* name, double ns) {
    std::printf("%-34s ns_per_op=%.3f\n", name, ns);
}

int main() {
    // Heap allocation avoids placing the large flat arrays on the stack.
    auto flat = std::make_unique<Book>();
    MapBook tree;
    require(same_price(flat->best_bid(), tree.best_bid()) &&
            same_price(flat->best_ask(), tree.best_ask()), "empty BBO");

    constexpr std::size_t order_count = 1024;
    for (std::size_t i = 0; i < order_count; ++i) {
        const bool bid = (i % 2 == 0);
        const int offset = static_cast<int>((i / 2) % 256);
        const int tick = bid ? 9999 - offset : 10001 + offset;
        const char side = bid ? 'B' : 'S';
        const double price = tick * 0.01;
        const uint32_t quantity = 1 + static_cast<uint32_t>(i % 9);
        flat->add(i + 1, side, price, quantity);
        tree.add(side, price, quantity);
    }
    require(same_price(flat->best_bid(), tree.best_bid()), "populated best bid");
    require(same_price(flat->best_ask(), tree.best_ask()), "populated best ask");

    constexpr std::size_t symbol_count = 1024; // Power of two for query indexing.
    auto symbols = std::make_unique<SymMap>();
    std::unordered_map<std::string, uint64_t> standard;
    standard.reserve(symbol_count);
    std::vector<std::string> queries;
    queries.reserve(symbol_count);
    for (std::size_t i = 0; i < symbol_count; ++i) {
        char key[17];
        std::snprintf(key, sizeof(key), "SYM%04u", static_cast<unsigned>(i));
        queries.emplace_back(key);
        symbols->put(key, i + 1);
        standard.emplace(key, i + 1);
    }
    std::mt19937 random(42);
    std::shuffle(queries.begin(), queries.end(), random);
    for (const std::string& key : queries)
        require(symbols->get(key.c_str()) == standard.at(key), "symbol lookup");
    require(symbols->get("MISSING") == static_cast<uint64_t>(-1), "absent symbol");

    std::puts("Correctness checks: PASS (benchmark inputs only)");
    std::puts("Median of 5 warmed batches; setup excluded; GCC/Clang barriers enabled.");
    std::puts("Book: 1024 orders, 256 price levels per side. One BBO op reads BOTH sides.");

    const double flat_bbo = ns_per_op([&](std::size_t) {
        observe(*flat);
        const double bid = flat->best_bid();
        const double ask = flat->best_ask();
        consume(bid);
        consume(ask);
    });
    const double tree_bbo = ns_per_op([&](std::size_t) {
        observe(tree);
        const double bid = tree.best_bid();
        const double ask = tree.best_ask();
        consume(bid);
        consume(ask);
    });
    result("Book BBO", flat_bbo);
    result("std::map<int,uint32_t> BBO", tree_bbo);
    std::printf("BBO speedup (map / flat): %.2fx\n\n", tree_bbo / flat_bbo);

    // Separate best-bid measurement for the original homework's wording.
    const double flat_bid = ns_per_op([&](std::size_t) {
        observe(*flat);
        const double price = flat->best_bid();
        consume(price);
    });
    const double tree_bid = ns_per_op([&](std::size_t) {
        observe(tree);
        const double price = tree.best_bid();
        consume(price);
    });
    result("Book best_bid", flat_bid);
    result("std::map<int,uint32_t> best_bid", tree_bid);
    std::printf("Best-bid speedup (map / flat): %.2fx\n\n", tree_bid / flat_bid);

    std::puts("Symbols: 1024 keys, successful lookups, fixed shuffled query order.");
    std::puts("std::string queries are prebuilt: no string construction in the timed loop.");
    const double flat_get = ns_per_op([&](std::size_t i) {
        observe(*symbols);
        const auto& key = queries[i & (symbol_count - 1)];
        const uint64_t id = symbols->get(key.c_str());
        consume(id);
    });
    const double standard_get = ns_per_op([&](std::size_t i) {
        observe(standard);
        const auto& key = queries[i & (symbol_count - 1)];
        const uint64_t id = standard.find(key)->second;
        consume(id);
    });
    result("SymMap::get", flat_get);
    result("std::unordered_map::find", standard_get);
    std::printf("Lookup speedup (unordered / flat): %.2fx\n", standard_get / flat_get);
    std::puts("A factor above 1 favors the flat implementation; below 1 favors the baseline.");

    // Additional course-helper tick measurements, separately timed.
    // On x86 these are TSC reference ticks, not variable-frequency core cycles.
    std::puts("\nCourse helper counter measurements: median of 5 batches of 2000000 ops.");
#if defined(__x86_64__) || defined(__i386__)
    std::puts("Counter: x86 TSC reference ticks (not actual variable-frequency core cycles).");
#elif defined(__aarch64__)
    std::puts("Counter: ARM virtual timer ticks (not core cycles).");
#else
    std::puts("Counter: steady_clock nanosecond fallback (not core cycles).");
#endif
    const auto ticks = [](const char* name, auto operation) {
        std::array<double, 5> samples{};
        for (auto& sample : samples)
            sample = cycles_per_op(operation, 2'000'000);
        std::sort(samples.begin(), samples.end());
        std::printf("%-34s helper_ticks_per_op=%.3f\n", name, samples[2]);
    };
    ticks("Book BBO", [&] {
        observe(*flat);
        const double bid = flat->best_bid(), ask = flat->best_ask();
        consume(bid); consume(ask);
    });
    ticks("std::map<int,uint32_t> BBO", [&] {
        observe(tree);
        const double bid = tree.best_bid(), ask = tree.best_ask();
        consume(bid); consume(ask);
    });
    ticks("Book best_bid", [&] {
        observe(*flat);
        const double bid = flat->best_bid(); consume(bid);
    });
    ticks("std::map<int,uint32_t> best_bid", [&] {
        observe(tree);
        const double bid = tree.best_bid(); consume(bid);
    });
    std::size_t custom_index = 0, standard_index = 0;
    ticks("SymMap::get", [&] {
        observe(*symbols);
        const auto& key = queries[custom_index++ & (symbol_count - 1)];
        const uint64_t id = symbols->get(key.c_str()); consume(id);
    });
    ticks("std::unordered_map::find", [&] {
        observe(standard);
        const auto& key = queries[standard_index++ & (symbol_count - 1)];
        const uint64_t id = standard.find(key)->second; consume(id);
    });
    return 0;
}


