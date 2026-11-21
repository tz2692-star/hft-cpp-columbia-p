#pragma once

#include <cstdint>
#include <cstring>

namespace u64toa_detail {

// v is in [0, 99]. memcpy allows an unaligned two-byte store.
inline void write_pair(unsigned v, char* out) {
    static constexpr char digits[] =
        "00010203040506070809" "10111213141516171819"
        "20212223242526272829" "30313233343536373839"
        "40414243444546474849" "50515253545556575859"
        "60616263646566676869" "70717273747576777879"
        "80818283848586878889" "90919293949596979899";
    std::memcpy(out, digits + 2 * v, 2);
}

inline void write_four(unsigned v, char* out) {
    write_pair(v / 100, out);
    write_pair(v % 100, out + 2);
}

inline void write_eight(unsigned v, char* out) {
    write_four(v / 10000, out);
    write_four(v % 10000, out + 4);
}

// Only the leading group may omit zeros. Precondition: v < 10000.
inline int write_leading_four(unsigned v, char* out) {
    if (v < 10) {
        out[0] = static_cast<char>('0' + v);
        return 1;
    }
    if (v < 100) {
        write_pair(v, out);
        return 2;
    }
    if (v < 1000) {
        out[0] = static_cast<char>('0' + v / 100);
        write_pair(v % 100, out + 1);
        return 3;
    }
    write_four(v, out);
    return 4;
}

inline int write_leading_eight(unsigned v, char* out) {
    if (v < 10000) return write_leading_four(v, out);
    const int n = write_leading_four(v / 10000, out);
    write_four(v % 10000, out + n);
    return n + 4;
}

} // namespace u64toa_detail

// Write 1..20 decimal digits. The caller supplies space and owns termination.
inline int u64toa(std::uint64_t v, char* out) {
    constexpr std::uint64_t base = 100000000;
    if (v < base)
        return u64toa_detail::write_leading_eight(static_cast<unsigned>(v), out);

    const std::uint64_t high = v / base;
    int n;
    if (high < base) {
        n = u64toa_detail::write_leading_eight(static_cast<unsigned>(high), out);
    } else {
        // UINT64_MAX / 10^16 is 1844, so the third group fits in four digits.
        n = u64toa_detail::write_leading_four(static_cast<unsigned>(high / base), out);
        u64toa_detail::write_eight(static_cast<unsigned>(high % base), out + n);
        n += 8;
    }
    u64toa_detail::write_eight(static_cast<unsigned>(v % base), out + n);
    return n + 8;
}

// Write tag=value<SOH> and return its byte count. At most 42 bytes.
inline int encode_u64_field(std::uint64_t tag, std::uint64_t value, char* out) {
    int n = u64toa(tag, out);
    out[n++] = '=';
    n += u64toa(value, out + n);
    out[n++] = '\x01';
    return n;
}
