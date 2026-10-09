// MIDI learn (F-55, docs/MIDI_LEARN.md; no plugin framework here). A controller is a second front panel: a learned CC
// (or note, for a button) produces exactly what the panel produces for that control, through the same calls as the
// mouse (Engine::setControl, Engine::pressRaw). Learning changes only how a control is driven, never what it does:
// no synth parameters, no host parameters; with no bindings, MIDI passes exactly as before.
//
// Threads. The bindings live on the message side (under a mutex the audio thread never takes) and are mirrored into
// fixed tables of atomics, (Any + 16 channels) x 128 for CCs and for notes, each holding a control or nothing. The
// audio thread reads only atomics and its own arrays: no lock, no allocation. It reports what it heard (a learned
// message, a one-edge button) through a lock-free single-producer queue that the message thread drains.
#pragma once
#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace midilearn {

// A learnable control, as one int: a knob is its analog control number (cc 0-22, docs/PANEL_CONTROLS.md), a button
// 0x100 | its panel id (raw 0-0x7F). The pitch wheel, mod wheel and pressure (cc 23-25) and the keys are never
// learnable: they stay plain MIDI, as on the hardware.
constexpr int kNone = -1;
constexpr int kKnobs = 23;
constexpr int knob(int cc) { return cc; }
constexpr int button(int raw) { return 0x100 | raw; }
constexpr bool isKnob(int c) { return c >= 0 && c < kKnobs; }
constexpr bool isButton(int c) { return c >= 0x100 && c <= 0x17F; }
constexpr bool learnable(int c) { return isKnob(c) || isButton(c); }
constexpr int knobCc(int c) { return isKnob(c) ? c : -1; }
constexpr int buttonRaw(int c) { return isButton(c) ? c & 0x7F : -1; }
constexpr int kMaxCc = 119;                  // CC 120-127 (channel mode messages): never offered or learned
constexpr double kOneEdgeMs = 2000;          // a learned button's first press with no release within this: a warning

// The perf knobs (System Controller 3, CC 71, CC 74, CC 79, System Controller 4) by their analog control numbers.
constexpr int kPerfCc[5] = {9, 13, 6, 5, 3};

struct Binding {
    int control = kNone;
    int channel = 0;                         // 0 = Any, 1-16
    bool note = false;                       // a note (buttons only) instead of a CC
    int number = 0;                          // CC 0-119 or note 0-127
    bool fine = false;                       // knobs: the 14-bit LSB (CC number + 32) has been seen
    bool operator==(const Binding& o) const {
        return control == o.control && channel == o.channel && note == o.note && number == o.number && fine == o.fine;
    }
};

// What the audio thread reports to the message thread.
struct Event {
    enum Kind : uint8_t { Learned, OneEdge } kind = Learned;
    int control = kNone;
    int channel = 0;                         // Learned: the channel heard (1-16)
    bool note = false;
    int number = 0;
};

// The panel the learned messages drive. The plugin: Engine::setControl and Engine::pressRaw (lock-free, any thread).
class Panel {
public:
    virtual ~Panel() = default;
    virtual void control(int cc, int raw) = 0;
    virtual void button(int raw, bool down) = 0;
};

// Knobs are absolute, like the real pots.
inline int raw7(int v) { return (std::min(std::max(v, 0), 127) * 1023 * 2 + 127) / (2 * 127); }       // round(v x 1023 / 127)
inline int raw14(int v) { return (std::min(std::max(v, 0), 16383) * 1023 * 2 + 16383) / (2 * 16383); } // round(v x 1023 / 16383)

class Learn {
public:
    explicit Learn(Panel& panel);

    // ---------------------------------------------------------------- message thread (any thread but the audio one)
    // Learn this control from the next eligible message on the host MIDI input (a knob: a CC 0-119; a button: a CC
    // 0-119 or a note on/off), on any channel. kNone (or an unlearnable control) cancels.
    void arm(int control);
    int armed() const { return armed_.load(); }
    // Sets a binding. One binding per control, and one control per (channel, CC) or (channel, note); a binding on Any
    // takes its CC (or note) on every channel and replaces that number's single-channel bindings, and the other way
    // round. Controls that lose their binding go to `displaced`. Refused (false): an unlearnable control, a note for
    // a knob, CC 120-127, a channel outside 0-16. A held button whose binding goes or changes is released first.
    // `watch`: check the button's first press for a release (the one-edge warning), as after learning it.
    bool bind(const Binding& b, std::vector<int>* displaced = nullptr, bool watch = true);
    bool forget(int control);                // false if it had no binding (a held button is released first)
    void forgetAll();
    void setBindings(const std::vector<Binding>& list);   // replaces every binding (a project, the defaults)
    std::vector<Binding> bindings() const;   // in control order, with the 14-bit flags as the audio thread saw them
    bool bindingOf(int control, Binding& out) const;
    int controlAt(int channel, bool note, int number) const;   // who holds this source exactly (channel 0 = Any)
    // What the audio thread heard: a learned message (commit it with commit()), a one-edge button.
    bool takeEvent(Event& e);
    // Commits a learned message: the binding on the channel heard (moving it from another control if needed).
    bool commit(const Event& e, std::vector<int>* displaced = nullptr);
    // Releases buttons the learned messages hold down (control kNone: all of them; orphans: only buttons with no
    // binding left). Each release goes through the panel once.
    void releaseHeld(int control = kNone);
    void releaseOrphans();
    bool held(int control) const;            // a learned button is down now
    // The skin: a learned knob moved (the visible knob shows it); taken once. serial() changes with every move.
    bool takeKnobMove(int cc, int& raw);
    uint32_t knobMoveSerial() const { return moveSerial_.load(std::memory_order_relaxed); }

