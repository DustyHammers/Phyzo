// MIDI clock from the host's tempo (F-68). The OS follows MIDI clock (24 F8 per quarter note): it plays at the clock's
// tempo and shows it on its Tempo display, panel tempo edits don't change the playing tempo while clock arrives, and
// when clock stops it keeps the last clock tempo. It ignores Start, Stop, Continue and Song Position, so only F8 is
// ever sent. The plugin feeds the ticks into the block's MIDI at their sample positions (no plugin framework here).
//
// Modes: Off (no clock; the block's events are untouched), WhilePlaying (clock only while the host transport plays;
// the default) and Always (whenever the host reports a tempo: free-running at its bpm while stopped).
// Ticks fall on the host's grid, at ppq = k / 24: tick k is at sample floor(x + 0.5) of the block, x = (k - 24 * ppq0)
// * samples-per-tick from the block's start. A tick that rounds to the first sample of the next block belongs to that
// block, so every block size gives the same positions for the same host timeline. If the position goes backwards
// (a loop) or jumps forwards by more than one block, the phase starts again from the new position (no catch-up burst;
// a first tick less than half a tick after the last one sent is left out, so a loop off the grid never sends two
// ticks close together).
#pragma once
#include <cstdint>

namespace midiclock {

enum class Mode : uint8_t { Off = 0, WhilePlaying = 1, Always = 2 };

// What the host's playhead reported for the block (no playhead: all false).
struct Transport {
    bool hasBpm = false;
    double bpm = 0;
    bool hasPpq = false;
    double ppq = 0;                  // quarter notes at the block's first sample
    bool playing = false;
};

inline constexpr uint8_t kClock = 0xF8;
inline const uint8_t kClockMessage[1] = {kClock};

class Generator {
public:
    static constexpr double kMaxBpm = 999.0;
    // Room for the ticks of a block of up to `blockSize` samples at up to kMaxBpm (plus the one carried in).
    static int maxTicks(double sampleRate, int blockSize);
    // The ticks of a block of n samples at sampleRate: their sample offsets (0..n-1, ascending) into ticks[0..max);
    // returns the count. Audio thread: no allocation, no lock.
    int process(Mode mode, const Transport& t, double sampleRate, int n, int* ticks, int max);
    void reset();

private:
    bool grid_ = false;              // the last block was on the host's grid (playing, with a position)
    double endPpq_ = 0;              // where it ended: the next block's start if the transport runs on
    int64_t nextTick_ = 0;           // the next grid tick to send (ppq = k / 24)
    bool free_ = false;              // a phase to go on from (after a grid block, or free-running)
    double freeAt_ = 0;              // the next tick, in samples from the next block's start
    double sinceLast_ = 1e18;        // samples from the last tick sent to the next block's start
};

// The block's MIDI with the clock: when mode is not Off, the host's own clock (F8) is taken out and the ticks are
// merged in, in sample order (a tick after the host's events at the same offset). Off: nothing changes. `Event` has
// int offset, const uint8_t* data, int size (Engine::MidiEvent). events must have room for count + numTicks.
// Returns the new count.
template <typename Event>
int merge(Mode mode, Event* events, int count, const int* ticks, int numTicks) {
    if (mode == Mode::Off) return count;
    int kept = 0;
    for (int i = 0; i < count; ++i)
        if (!(events[i].size == 1 && events[i].data[0] == kClock)) events[kept++] = events[i];
    int w = kept + numTicks - 1, i = kept - 1;
    for (int j = numTicks - 1; j >= 0; --j) {
        while (i >= 0 && events[i].offset > ticks[j]) events[w--] = events[i--];
        Event e = Event();
        e.offset = ticks[j]; e.data = kClockMessage; e.size = 1;
        events[w--] = e;
    }
    return kept + numTicks;
}

}  // namespace midiclock
