#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

struct NewOrder {
    const char* clordid; int clordid_len;   // tag 11, borrowed from input
    char symbol[16]; char side;             // tags 55, 54
    uint32_t qty; double price;             // tags 38, 44
};

namespace fix {
namespace detail {
constexpr char soh = '\x01';
constexpr unsigned all_fields = 31;

inline bool digit(char c) { return c >= '0' && c <= '9'; }

// Slow path for decimal values beyond the exact integer fast path. Work in
// decimal until we have a binary64 significand plus its rounding information.
// Binary64 overflows beyond 309 integer digits; its finest rounding midpoint
// is 2^-1075, so digits past decimal place 1075 only break exact ties.
inline bool long_price(std::string_view whole, std::string_view fraction, double& out) {
    static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);
    constexpr uint64_t leading_bit = uint64_t{1} << 52;
    const auto first = whole.find_first_not_of('0');
    whole.remove_prefix(first == whole.npos ? whole.size() : first);
    if (whole.size() > 309) return false;

    std::array<unsigned char, 309> integer;
    for (std::size_t i = 0; i < whole.size(); ++i) integer[i] = whole[i] - '0';
    std::size_t begin = 0;
    uint64_t significand = 0;
    int exponent = 0;
    bool guard = false, sticky = false;
    for (;;) {
        if (whole.size() - begin <= 16) {
            significand = 0;
            for (auto i = begin; i < whole.size(); ++i)
                significand = significand * 10 + integer[i];
            if (significand < 2 * leading_bit) break;
        }
        unsigned carry = 0;
        for (auto i = begin; i < whole.size(); ++i) {
            unsigned d = carry * 10 + integer[i];
            integer[i] = static_cast<unsigned char>(d / 2);
            carry = d % 2;
        }
        if (integer[begin] == 0) ++begin;
        sticky |= guard;
        guard = carry != 0;
        ++exponent;
    }

    if (exponent > 0) {
        sticky |= fraction.find_first_not_of('0') != fraction.npos;
    } else {
        std::array<unsigned char, 1075> decimal;
        auto used = std::min(fraction.size(), decimal.size());
        sticky = fraction.substr(used).find_first_not_of('0') != fraction.npos;
        for (std::size_t i = 0; i < used; ++i) decimal[i] = fraction[i] - '0';
        // Doubling a decimal fraction emits its next binary bit as the carry.
        auto next_bit = [&] {
            unsigned carry = 0;
            for (auto i = used; i > 0; --i) {
                unsigned d = 2 * decimal[i - 1] + carry;
                decimal[i - 1] = static_cast<unsigned char>(d % 10);
                carry = d / 10;
            }
            while (used && decimal[used - 1] == 0) --used;
            return carry;
        };
        while (used && significand < leading_bit && exponent > -1074) {
            significand = 2 * significand + next_bit();
            --exponent;
        }
        guard = next_bit() != 0;
        sticky |= used != 0;
    }
    if (guard && (sticky || (significand & 1))) ++significand;
    out = std::ldexp(static_cast<double>(significand), exponent);
    return std::isfinite(out);
}

// The caller has already found the SOH at end. Convert ordinary decimal
// prices with one correctly rounded division of two exactly representable ints.
inline bool price(const char* p, const char* end, double& out) {
    const bool negative = *p == '-';
    if (negative || *p == '+') ++p;
    const char* digits = p;
    const char* point = nullptr;
    uint64_t coefficient = 0;
    for (; p != end; ++p) {
        if (digit(*p)) {
            coefficient = coefficient * 10 + static_cast<unsigned>(*p - '0');
        } else if (*p == '.' && !point) {
            point = p;
        } else {
            return false;
        }
    }
    const auto count = end - digits - (point != nullptr);
    if (count == 0) return false;
    if (count <= 15) {
        constexpr double powers[] = {1, 10, 100, 1000, 10000, 1e5, 1e6, 1e7,
                                     1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15};
        double value = static_cast<double>(coefficient) / powers[point ? end - point - 1 : 0];
        out = negative ? -value : value;
        return true;
    }
    const auto* integer_end = point ? point : end;
    const auto* fraction = point ? point + 1 : end;
    double magnitude;
    if (!long_price({digits, static_cast<std::size_t>(integer_end - digits)},
                    {fraction, static_cast<std::size_t>(end - fraction)}, magnitude))
        return false;
    out = negative ? -magnitude : magnitude;
    return true;
}

// Decode one field; 'end' points at its SOH. Unknown numeric tags are skipped.
inline bool field(const char* p, const char* end, NewOrder& order, unsigned& seen) {
    if (p == end || !digit(*p) || *p == '0') return false;
    const char* tag = p;
    do { ++p; } while (p != end && digit(*p));
    if (p == end || *p != '=') return false;
    const auto tag_len = p - tag;
    ++p;
    if (p == end) return false;
    if (tag_len != 2) return true;
    const int number = (tag[0] - '0') * 10 + (tag[1] - '0');
    const auto n = static_cast<std::size_t>(end - p);
    unsigned flag;
    switch (number) {
    case 11:
        flag = 1;
        order.clordid = p;
        order.clordid_len = static_cast<int>(n);
        break;
    case 55: {
        flag = 2;
        const auto copied = n < sizeof(order.symbol) ? n : sizeof(order.symbol) - 1;
        std::memcpy(order.symbol, p, copied);
        order.symbol[copied] = '\0';
        break;
    }
    case 54:
        flag = 4;
        if (n != 1) return false;
        order.side = *p;
        break;
    case 38: {
        flag = 8;
        auto r = std::from_chars(p, end, order.qty);
        if (r.ec != std::errc{} || r.ptr != end) return false;
        break;
    }
    case 44:
        flag = 16;
        if (!price(p, end, order.price)) return false;
        break;
    default:
        return true;
    }
    if (seen & flag) return false;
    seen |= flag;
    return true;
}
} // namespace detail
} // namespace fix

