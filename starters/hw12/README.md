# HW12 — unsigned integer serialization

On a mixed workload spanning 1–20 decimal digits, `u64toa` took **10.58 CPU cycles/op** at **399.34 million conversions/s**. The digit-at-a-time implementation took 31.00 cycles/op, `std::to_string` took 44.12, and `snprintf` took 160.29.

## Implementation

The [header](../../include/u64toa.hpp) writes 1–20 digits into caller-owned storage and returns the byte count. It uses integer arithmetic and a static digit-pair table, with no allocation, locale lookup or trailing NUL. It splits a value into groups of eight decimal digits. The leading group determines the length. Each remaining group writes exactly eight digits, including internal zeros. For example, 100000001 splits into `1` and `00000001`. The bonus `encode_u64_field(tag, value, out)` uses the same function for both numbers and appends a single SOH byte after the value. A 42-byte buffer accommodates two maximum-width integers, the equals sign and SOH.

## Measurements

Each cell gives **CPU cycles/op / million conversions per second**. One operation formats one pre-generated integer. The mixed row gives each digit length from 1 through 20 equal probability.

| Input digits | Before · digit loop | Pair loop | After · u64toa | snprintf | std::to_string |
|---|---:|---:|---:|---:|---:|
| 1 (0–9) | 5.01 / 769.63 | 6.01 / 663.66 | 2.01 / 1881.42 | 100.97 / 38.98 | 13.01 / 306.06 |
| 4 | 11.01 / 396.39 | 9.01 / 487.21 | 4.01 / 1098.11 | 105.48 / 40.11 | 18.02 / 234.83 |
| 10 | 17.39 / 233.95 | 11.07 / 383.63 | 6.00 / 678.35 | 109.89 / 38.07 | 36.53 / 117.69 |
| 16 | 31.14 / 136.96 | 15.09 / 272.23 | 9.36 / 439.47 | 119.81 / 34.75 | 39.01 / 107.37 |
| 20 | 44.86 / 95.80 | 21.88 / 191.18 | 13.05 / 321.50 | 128.03 / 32.46 | 45.06 / 93.45 |
| Mixed | 31.00 / 137.05 | 19.79 / 215.05 | **10.58 / 399.34** | 160.29 / 26.39 | 44.12 / 98.35 |

**The largest gain on mixed inputs came from extracting two digits per iteration.** The first two versions fill the same scratch buffer backwards and copy the result once. Changing the divisor from 10 to 100 and looking up each remainder as a pair of characters roughly halves the chain of dependent quotients on long inputs. That change saved **11.21 cycles/op** on the mixed workload and **22.98** on 20-digit inputs. It cost **1.00 cycle/op** on single digits, where there is no pair to extract. The final version writes single digits directly and emits eight-digit groups straight into the caller's buffer, saving a further **9.21 cycles/op** on mixed inputs.

Measured on Apple M4, macOS 15.2 arm64, Apple Clang 16.0.0, C++17 with `-O3 -DNDEBUG`. All five methods process the same 8,192 values per workload, 24 passes per sample. After one warm-up sample per method, the benchmark takes 15 samples and rotates the method order. A compiler barrier makes the output pointer, length and bytes observable on every call. The single-threaded process reads CPU cycles from `proc_pid_rusage` (field `ri_cycles`) and wall time from `steady_clock`. The table reports median cycles and throughput derived from median wall time. On x86 the executable reports fenced `__rdtsc()` reference cycles. Other supported targets label the fallback as timer ticks.

## Reproduce

Run from the repository root.

```sh
make u64toa
c++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wpedantic -Iinclude -Itests starters/hw12/u64toa_bench.cpp -o /tmp/hw12_u64toa_bench
/tmp/hw12_u64toa_bench
```

The course driver passed its edge cases and all 200,000 comparisons with `std::to_string`. Its separate fixed-input test reported 3.61 ns/op for `u64toa` and 10.55 ns/op for `std::to_string`. The [benchmark](u64toa_bench.cpp) also checks every value through 9999, the neighbors of each representable power of ten, `UINT64_MAX`, every benchmark input, and exact FIX field bytes. Guard bytes check that output ends at the returned length. These checks passed under AddressSanitizer and UndefinedBehaviorSanitizer and can be run separately with `--check`.
