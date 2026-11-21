#include "u64toa.hpp"
#include "bench.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <random>
#include <string>
#include <vector>

#if defined(__APPLE__) && defined(__aarch64__)
#include <libproc.h>
#include <unistd.h>
#endif

namespace {
using U64 = std::uint64_t;
constexpr U64 max_value = std::numeric_limits<U64>::max();
constexpr int runs = 15;
constexpr int passes = 24;
constexpr std::size_t input_count = 8192;

// Same scratch buffer and final copy for the two intermediate algorithms.
template <unsigned Base>
int write_backwards(U64 value, char* out) {
    static_assert(Base == 10 || Base == 100);
    char digits[20];
    char* const end = digits + sizeof(digits);
    char* p = end;
    while (value >= Base) {
        const unsigned remainder = static_cast<unsigned>(value % Base);
        if constexpr (Base == 100) {
            p -= 2;
            u64toa_detail::write_pair(remainder, p);
        } else {
            *--p = static_cast<char>('0' + remainder);
        }
        value /= Base;
    }
    if (value < 10) {
        *--p = static_cast<char>('0' + value);
    } else {
        p -= 2;
        u64toa_detail::write_pair(static_cast<unsigned>(value), p);
    }
    const int n = static_cast<int>(end - p);
    std::memcpy(out, p, n);
    return n;
}

template <std::size_t Capacity, class Writer>
void check_bytes(const std::string& expected, Writer write) {
    std::array<char, Capacity + 2> buffer;
    buffer.fill('#');
    const int n = write(buffer.data() + 1);
    if (n != static_cast<int>(expected.size()) ||
        buffer.front() != '#' ||
        std::memcmp(buffer.data() + 1, expected.data(), expected.size()) != 0 ||
        !std::all_of(buffer.begin() + 1 + expected.size(), buffer.end(),
                     [](char c) { return c == '#'; })) {
        std::fprintf(stderr, "Incorrect length, bytes or bounds for %s\n", expected.c_str());
        std::exit(EXIT_FAILURE);
    }
}

void check_value(U64 v) {
    const std::string expected = std::to_string(v);
    check_bytes<20>(expected, [&](char* p) { return u64toa(v, p); });
    check_bytes<20>(expected, [&](char* p) { return write_backwards<10>(v, p); });
    check_bytes<20>(expected, [&](char* p) { return write_backwards<100>(v, p); });
}

// digits=0 mixes all 20 lengths with equal probability.
std::vector<U64> make_inputs(unsigned digits) {
    std::array<U64, 20> powers{1};
    for (std::size_t i = 1; i < powers.size(); ++i) powers[i] = powers[i - 1] * 10;
    std::mt19937_64 random(20261121 + digits);
    std::uniform_int_distribution<unsigned> lengths(1, 20);
    std::vector<U64> inputs(input_count);
    for (U64& v : inputs) {
        const unsigned d = digits ? digits : lengths(random);
        const U64 lo = d == 1 ? 0 : powers[d - 1];
        const U64 hi = d == 20 ? max_value : powers[d] - 1;
        v = std::uniform_int_distribution<U64>(lo, hi)(random);
    }
    return inputs;
}

U64 read_counter() {
#if defined(__APPLE__) && defined(__aarch64__)
    rusage_info_v4 usage{};
    if (proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&usage)) ||
        !usage.ri_cycles) {
        std::fputs("Process CPU cycle counter unavailable\n", stderr);
        std::exit(EXIT_FAILURE);
    }
    return usage.ri_cycles;
#elif defined(__x86_64__) || defined(__i386__)
    _mm_lfence();
    const auto t = cycle_count();
    _mm_lfence();
    return t;
#else
    return cycle_count(); // Course helper's constant-rate timer fallback.
#endif
}

// Make every written byte observable while it is still alive.
inline void consume(const char* bytes, std::size_t size) {
    asm volatile("" : : "r"(bytes), "r"(size) : "memory");
}

struct Sample { double ticks, ns; };

