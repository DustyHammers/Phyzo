// MIDI clock test (F-68; no ROM data): the host-tempo clock generator and the merge into a block's MIDI.
//  1. 120 bpm at 48 kHz from ppq 0: 48 ticks a second, 1000 samples apart, the first at offset 0.
//  2. Non-integer tempos at 44.1 and 48 kHz: the same tick positions for block sizes 1, 7, 64, 441, 512, 1024, 4096
//     and a mixed sequence, each tick at floor(k * samples-per-tick + 0.5) (none lost, none twice).
//  3. Loop wraps (mid-block and on a block boundary) and jumps: the phase starts again; no burst, no lost tick.
//  4. Transport stopped: While playing sends nothing; Always free-runs at the bpm, going on from the last tick; no
//     tempo (no playhead, no bpm): nothing in any mode.
//  5. Off: nothing, and the block's events are untouched (the host's own F8 included). Not Off: the host's F8 is taken
//     out and the ticks are merged in sample order, after the host's events at the same offset.
//  6. A tempo change, and the tick capacity (never more than it has room for).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>
#include "midi_clock.h"

using midiclock::Generator;
using midiclock::Mode;
using midiclock::Transport;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Event { int offset; const uint8_t* data; int size; };

// A host timeline: playing from sample 0 at ppq 0 (or a loop of `loopSamples`), blocks of the given sizes in turn.
// The host reports the ppq at each block's first sample as it would: samples * bpm / (60 * rate).
struct Host {
    double rate = 48000, bpm = 120;
    bool playing = true, hasBpm = true, hasPpq = true;
    int64_t loopSamples = 0;                         // 0: no loop
    double startPpq = 0;
};

// Absolute sample positions of the ticks over `total` samples.
std::vector<int64_t> run(Mode mode, const Host& h, int64_t total, const std::vector<int>& sizes, Generator* g = nullptr) {
    Generator local;
    Generator& gen = g ? *g : local;
    std::vector<int64_t> out;
    std::vector<int> ticks(size_t(Generator::maxTicks(h.rate, 4096)));
    int64_t s = 0;
    for (size_t b = 0; s < total; ++b) {
        const int n = int(std::min<int64_t>(sizes[b % sizes.size()], total - s));
        const int64_t inLoop = h.loopSamples ? s % h.loopSamples : s;
        Transport t;
        t.hasBpm = h.hasBpm; t.bpm = h.bpm;
        t.hasPpq = h.hasPpq; t.ppq = h.startPpq + double(inLoop) * h.bpm / (60.0 * h.rate);
        t.playing = h.playing;
        const int c = gen.process(mode, t, h.rate, n, ticks.data(), int(ticks.size()));
        for (int i = 0; i < c; ++i) {
            CHECK(ticks[size_t(i)] >= 0 && ticks[size_t(i)] < n);
            if (i) CHECK(ticks[size_t(i)] > ticks[size_t(i) - 1]);
            out.push_back(s + ticks[size_t(i)]);
        }
        s += n;
    }
    return out;
}

// The ideal positions: tick k at floor(k * samples-per-tick + 0.5), below `total`.
std::vector<int64_t> ideal(double rate, double bpm, int64_t total) {
    const double spt = rate * 60.0 / bpm / 24.0;
    std::vector<int64_t> v;
    for (int64_t k = 0;; ++k) {
        const int64_t p = int64_t(std::floor(double(k) * spt + 0.5));
        if (p >= total) break;
        v.push_back(p);
    }
    return v;
}

// Every gap between ticks within [lo, hi] samples.
bool gapsWithin(const std::vector<int64_t>& v, double lo, double hi) {
    for (size_t i = 1; i < v.size(); ++i) {
        const double d = double(v[i] - v[i - 1]);
        if (d < lo || d > hi) { std::printf("  gap %.0f at %lld (tick %zu)\n", d, (long long)v[i], i); return false; }
    }
    return true;
}
}  // namespace

