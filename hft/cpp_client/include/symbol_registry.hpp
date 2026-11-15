#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>

namespace arena {

// Maps short symbol names to caller-assigned IDs using a fixed-size,
// open-addressed hash table. The table allocates no memory after construction,
// which makes lookups and inserts suitable for a hot path.
//
// Capacity is the number of hash-table slots, not the maximum number of
// symbols: insertion stops at about 70% occupancy to keep probe sequences
// bounded. MaxSymbolBytes is the largest accepted symbol length.
//
// This class is not thread-safe; its owner must serialize access. Entries
// cannot be deleted because find() treats an unused slot as the end of a
// probe chain. IDs are supplied by the caller and remain stable because this
// registry never removes or relocates entries.
template<std::size_t Capacity = 128, std::size_t MaxSymbolBytes = 16>
class SymbolRegistry {
public:
    // Sentinel returned by find() for an invalid or unregistered symbol.
    static constexpr std::size_t npos = std::numeric_limits<std::size_t>::max();

    // The table uses a bit mask instead of modulo, so Capacity must be a power
    // of two. At least two slots are required for an actual lookup table.
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(MaxSymbolBytes > 0);

    // Return the ID associated with symbol, or npos if the symbol is invalid
    // or absent. Linear probing checks consecutive slots, wrapping at the end.
    // Encountering an unused slot proves the symbol was never inserted:
    // entries are never erased, so probe chains cannot contain deletion gaps.
    std::size_t find(std::string_view symbol) const {
        if (symbol.empty() || symbol.size() > MaxSymbolBytes) return npos;

        // hash() selects the initial slot; Capacity - 1 extracts the low bits
        // because Capacity is a power of two.
        std::size_t index = hash(symbol) & (Capacity - 1);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            const auto& slot = slots_[index];
            if (!slot.used) return npos;
            if (matches(slot, symbol)) return slot.id;

            // A collision: inspect the next slot, wrapping to slot zero.
            index = (index + 1) & (Capacity - 1);
        }

        // The table is full or every slot was checked without a match.
        return npos;
    }

    // Associate symbol with the caller-provided ID. Returns false for invalid
    // input, a full/too-dense table, or a duplicate symbol given a different
    // ID. Re-inserting an existing symbol with its original ID succeeds.
    // Names are copied in full; they are never truncated.
    bool insert(std::string_view symbol, std::size_t id) {
        if (symbol.empty() || symbol.size() > MaxSymbolBytes || id == npos)
            return false;

        std::size_t index = hash(symbol) & (Capacity - 1);
        for (std::size_t probe = 0; probe < Capacity; ++probe) {
            auto& slot = slots_[index];
            if (slot.used) {
                // Do not change an existing symbol's ID: callers may use it
                // to index parallel state that has already been initialized.
                if (matches(slot, symbol)) return slot.id == id;
            } else {
                // Stop before the table gets too dense; this limits the
                // expected number of slots inspected during future lookups.
                if (size_ + 1 > Capacity * 7 / 10) return false;

                // Publish a complete key and ID before marking this slot used.
                // The class is single-owner, so no atomic publication is needed.
                std::memcpy(slot.key.data(), symbol.data(), symbol.size());
                slot.length = symbol.size();
                slot.id = id;
                slot.used = true;
                ++size_;
                return true;
            }

            // This slot belongs to a different symbol; continue the probe.
            index = (index + 1) & (Capacity - 1);
        }
        return false;
    }

private:
    // One hash-table bucket. key stores exactly length bytes; ID has meaning
    // outside this class (for example, an index into a strategy state array).
    struct Slot {
        std::array<char, MaxSymbolBytes> key{};
        std::size_t length = 0;
        std::size_t id = npos;
        bool used = false;
    };

    // 64-bit FNV-1a hash. It processes each symbol byte and returns a value
    // used to choose the initial bucket. Equality is still checked by matches(),
    // so hash collisions do not make different symbols compare equal.
    static std::size_t hash(std::string_view symbol) {
        std::uint64_t value = 14695981039346656037ull;
        for (char c : symbol) {
            value ^= static_cast<unsigned char>(c);
            value *= 1099511628211ull;
        }
        return static_cast<std::size_t>(value);
    }

    // Compare both length and bytes. Checking length first prevents a prefix
    // such as "ABC" from matching a longer symbol such as "ABCD".
    static bool matches(const Slot& slot, std::string_view symbol) {
        return slot.length == symbol.size() &&
            std::memcmp(slot.key.data(), symbol.data(), symbol.size()) == 0;
    }

    // Inline fixed-capacity storage keeps keys in place and avoids per-entry
    // allocation. size_ counts occupied slots for the insertion load limit.
    std::array<Slot, Capacity> slots_{};
    std::size_t size_ = 0;
};

} // namespace arena