    // ---------------------------------------------------------------- audio thread
    // Removes from `events` (in place) every message a binding takes and the message that completes learn, drives
    // the panel with them, and returns how many events are left (in their order, untouched). Unbound messages pass
    // exactly as they came. nowMs: a steady clock (for the one-edge warning). E has `data` and `size`.
    template <class E> int process(E* events, int n, double nowMs) {
        if (!active_.load(std::memory_order_acquire)) return n;
        int out = 0;
        for (int i = 0; i < n; ++i) {
            if (handle(events[i].data, events[i].size, nowMs)) continue;
            if (out != i) events[out] = events[i];
            ++out;
        }
        tick(nowMs);
        return out;
    }
    bool handle(const uint8_t* d, int size, double nowMs);   // true: taken (swallowed)
    void tick(double nowMs);                                 // the one-edge timeout

    // Text forms (docs/MIDI_LEARN.md): the settings' default bindings ("k9:1:c21:f b0B:0:n60" ...). Invalid entries
    // are skipped.
    static std::string toText(const std::vector<Binding>& list);
    static std::vector<Binding> fromText(const std::string& text);

private:
    static constexpr int kChannels = 17;     // Any + 16
    int16_t lookup(const std::array<std::atomic<int16_t>, kChannels * 128>& t, int ch, int num) const {
        const int16_t c = t[size_t(ch * 128 + num)].load(std::memory_order_relaxed);
        return c != kNone ? c : t[size_t(num)].load(std::memory_order_relaxed);
    }
    void learned(int control, int ch, bool note, int num);
    void knobValue(int cc, int value);       // MSB (or 7-bit)
    void knobLsb(int cc, int lsb);
    void edge(int raw, bool down, double nowMs);
    void moved(int cc, int raw);
    void push(const Event& e);
    void rebuild();                          // the tables from list_ (listMtx_ held)
    bool bindLocked(const Binding& b, std::vector<int>* displaced, bool watch);
    void releaseLocked(int control);
    void updateActive();

    Panel& panel_;
    mutable std::mutex listMtx_;             // the message side's list (never taken by the audio thread)
    std::vector<Binding> list_;
    std::atomic<int> armed_{kNone};
    std::atomic<bool> active_{false};        // any binding or learn armed (else process() is a no-op)
    std::array<std::atomic<int16_t>, kChannels * 128> cc_, note_;
    // A learned message, until the message thread commits it: messages from the same source act on that control.
    std::atomic<int32_t> provisional_{-1};   // control << 16 | channel << 8 | note << 7 | number
    // knobs
    std::array<std::atomic<int16_t>, kKnobs> msb_;            // the last MSB (-1: none yet)
    std::array<std::atomic<bool>, kKnobs> fine_;
    std::array<std::atomic<uint16_t>, kKnobs> moved_;         // 0x8000 | raw: a learned move for the skin
    std::atomic<uint32_t> moveSerial_{0};
    // buttons, by raw
    std::array<std::atomic<bool>, 128> down_, watch_;
    std::array<double, 128> pressedAt_{};     // audio thread
    // audio -> message: single producer (the audio thread), single consumer
    static constexpr uint32_t kQueue = 64;
    std::array<Event, kQueue> queue_{};
    std::atomic<uint32_t> qHead_{0}, qTail_{0};
};

// ---------------------------------------------------------------- names and texts (menus, messages)
// Perf knob labels come from the skin (its knob letters); empty: "Perf 1" ... "Perf 5".
using PerfLabels = std::array<std::string, 5>;
std::string controlName(int control, const PerfLabels& labels);   // "X (Perf 1)" (X: the skin's letter), "Cutoff", "Save", "Button 0x2A"
std::string shortName(int control, const PerfLabels& labels);     // upper case: "X", "PERF 1", "CUTOFF", "SAVE"
std::string sourceText(const Binding& b);                         // "CC 21, ch 1", "CC 21 (14-bit), any channel", "note 60, ch 2"
std::string header(int control, const Binding* b, const PerfLabels& labels);   // "X (Perf 1) · CC 21, ch 1" / "· not learned"
std::string listEntry(const Binding& b, const PerfLabels& labels); // "X (Perf 1)  CC 21 ch 1"
// What the OS itself does with a CC on its own (factory MIDI settings; System Controllers 3 and 4 at their defaults),
// empty if nothing: "OS: mod wheel", "OS: X knob, SYS3" ...
std::string osUse(int cc, const PerfLabels& labels);
// Messages (upper case, at most 50 columns: the editor's row 4).
constexpr size_t kMessageCols = 50;
std::string msgLearn(int control, const PerfLabels& labels);       // "LEARN X  MOVE A CONTROL..."
std::string msgBound(const Binding& b, const PerfLabels& labels);  // "X = CC 21 CH 1"
std::string msgMoved(const Binding& b, const std::vector<int>& from, const PerfLabels& labels);   // "CC 22 MOVED  CUTOFF -> X"
std::string msgForgotten(const Binding& b, const PerfLabels& labels);   // "X: CC 21 FORGOTTEN"
std::string msgOneEdge(const Binding& b, const PerfLabels& labels);     // "SAVE = CC 80  ONE EDGE ONLY: SET BUTTON MOMENTARY"

}  // namespace midilearn