int main() {
    const std::vector<int> sizes = {1, 7, 64, 441, 512, 1024, 4096};

    // 1. 120 bpm, 48 kHz: one tick every 1000 samples from offset 0.
    {
        Host h;
        const std::vector<int64_t> v = run(Mode::WhilePlaying, h, 48000, {512});
        CHECK(v.size() == 48);
        for (size_t i = 0; i < v.size(); ++i) CHECK(v[i] == int64_t(i) * 1000);
        Generator g;
        int ticks[8];
        Transport t; t.hasBpm = true; t.bpm = 120; t.hasPpq = true; t.ppq = 0; t.playing = true;
        CHECK(g.process(Mode::WhilePlaying, t, 48000, 512, ticks, 8) == 1 && ticks[0] == 0);
    }

    // 2. Non-integer tempos: every block size gives the ideal positions.
    {
        int compared = 0;
        for (double rate : {44100.0, 48000.0})
            for (double bpm : {123.4, 97.31, 171.0}) {
                Host h; h.rate = rate; h.bpm = bpm;
                const int64_t total = int64_t(rate * 6);
                const std::vector<int64_t> want = ideal(rate, bpm, total);
                for (int n : sizes) { CHECK(run(Mode::WhilePlaying, h, total, {n}) == want); ++compared; }
                CHECK(run(Mode::WhilePlaying, h, total, {1, 4096, 7, 441, 2, 1024, 64, 3, 512}) == want);
                CHECK(run(Mode::Always, h, total, {333}) == want);   // playing: Always is on the grid too
                // starting mid-song (ppq 37.3): the same grid, shifted
                Host m = h; m.startPpq = 37.3;
                const std::vector<int64_t> a = run(Mode::WhilePlaying, m, total, {1}), b = run(Mode::WhilePlaying, m, total, {4096});
                CHECK(a == b && !a.empty());
            }
        std::printf("midi_clock_test: %d block-size runs at non-integer tempos give the same tick positions\n", compared);
    }

    // 3. Loops and jumps: the phase starts again from the new position, no burst, no lost tick.
    {
        Host h; h.loopSamples = 96000;               // 4 quarters at 120 bpm: the loop end is on the grid
        for (int n : {512, 500, 441, 4096, 1}) {     // 500: the wrap on a block boundary; others: inside a block
            const std::vector<int64_t> v = run(Mode::WhilePlaying, h, 96000 * 3 + 5000, {n});
            CHECK(gapsWithin(v, 1000, 1000));
            CHECK(v.size() == 3 * 96 + 5);
        }
        // a loop whose end is off the grid: the restart is on the grid at the loop's start; never two ticks within
        // half a tick (no burst, no double), never a gap of more than two ticks
        for (int64_t extra : {300, 700, 999, 1}) {
            Host o; o.loopSamples = 96000 + extra;
            for (int n : sizes) CHECK(gapsWithin(run(Mode::WhilePlaying, o, 400000, {n}), 500, 2000));
        }
        // jumps: forwards by more than a block, and backwards
        Generator g;
        int ticks[64];
        Transport t; t.hasBpm = true; t.bpm = 120; t.hasPpq = true; t.playing = true;
        t.ppq = 0; g.process(Mode::WhilePlaying, t, 48000, 512, ticks, 64);
        t.ppq = 512.0 / 24000.0; g.process(Mode::WhilePlaying, t, 48000, 512, ticks, 64);
        t.ppq = 10.0 + 100.0 / 24000.0;              // jump ahead: the next tick is the grid's (ppq 10 + 1/24, at 900)
        int c = g.process(Mode::WhilePlaying, t, 48000, 4096, ticks, 64);
        CHECK(c == 4 && ticks[0] == 900 && ticks[1] == 1900 && ticks[3] == 3900);
        t.ppq = 2.0;                                 // back: the grid from there, but the tick at 0 would come only 196
        c = g.process(Mode::WhilePlaying, t, 48000, 4096, ticks, 64);   // samples after the last one: left out
        CHECK(c == 4 && ticks[0] == 1000 && ticks[3] == 4000);
        t.playing = false;                           // stopped for a block, then playing from ppq 7: the last tick
        CHECK(g.process(Mode::WhilePlaying, t, 48000, 4096, ticks, 64) == 0);   // is long past, so the first is at 0
        t.playing = true; t.ppq = 7.0;
        c = g.process(Mode::WhilePlaying, t, 48000, 4096, ticks, 64);
        CHECK(c == 5 && ticks[0] == 0 && ticks[4] == 4000);
        std::printf("midi_clock_test: loop wraps and jumps restart the phase without bursts or lost ticks\n");
    }

    // 4. Stopped transport, no tempo.
    {
        Host h; h.playing = false;
        CHECK(run(Mode::WhilePlaying, h, 96000, {512}).empty());
        const std::vector<int64_t> free = run(Mode::Always, h, 96000, {512});
        CHECK(free.size() == 96 && free.front() == 0 && gapsWithin(free, 1000, 1000));
        // playing, then stopped: While playing stops; Always goes on from the last tick at the bpm
        for (Mode mode : {Mode::WhilePlaying, Mode::Always}) {
            Generator g;
            Host p; p.bpm = 123.4;
            const std::vector<int64_t> a = run(mode, p, 48000, {512}, &g);
            p.playing = false;
            std::vector<int64_t> b = run(mode, p, 48000, {512}, &g);
            for (int64_t& x : b) x += 48000;
            if (mode == Mode::WhilePlaying) CHECK(b.empty());
            else {
                const double spt = 48000.0 * 60.0 / 123.4 / 24.0;
                CHECK(!b.empty() && std::fabs(double(b.front() - a.back()) - spt) <= 1.0);
                CHECK(gapsWithin(b, std::floor(spt), std::ceil(spt)));
            }
        }
        for (Mode mode : {Mode::Off, Mode::WhilePlaying, Mode::Always}) {
            Host nb; nb.hasBpm = false;
            CHECK(run(mode, nb, 48000, {512}).empty());
            nb.hasPpq = false; nb.playing = false;   // no playhead at all
            CHECK(run(mode, nb, 48000, {512}).empty());
            Host zero; zero.bpm = 0;
            CHECK(run(mode, zero, 48000, {512}).empty());
        }
        // playing without a position: free-running at the bpm
        Host np; np.hasPpq = false;
        const std::vector<int64_t> v = run(Mode::WhilePlaying, np, 48000, {512});
        CHECK(v.size() == 48 && gapsWithin(v, 1000, 1000));
        std::printf("midi_clock_test: stopped / no tempo as specified\n");
    }

    // 5. Off, and the merge.
    {
        Host h;
        CHECK(run(Mode::Off, h, 96000, {512}).empty());
        const uint8_t note[3] = {0x90, 60, 100}, cc[3] = {0xB0, 7, 99}, f8[1] = {0xF8}, fa[1] = {0xFA};
        const std::vector<Event> host = {{0, note, 3}, {10, f8, 1}, {100, note, 3}, {100, fa, 1}, {500, cc, 3}};
        const int ticks[3] = {0, 100, 300};
        std::vector<Event> ev(16);
        std::copy(host.begin(), host.end(), ev.begin());
        CHECK(midiclock::merge(Mode::Off, ev.data(), int(host.size()), ticks, 0) == int(host.size()));
        for (size_t i = 0; i < host.size(); ++i) CHECK(ev[i].offset == host[i].offset && ev[i].data == host[i].data);
        const int c = midiclock::merge(Mode::WhilePlaying, ev.data(), int(host.size()), ticks, 3);
        const std::vector<std::pair<int, const uint8_t*>> want = {{0, note}, {0, midiclock::kClockMessage}, {100, note}, {100, fa},
                                                                  {100, midiclock::kClockMessage}, {300, midiclock::kClockMessage}, {500, cc}};
        CHECK(c == int(want.size()));
        for (int i = 0; i < c && i < int(want.size()); ++i) {
            CHECK(ev[size_t(i)].offset == want[size_t(i)].first && ev[size_t(i)].data == want[size_t(i)].second);
            CHECK(ev[size_t(i)].size == (ev[size_t(i)].data == note || ev[size_t(i)].data == cc ? 3 : 1));
        }
        std::copy(host.begin(), host.end(), ev.begin());
        CHECK(midiclock::merge(Mode::Always, ev.data(), int(host.size()), ticks, 0) == 4);   // host F8 out, no ticks
        CHECK(ev[1].offset == 100 && ev[1].data == note);
    }

    // 6. A tempo change, and the capacity.
    {
        Generator g;
        int ticks[64];
        Transport t; t.hasBpm = true; t.bpm = 120; t.hasPpq = true; t.ppq = 0; t.playing = true;
        std::vector<int64_t> v;
        int64_t s = 0;
        for (int b = 0; b < 200; ++b) {
            if (b == 100) t.bpm = 150;               // 800 samples a tick from here
            const int c = g.process(Mode::WhilePlaying, t, 48000, 480, ticks, 64);
            for (int i = 0; i < c; ++i) v.push_back(s + ticks[i]);
            t.ppq += 480.0 * t.bpm / (60.0 * 48000.0);
            s += 480;
        }
        CHECK(gapsWithin(v, 800, 1000));
        CHECK(Generator::maxTicks(192000, 4096) >= 3 && Generator::maxTicks(44100, 4096) >= 38);
        t.bpm = 999; t.ppq = 0;
        Generator h;
        CHECK(h.process(Mode::WhilePlaying, t, 44100, 4096, ticks, 5) == 5);   // never more than there is room for
        CHECK(h.process(Mode::WhilePlaying, t, 44100, 4096, ticks, Generator::maxTicks(44100, 4096)) <= Generator::maxTicks(44100, 4096));
    }

    std::printf("midi_clock_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