template <class Format>
Sample measure(const std::vector<U64>& inputs, Format format) {
    using Clock = std::chrono::steady_clock;
    const U64 c0 = read_counter();
    const auto start = Clock::now();
    for (int pass = 0; pass < passes; ++pass)
        for (U64 v : inputs) format(v);
    const auto elapsed = Clock::now() - start;
    const U64 ticks = read_counter() - c0;
    const double ops = static_cast<double>(inputs.size() * passes);
    return {ticks / ops, std::chrono::duration<double, std::nano>(elapsed).count() / ops};
}

enum Method { scalar, pairs, final_version, c_library, cpp_library, method_count };
constexpr std::array<const char*, method_count> names{
    "digit loop", "pair loop", "u64toa", "snprintf", "std::to_string"};

Sample run(Method method, const std::vector<U64>& inputs) {
    char out[32];
    switch (method) {
    case scalar:
        return measure(inputs, [&](U64 v) { consume(out, write_backwards<10>(v, out)); });
    case pairs:
        return measure(inputs, [&](U64 v) { consume(out, write_backwards<100>(v, out)); });
    case final_version:
        return measure(inputs, [&](U64 v) { consume(out, u64toa(v, out)); });
    case c_library:
        return measure(inputs, [&](U64 v) { consume(out, std::snprintf(out, sizeof(out), "%" PRIu64, v)); });
    case cpp_library:
        return measure(inputs, [](U64 v) {
            const auto s = std::to_string(v);
            consume(s.data(), s.size());
        });
    default:
        std::abort();
    }
}

void benchmark(const std::vector<U64>& inputs, unsigned digits) {
    std::array<std::array<double, runs>, method_count> ticks{}, ns{};
    // Round -1 warms each method. Rotate the first method across measured rounds.
    for (int r = -1; r < runs; ++r) {
        for (int i = 0; i < method_count; ++i) {
            const int m = (i + r + method_count) % method_count;
            const Sample sample = run(static_cast<Method>(m), inputs);
            if (r >= 0) {
                ticks[m][r] = sample.ticks;
                ns[m][r] = sample.ns;
            }
        }
    }
    const std::string label = digits ? std::to_string(digits) : "mixed";
    for (int m = 0; m < method_count; ++m) {
        std::sort(ticks[m].begin(), ticks[m].end());
        std::sort(ns[m].begin(), ns[m].end());
        const double median_ns = ns[m][runs / 2];
        std::printf("%5s  %-14s %10.2f %8.2f %10.2f\n", label.c_str(), names[m],
                    ticks[m][runs / 2], median_ns, 1000.0 / median_ns);
    }
}
} // namespace

int main(int argc, char** argv) {
    const bool check_only = argc == 2 && std::strcmp(argv[1], "--check") == 0;
    if (argc > 1 && !check_only) return EXIT_FAILURE;
    for (U64 v = 0; v <= 9999; ++v) check_value(v);
    for (U64 p = 1; ; p *= 10) {
        for (U64 v : {p - 1, p, p + 1}) check_value(v);
        if (p > max_value / 10) break;
    }
    check_value(max_value);
    for (U64 tag : {U64{38}, max_value})
        for (U64 v : {U64{0}, U64{100000001}, max_value})
            check_bytes<42>(std::to_string(tag) + '=' + std::to_string(v) + '\x01',
                            [&](char* p) { return encode_u64_field(tag, v, p); });

#if defined(__APPLE__) && defined(__aarch64__)
    constexpr auto unit = "CPU cyc/op";
#elif defined(__x86_64__) || defined(__i386__)
    constexpr auto unit = "TSC cyc/op";
#else
    constexpr auto unit = "timer ticks/op";
#endif
    if (!check_only)
        std::printf("digits  method         %10s    ns/op     Mops/s\n", unit);
    for (unsigned digits : {1, 4, 10, 16, 20, 0}) {
        const auto inputs = make_inputs(digits);
        for (U64 v : inputs) check_value(v);
        if (!check_only) benchmark(inputs, digits);
    }
    std::puts("Byte, length, bounds and FIX field checks passed.");
}
