#include "midi_clock.h"
#include <algorithm>
#include <cmath>

namespace midiclock {

int Generator::maxTicks(double sampleRate, int blockSize) {
    if (sampleRate <= 0 || blockSize <= 0) return 2;
    return int(std::ceil(double(blockSize) * kMaxBpm * 24.0 / 60.0 / sampleRate)) + 2;
}

void Generator::reset() {
    grid_ = free_ = false;
    endPpq_ = freeAt_ = 0;
    nextTick_ = 0;
    sinceLast_ = 1e18;
}

int Generator::process(Mode mode, const Transport& t, double sampleRate, int n, int* ticks, int max) {
    if (n <= 0) return 0;
    if (mode == Mode::Off || !t.hasBpm || !std::isfinite(t.bpm) || t.bpm <= 0 || !(sampleRate > 0)) {
        reset();                                     // no clock without a tempo
        return 0;
    }
    const double spq = sampleRate * 60.0 / t.bpm;    // samples per quarter note
    const double spt = spq / 24.0;                   // samples per tick
    int count = 0;
    int last = -1;                                   // the last tick's offset in this block
    auto emit = [&](int64_t s) {
        last = int(std::max<int64_t>(0, s));
        if (count < max) ticks[count++] = last;
    };
    auto done = [&] {
        sinceLast_ = last >= 0 ? double(n - last) : sinceLast_ + double(n);
        return count;
    };
    if (t.playing && t.hasPpq && std::isfinite(t.ppq)) {
        const double q = t.ppq * 24.0;               // ticks at the block's start
        auto pos = [&](int64_t k) { return int64_t(std::floor((double(k) - q) * spt + 0.5)); };
        int64_t first = int64_t(std::ceil(q - 0.5 / spt));   // the first tick at or after the block's first sample
        while (pos(first) < 0) ++first;
        while (pos(first - 1) >= 0) --first;
        // The host ran on from the last block (not back by more than a sample, not ahead by more than a block): go on
        // with its next tick (one left for this block that rounds just before it goes at 0; one already sent is not
        // sent again). Otherwise (a loop, a jump, a start) the phase starts again here, with no catch-up burst.
        const bool goesOn = grid_ && t.ppq >= endPpq_ - 1.0 / spq && t.ppq <= endPpq_ + double(n) / spq;
        int64_t k = first;
        if (goesOn && nextTick_ >= first - 1) k = nextTick_;
        else while (double(pos(k)) + sinceLast_ < spt / 2) ++k;   // a new phase: never right behind the last tick
        for (;; ++k) {
            const int64_t s = pos(k);
            if (s >= n) break;
            emit(s);
        }
        nextTick_ = k;
        grid_ = true;
        endPpq_ = t.ppq + double(n) / spq;
        free_ = true;                                // if the host stops (Always): go on from the next tick
        freeAt_ = (double(k) - q) * spt - double(n);
        return done();
    }
    grid_ = false;
    if (mode == Mode::Always || t.playing) {         // free-running at the bpm: stopped (Always), or no position
        if (!free_) { free_ = true; freeAt_ = std::max(0.0, spt - sinceLast_); }   // starting: one tick after the last
        for (;;) {
            const int64_t s = int64_t(std::floor(freeAt_ + 0.5));
            if (s >= n) break;
            emit(s);
            freeAt_ += spt;
        }
        freeAt_ -= double(n);
        return done();
    }
    free_ = false;                                   // While playing, stopped: no clock (the OS keeps the last tempo)
    return done();
}

}  // namespace midiclock
