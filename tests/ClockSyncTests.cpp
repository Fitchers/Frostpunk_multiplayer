#include "../src/ClockSync.h"
#include <cassert>
#include <iostream>

int main() {
    frostsync::ClockCorrection c;
    // Real observed 0/4.8/9.6 second frame jitter: zero correction pauses.
    for (int i = 0; i < 10000; ++i)
        assert(!c.update(100000 + (i % 3) * 4800, 100000, i * 25));
    c.reset();
    assert(!c.update(200000, 100000, 0));
    assert(!c.update(200000, 100000, 749));
    assert(c.update(200000, 100000, 750));
    assert(c.update(200000, 190000, 800));
    assert(!c.update(200000, 195000, 850));
    // Transient spikes and reversing leader do not accumulate debounce time.
    c.reset();
    assert(!c.update(200000, 100000, 0));
    assert(!c.update(100000, 200000, 700));
    assert(!c.update(200000, 100000, 800));
    assert(!c.update(200000, 100000, 1500));
    assert(c.update(200000, 100000, 1550));
    c.reset();
    assert(!c.update(200000, 100000, 0, true));
    assert(!c.update(200000, 100000, 5000, true));
    assert(!c.update(200000, 100000, 5100));
    assert(c.update(200000, 100000, 5850));
    // Equal and lagging cities must never be held by the correction itself.
    assert(!c.update(100000, 200000, 6000));
    assert(!c.update(200000, 200000, 7000));
    std::cout << "PASS: frame jitter, debounce, catchup, hysteresis and user pause.\n";
}