// Parse the caller's single message slice. Only publish a complete order.
inline bool parse_new_order(const char* buf, int len, NewOrder& out) {
    if (!buf || len <= 0) return false;
    const char* end = buf + len;
    NewOrder order{};
    unsigned seen = 0;
    while (buf != end) {
        const auto* stop = static_cast<const char*>(std::memchr(buf, fix::detail::soh, end - buf));
        if (!stop || !fix::detail::field(buf, stop, order, seen)) return false;
        buf = stop + 1;
    }
    if (seen != fix::detail::all_fields) return false;
    out = order;
    return true;
}

namespace fix {
enum class status { ok, incomplete, malformed };

// On success, expose the body and its total wire frame size. BodyLength counts
// from just after tag 9's SOH through the SOH immediately preceding tag 10.
inline status frame(std::string_view wire, std::string_view& body, std::size_t& size) {
    constexpr std::string_view begin_tag = "8=";
    constexpr std::string_view length_tag = "9=";
    constexpr std::size_t trailer_size = 7; // 10=ddd<SOH>
    if (wire.substr(0, 2) != begin_tag.substr(0, wire.size())) return status::malformed;
    const auto first_end = wire.find(detail::soh, 2);
    if (first_end == wire.npos) return status::incomplete;
    if (first_end == 2) return status::malformed;

    const auto length_field = wire.substr(first_end + 1);
    if (length_field.substr(0, 2) != length_tag.substr(0, length_field.size()))
        return status::malformed;
    if (length_field.size() <= 2) return status::incomplete;
    const char* first = length_field.data() + 2;
    const char* end = wire.data() + wire.size();
    int length;
    auto r = std::from_chars(first, end, length);
    if (!detail::digit(*first) || r.ec != std::errc{}) return status::malformed;
    if (r.ptr == end) return status::incomplete;
    if (*r.ptr != detail::soh || length <= 0) return status::malformed;
    const auto after_length = wire.substr(r.ptr + 1 - wire.data());
    const auto n = static_cast<std::size_t>(length);
    if (after_length.size() < n) return status::incomplete;
    if (after_length[n - 1] != detail::soh) return status::malformed;
    const auto trailer = after_length.substr(n);
    constexpr std::string_view prefix = "10=";
    if (trailer.substr(0, 3) != prefix.substr(0, trailer.size())) return status::malformed;
    for (std::size_t i = 3; i < 6 && i < trailer.size(); ++i)
        if (!detail::digit(trailer[i])) return status::malformed;
    if (trailer.size() < trailer_size) return status::incomplete;
    if (trailer[6] != detail::soh) return status::malformed;
    body = after_length.substr(0, n);
    size = wire.size() - trailer.size() + trailer_size;
    return status::ok;
}

// Repeated calls drain a TCP receive buffer or one datagram. An incomplete
// tail stays at the start of pending for the caller to retain across reads.
inline status next_new_order(std::string_view& pending, NewOrder& out) {
    std::string_view body;
    std::size_t size;
    auto result = frame(pending, body, size);
    if (result != status::ok) return result;
    if (!parse_new_order(body.data(), static_cast<int>(body.size()), out))
        return status::malformed;
    pending.remove_prefix(size);
    return status::ok;
}
} // namespace fix
