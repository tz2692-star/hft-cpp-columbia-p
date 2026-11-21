#include "fix_parser.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef __APPLE__
#include <libproc.h>
#include <unistd.h>
#elif defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace {
using View = std::string_view;
constexpr char soh = '\x01';
constexpr std::size_t batch_size = 4096;
constexpr std::array<View, 5> symbols{"AAPL", "IBM", "MSFT", "BRK.B", "GOOG"};

void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

std::string frame_body(const std::string& body) {
    std::string wire = "8=FIX.4.2" + std::string(1, soh) + "9=" +
                       std::to_string(body.size()) + soh + body;
    unsigned sum = 0;
    for (unsigned char c : wire) sum += c;
    char tail[8];
    std::snprintf(tail, sizeof(tail), "10=%03u%c", sum % 256, soh);
    return wire + tail;
}

unsigned qty(std::size_t i) { return 1 + (i * 157) % 20000; }
unsigned cents(std::size_t i) { return 100 + (i * 193) % 50000; }
std::string id(std::size_t i) { return "CLIENT-20261121-" + std::to_string(i); }

std::string message(std::size_t i) {
    std::string body = "35=D\1" "49=BUY-SIDE\1" "56=EXCHANGE\1" "34=";
    body += std::to_string(i + 1) + soh + "52=20261121-14:30:00.000" + soh;
    body += "11=" + id(i) + soh + "55=" + std::string(symbols[i % symbols.size()]) + soh;
    body += i % 2 ? "54=2\1" : "54=1\1";
    body += "60=20261121-14:30:00.000\1" "40=2\1" "38=" + std::to_string(qty(i)) + soh;
    body += "44=" + std::to_string(cents(i) / 100) + '.' +
            (cents(i) % 100 < 10 ? "0" : "") + std::to_string(cents(i) % 100) + soh;
    return frame_body(body);
}

// Both controls split the entire body into a fresh vector before decoding.
// The only difference between them is ownership of the field bytes.
template<class Token>
bool split_order(const char* buf, int len, NewOrder& out) {
    View rest(buf, len);
    std::vector<Token> fields;
    while (!rest.empty()) {
        auto pos = rest.find(soh);
        if (pos == rest.npos) return false;
        fields.emplace_back(rest.data(), pos + 1);
        rest.remove_prefix(pos + 1);
    }
    NewOrder order{};
    unsigned seen = 0;
    const char* wire = buf;
    for (const auto& field : fields) {
        const char* start = field.data();
        const bool had_id = order.clordid != nullptr;
        if (!fix::detail::field(start, start + field.size() - 1, order, seen)) return false;
        // The owned baseline must return a view into the original receive buffer too.
        if (!had_id && order.clordid) order.clordid = wire + (order.clordid - start);
        wire += field.size();
    }
    if (seen != fix::detail::all_fields) return false;
    out = order;
    return true;
}

using Parser = bool (*)(const char*, int, NewOrder&);
constexpr std::array<Parser, 3> parsers{split_order<std::string>, split_order<View>, parse_new_order};

void check_batch(const std::string& tape, Parser parse) {
    View pending = tape;
    for (std::size_t i = 0; i < batch_size; ++i) {
        View body;
        std::size_t size;
        check(fix::frame(pending, body, size) == fix::status::ok, "generated frame");
        NewOrder o{};
        check(parse(body.data(), static_cast<int>(body.size()), o), "generated body");
        auto expected_id = id(i);
        check(o.clordid == body.data() + body.find("11=") + 3 &&
              View(o.clordid, o.clordid_len) == expected_id &&
              o.symbol == symbols[i % symbols.size()] && o.side == (i % 2 ? '2' : '1') &&
              o.qty == qty(i) && o.price == cents(i) / 100.0, "decoded order mismatch");
        pending.remove_prefix(size);
    }
    check(pending.empty(), "unexpected bytes after batch");
}

void check_edges() {
    const std::string fields = "54=2\1" "10001=ignored\1" "11=X\1" "38=4294967295\1"
                               "55=12345678901234567890\1";
    const auto valid = fields + "44=-0.125\1";
    NewOrder order{};
    check(parse_new_order(valid.data(), static_cast<int>(valid.size()), order) &&
          order.qty == UINT32_MAX && View(order.symbol) == "123456789012345" &&
          order.price == -0.125, "field order, uint32 limit, symbol bound or signed price");
    const auto precise = fields + "44=0.1234567890123456789012345\1";
    check(parse_new_order(precise.data(), static_cast<int>(precise.size()), order) &&
          order.price == 0.1234567890123456789012345, "long decimal");
    for (const auto& example : std::array<std::pair<const char*, double>, 3>{{
             {"9007199254740993", 9007199254740992.0},
             {"9007199254740993.00000001", 9007199254740994.0},
             {"1.00000000000000011102230246251565404236316680908203125", 1.0}}}) {
        auto input = fields + "44=" + example.first + soh;
        check(parse_new_order(input.data(), static_cast<int>(input.size()), order) &&
              order.price == example.second, "decimal rounding boundary");
    }
    for (const char* bad : {"", "44=\1", "44=.\1", "44=1.2.3\1", "44=1x\1", "44=1",
                            "44=1\1" "38=1\1", "44=1\1" "x=y\1"}) {
        auto input = fields + bad;
        check(!parse_new_order(input.data(), static_cast<int>(input.size()), order), "malformed field");
    }
    for (const char* bad : {"38=4294967296\1", "38=1.1\1", "38=-1\1", "38=\1"}) {
        std::string input = std::string("11=A\1" "55=B\1" "54=1\1" "44=1\1") + bad;
        check(!parse_new_order(input.data(), static_cast<int>(input.size()), order), "invalid quantity");
    }
    check(!parse_new_order(nullptr, 0, order), "empty input");

    const auto first = message(0);
    const auto second = message(1);
    const auto wire = first + second;
    for (std::size_t cut = 0; cut <= wire.size(); ++cut) {
        View pending(wire.data(), cut);
        unsigned consumed = 0;
        while (fix::next_new_order(pending, order) == fix::status::ok) ++consumed;
        check(consumed == (cut >= first.size()) + static_cast<unsigned>(cut == wire.size()),
              "order count at TCP cut");
        auto offset = static_cast<std::size_t>(pending.data() - wire.data());
        check(offset == (consumed == 0 ? 0 : consumed == 1 ? first.size() : wire.size()),
              "TCP cut consumed part of a frame");
        check(fix::next_new_order(pending, order) == fix::status::incomplete, "partial frame");
        pending = View(wire).substr(offset); // retained prefix plus next read
        while (!pending.empty()) {
            check(fix::next_new_order(pending, order) == fix::status::ok, "resumed frame");
            check(View(order.clordid, order.clordid_len) == id(consumed++), "resumed ID");
        }
        check(consumed == 2, "resumed count");
    }
    for (View bad : {"8=F\1" "9=x\1", "8=F\1" "9=999999999999\1", "8=F\1" "9=3\1"
                     "a=b\1" "10=000\1", "8=F\1" "9=4\1" "a=b\1" "10=00x\1",
                     "8=F\1" "9=4\1" "a=b\1" "10=000\1"}) {
        View copy = bad;
        check(fix::next_new_order(copy, order) == fix::status::malformed && copy == bad,
              "invalid frame advanced input");
    }
}

