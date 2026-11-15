#include <cstdio>
#include <new>
#include <string>
#include "pool.hpp"

static int live = 0;                       // counts constructed-but-not-destroyed

struct Strategy {                          // abstract interface
    Strategy()  { ++live; }
    virtual ~Strategy() { --live; }        // <- the line that matters
    virtual double on_book(double bid, double ask, double obi) = 0;
};

struct Momentum : Strategy {
    double on_book(double bid, double ask, double obi) override {
        return obi * 0.5 * (ask - bid);
    }
};

struct MeanRev : Strategy {
    std::string tag = "a-name-long-enough-to-force-a-heap-allocation-in-SSO-land";  // owns heap memory
    double ema = 100.0;
    double on_book(double bid, double ask, double) override {
        double mid = 0.5 * (bid + ask);
        ema += 0.1 * (mid - ema);
        return ema - mid;
    }
};

int main() {
    constexpr std::size_t SLOT = sizeof(MeanRev) > sizeof(Momentum) ? sizeof(MeanRev) : sizeof(Momentum);
    Pool pool(SLOT, 8);                    // one slot size that fits every derived type

    Strategy* s[2] = {
        new (pool.alloc()) Momentum{},
        new (pool.alloc()) MeanRev{},
    };
    for (Strategy* p : s)
        std::printf("signal = %+.4f\n", p->on_book(100.00, 100.02, 0.4));

    for (Strategy* p : s) {
        p->~Strategy();                    // virtual: runs ~Momentum / ~MeanRev, then ~Strategy
        pool.free(p);                      // slot goes back on the free-list
    }
    std::printf("live objects after teardown: %d\n", live);
}