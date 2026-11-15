#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <unordered_map>
// HW7 — a fast order book (flat, price-indexed) + a fast symbol->id map.
// side 'B'=bid, 'S'=ask.  SymMap::get returns (uint64_t)-1 if absent.
//
// Range warning (see labs/week07.md step 2): a flat array of N one-cent slots
// is a BAND, not "all prices". 1<<16 slots indexed absolutely from $0.00 covers
// only $0.00-$655.35, and the arena lists NFLX near $720 and META near $580 —
// an absolute index walks off the end and, because the two side arrays are
// adjacent members, silently corrupts the OTHER side of your own book. ASan
// cannot see that (it is an intra-object overflow) and these tests only use
// prices near $100, so CI will not catch it for you. Index against a base tick
// (slot = tick - base_tick_) and bounds-check both ends.
static constexpr double TICK = 0.01;
static constexpr int    NLEV = 1 << 16;   // 65536 contiguous slots = a band

static inline int px_to_tick(double px) {
    return static_cast<int>(px / TICK + 0.5);   // round to nearest tick
}

struct Book {
    std::array<uint32_t, NLEV> bid_qty{};   // qty resting at each bid SLOT
    std::array<uint32_t, NLEV> ask_qty{};   // qty resting at each ask SLOT
    int  base_tick_ = 0;                    // absolute tick of slot 0
    bool based_     = false;                // has the band been placed yet?
    int best_bid_slot = -1;                 // highest bid slot with qty>0
    int best_ask_slot = -1;                 // lowest  ask slot with qty>0
    struct Rec { char side; int slot; uint32_t qty; };
    std::unordered_map<uint64_t, Rec> orders_;   // fine: cancel is off the read path

    // absolute tick -> slot. The first price we ever see centres the band, so
    // we get ~$327 of headroom on either side of the market. std::max keeps
    // base_tick_ >= 0 for cheap stocks, where the band just starts at $0.00.
    int slot_of(int tick) {
        if (!based_) { base_tick_ = std::max(0, tick - NLEV / 2); based_ = true; }
        return tick - base_tick_;
    }
    // slot -> price, for the BBO reads
    double px_of(int slot) const { return (slot + base_tick_) * TICK; }
    


    void add(uint64_t id, char side, double px, uint32_t qty) { 
        int i = slot_of(px_to_tick(px));
        if (i < 0 || i >= NLEV) return;         // outside the band: refuse, don't
                                                // scribble (see 4.2 / Phase 2)
        orders_[id] = {side, i, qty};           // remember for cancel (see 4.6)
        if (side == 'B') {
            bid_qty[i] += qty;
            if (i > best_bid_slot) best_bid_slot = i;   // new inside price
        } else {
            ask_qty[i] += qty;
            if (best_ask_slot < 0 || i < best_ask_slot) best_ask_slot = i;
        }
    }   

    void cancel(uint64_t id) {
        auto it = orders_.find(id);
        if (it == orders_.end()) return;
        Rec r = it->second; orders_.erase(it);
        if (r.side == 'B') {
            bid_qty[r.slot] -= r.qty;
            if (r.slot == best_bid_slot && bid_qty[r.slot] == 0) {
                while (best_bid_slot >= 0 && bid_qty[best_bid_slot] == 0) --best_bid_slot;
            }
        } else {
            ask_qty[r.slot] -= r.qty;
            if (r.slot == best_ask_slot && ask_qty[r.slot] == 0) {
                while (best_ask_slot < NLEV && ask_qty[best_ask_slot] == 0) ++best_ask_slot;
                if (best_ask_slot >= NLEV) best_ask_slot = -1;   // side is empty
            }
        }
    }

    double best_bid() const {
        return best_bid_slot < 0 ? 0.0 : px_of(best_bid_slot);
    }
    double best_ask() const {
        return best_ask_slot < 0 ? 0.0 : px_of(best_ask_slot);
    }
};
struct SymMap {
    static constexpr int CAP = 1 << 12;           // 4,096 slots, power of two -> mask, not %
    struct Slot { char key[16]{}; uint64_t id = (uint64_t)-1; bool used = false; };
    std::array<Slot, CAP> t_{};
    int size_ = 0;                                // keys stored, for the load-factor guard

    static uint64_t hash(const char* s) {         // FNV-1a
        uint64_t h = 1469598103934665603ull;
        for (; *s; ++s) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
        return h;
    }
    static bool keq(const char* a, const char* b) {
        for (int i = 0; i < 16; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
        return true;
    }
    void put(const char* sym, uint64_t id) {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {                // linear probe
            if (t_[i].used && keq(t_[i].key, sym)) { t_[i].id = id; return; }  // overwrite
            if (!t_[i].used) {
                if (size_ * 10 >= CAP * 7) return;        // load-factor guard: refuse a NEW
                                                          // key past 70% full
                std::snprintf(t_[i].key, 16, "%s", sym);  // copy (cold path only)
                t_[i].id = id; t_[i].used = true; ++size_; return;
            }
        }
    }
    uint64_t get(const char* sym) const {
        int i = hash(sym) & (CAP - 1);
        for (;; i = (i + 1) & (CAP - 1)) {
            if (!t_[i].used)          return (uint64_t)-1; // empty slot => absent
            if (keq(t_[i].key, sym))  return t_[i].id;
        }
    }
};