class CycleCounter {
#ifdef __linux__
    int fd_;
#endif
public:
    CycleCounter() {
#ifdef __linux__
        perf_event_attr attr{};
        attr.size = sizeof(attr);
        attr.type = PERF_TYPE_HARDWARE;
        attr.config = PERF_COUNT_HW_CPU_CYCLES;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        fd_ = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
        check(fd_ >= 0, "CPU cycle counter unavailable (perf_event_open)");
#endif
    }
    ~CycleCounter() {
#ifdef __linux__
        close(fd_);
#endif
    }
    uint64_t read_cycles() const {
#ifdef __APPLE__
        rusage_info_v4 info{};
        check(proc_pid_rusage(getpid(), RUSAGE_INFO_V4, reinterpret_cast<rusage_info_t*>(&info)) == 0
              && info.ri_cycles != 0, "CPU cycle counter unavailable (proc_pid_rusage)");
        return info.ri_cycles;
#elif defined(__linux__)
        uint64_t result;
        check(read(fd_, &result, sizeof(result)) == sizeof(result), "read cycle counter");
        return result;
#else
        throw std::runtime_error("hardware cycle measurements require macOS or Linux");
#endif
    }
};

uint64_t run_batch(View pending, Parser parse) {
    uint64_t sum = 0;
    while (!pending.empty()) {
        View body;
        std::size_t size;
        NewOrder order;
        if (fix::frame(pending, body, size) != fix::status::ok ||
            !parse(body.data(), static_cast<int>(body.size()), order))
            throw std::runtime_error("benchmark parse failed");
        sum += order.qty + static_cast<unsigned>(order.price) + order.symbol[0] +
               order.side + order.clordid[order.clordid_len - 1];
        pending.remove_prefix(size);
    }
    return sum;
}

void measure(const std::string& tape) {
    constexpr int rounds = 9, repeats = 16;
    constexpr double operations = repeats * batch_size;
    std::array<std::array<double, rounds>, parsers.size()> cycles{}, ns{};
    CycleCounter counter;
    const auto expected = run_batch(tape, parse_new_order);
    for (auto parse : parsers) check(run_batch(tape, parse) == expected, "warmup mismatch");
    for (int r = 0; r < rounds; ++r) {
        for (std::size_t slot = 0; slot < parsers.size(); ++slot) {
            const auto k = (slot + r) % parsers.size();
            uint64_t sum = 0;
            auto t0 = std::chrono::steady_clock::now();
            auto c0 = counter.read_cycles();
            for (int i = 0; i < repeats; ++i) {
                std::atomic_signal_fence(std::memory_order_acq_rel);
                sum += run_batch(tape, parsers[k]);
            }
            auto c1 = counter.read_cycles();
            auto t1 = std::chrono::steady_clock::now();
            check(sum == repeats * expected, "timed result mismatch");
            cycles[k][r] = (c1 - c0) / operations;
            ns[k][r] = std::chrono::duration<double, std::nano>(t1 - t0).count() / operations;
        }
    }
    const char* names[] = {"split (owned)", "split (views)", "direct"};
    for (std::size_t k = 0; k < parsers.size(); ++k) {
        std::sort(cycles[k].begin(), cycles[k].end());
        std::sort(ns[k].begin(), ns[k].end());
        const double median_cycles = cycles[k][rounds / 2];
        const double median_ns = ns[k][rounds / 2];
        const double million_orders_per_second = 1'000.0 / median_ns;
        std::printf("%-14s %8.2f cycles/op  %8.2f ns/op  %6.2f million orders/s  %5.2fx vs owned\n",
                    names[k], median_cycles, median_ns, million_orders_per_second,
                    cycles[0][rounds / 2] / median_cycles);
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        check_edges();
        std::string tape;
        for (std::size_t i = 0; i < batch_size; ++i) tape += message(i);
        for (auto parse : parsers) check_batch(tape, parse);
        std::cout << "Passed field, framing and batch checks (" << batch_size << " messages, "
                  << tape.size() << " bytes).\n";
        if (argc == 2 && View(argv[1]) == "--check") return 0;
        check(argc == 1, "usage: fix_bench [--check]");
        measure(tape);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
