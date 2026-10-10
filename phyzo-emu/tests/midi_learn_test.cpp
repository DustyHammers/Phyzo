// MIDI learn test (F-55, docs/MIDI_LEARN.md; no ROM data). The learn core against a recording panel: value mapping
// (7-bit, 14-bit pairs), what is swallowed and what passes byte-identically, buttons with both edges and the
// one-edge warning, the binding rules (learn, channels, moving, refusals), the text form, and the audio thread's bound
// path without a lock or an allocation. Then the same against a real Engine running a small program of our own: a
// learned knob sends the OS the same panel stream as Engine::setControl, a learned button the same as pressRaw.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <thread>
#include <vector>
#include "engine.h"
#include "machine.h"
#include "midi_learn.h"

// Allocation counter for the audio thread's bound path (this test's own operator new).
static thread_local bool g_countAllocs = false;
static std::atomic<int> g_allocs{0};
void* operator new(std::size_t n) {
    if (g_countAllocs) ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace fs = std::filesystem;
using namespace midilearn;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Recorder : Panel {
    struct Call { bool knob; int id, value; };      // knob: cc, raw; button: raw, down
    std::vector<Call> calls;
    void control(int cc, int raw) override { calls.push_back({true, cc, raw}); }
    void button(int raw, bool down) override { calls.push_back({false, raw, down ? 1 : 0}); }
};

using Msg = std::vector<uint8_t>;
struct Ev { int offset; const uint8_t* data; int size; };
// Runs messages through process(); returns the ones left (as bytes).
std::vector<Msg> run(Learn& l, const std::vector<Msg>& in, double nowMs = 0) {
    std::vector<Ev> ev;
    for (size_t i = 0; i < in.size(); ++i) ev.push_back({int(i), in[i].data(), int(in[i].size())});
    const int n = l.process(ev.data(), int(ev.size()), nowMs);
    std::vector<Msg> out;
    for (int i = 0; i < n; ++i) out.emplace_back(ev[size_t(i)].data, ev[size_t(i)].data + ev[size_t(i)].size);
    return out;
}
Msg cc(int ch, int num, int val) { return {uint8_t(0xB0 | (ch - 1)), uint8_t(num), uint8_t(val)}; }
Msg noteOn(int ch, int num, int vel) { return {uint8_t(0x90 | (ch - 1)), uint8_t(num), uint8_t(vel)}; }
Msg noteOff(int ch, int num) { return {uint8_t(0x80 | (ch - 1)), uint8_t(num), 0x40}; }
const PerfLabels kNoLabels{};

void testMapping() {
    CHECK(raw7(0) == 0 && raw7(64) == 516 && raw7(127) == 1023);
    CHECK(raw14(0) == 0 && raw14(8192) == 512 && raw14(16383) == 1023);
    for (int v = 0; v < 128; ++v) CHECK(raw7(v) == int(v * 1023.0 / 127.0 + 0.5));
    for (int v = 0; v < 16384; v += 7) CHECK(raw14(v) == int(v * 1023.0 / 16383.0 + 0.5));
    // A knob on CC 21: the MSB alone is 7-bit; once an LSB (CC 53) is seen, MSB = coarse (LSB 0), LSB refines it.
    Recorder p;
    Learn l(p);
    CHECK(l.bind({knob(9), 1, false, 21, false}));
    CHECK(run(l, {cc(1, 21, 64)}).empty());
    CHECK(p.calls.size() == 1 && p.calls[0].knob && p.calls[0].id == 9 && p.calls[0].value == 516);
    Binding b;
    CHECK(l.bindingOf(knob(9), b) && !b.fine && sourceText(b) == "CC 21, ch 1");
    CHECK(run(l, {cc(1, 53, 0)}).empty());                                  // the LSB: 64 << 7 | 0 = 8192
    CHECK(p.calls.size() == 2 && p.calls[1].value == 512);
    CHECK(l.bindingOf(knob(9), b) && b.fine && sourceText(b) == "CC 21 (14-bit), ch 1");
    CHECK(run(l, {cc(1, 21, 127), cc(1, 53, 127)}).empty());
    CHECK(p.calls.size() == 4 && p.calls[2].value == raw14(127 << 7) && p.calls[3].value == 1023);
    CHECK(run(l, {cc(1, 21, 0)}).empty() && p.calls.back().value == 0);
    // An LSB with no MSB seen yet only marks the pair (nothing to refine).
    Recorder q;
    Learn m(q);
    m.bind({knob(13), 0, false, 2, false});
    CHECK(run(m, {cc(5, 34, 99)}).empty() && q.calls.empty());
    CHECK(run(m, {cc(5, 2, 64)}).empty() && q.calls.size() == 1 && q.calls[0].value == raw14(64 << 7));
    // The skin sees each learned move once.
    int raw = -1;
    CHECK(l.takeKnobMove(9, raw) && raw == 0 && !l.takeKnobMove(9, raw) && !l.takeKnobMove(13, raw));
    std::printf("midi_learn_test: value mapping (7-bit, 14-bit pairs)\n");
}

void testSwallow() {
    Recorder p;
    Learn l(p);
    // No bindings: every message passes untouched (the same events, in the same order).
    const std::vector<Msg> mixed = {cc(1, 21, 5), noteOn(1, 60, 100), noteOff(1, 60), {0xE0, 0x00, 0x40}, {0xD0, 0x30},
                                    {0xF0, 0x0F, 0x11, 0x01, 0x00, 0x04, 0xF7}, cc(1, 53, 9), {0xC0, 0x05}, {0xF8}, cc(2, 123, 0)};
    {
        std::vector<Ev> ev;
        for (size_t i = 0; i < mixed.size(); ++i) ev.push_back({int(i * 3), mixed[i].data(), int(mixed[i].size())});
        const std::vector<Ev> before = ev;
        CHECK(l.process(ev.data(), int(ev.size()), 0) == int(ev.size()));
        for (size_t i = 0; i < ev.size(); ++i) CHECK(ev[i].offset == before[i].offset && ev[i].data == before[i].data && ev[i].size == before[i].size);
    }
    // CC 21 bound: only it goes; notes, pitch bend, pressure, SysEx, program change, clock and channel mode pass.
    l.bind({knob(22), 1, false, 21, false});
    std::vector<Msg> left = run(l, mixed);
    CHECK(left.size() == mixed.size() - 2);                                // CC 21 and its LSB (CC 53)
    std::vector<Msg> expect = mixed;
    expect.erase(expect.begin() + 6);
    expect.erase(expect.begin());
    CHECK(left == expect);
    CHECK(p.calls.size() == 2 && p.calls[0].id == 22 && p.calls[0].value == raw7(5) && p.calls[1].value == raw14(5 << 7 | 9));
    // Another channel: not this binding's.
    CHECK(run(l, {cc(2, 21, 7)}).size() == 1);
    // The LSB is taken only when paired with a knob: CC 53 passes once CC 21 is a button's.
    l.bind({button(0x0B), 1, false, 21, false});
    CHECK(run(l, {cc(1, 53, 9)}).size() == 1);
    // ... and when CC 53 is bound itself, that binding wins.
    l.bind({knob(22), 1, false, 21, false});
    l.bind({knob(8), 1, false, 53, false});
    p.calls.clear();
    CHECK(run(l, {cc(1, 21, 10), cc(1, 53, 127)}).empty());
    CHECK(p.calls.size() == 2 && p.calls[0].id == 22 && p.calls[1].id == 8 && p.calls[1].value == 1023);
    // Channel mode messages are never taken, even on a bound channel.
    CHECK(run(l, {cc(1, 120, 0), cc(1, 127, 0)}).size() == 2);
    std::printf("midi_learn_test: swallow (bound CC and its LSB only; everything else byte-identical)\n");
}

void testButtons() {
    Recorder p;
    Learn l(p);
    l.bind({button(0x0B), 1, false, 80, false});                          // Save on CC 80
    l.bind({button(0x20), 0, true, 60, false});                           // Compare on note 60, any channel
    auto presses = [&](int raw, int down) { int n = 0; for (auto& c : p.calls) n += !c.knob && c.id == raw && c.value == down; return n; };
    CHECK(run(l, {cc(1, 80, 127), cc(1, 80, 0)}, 0).empty());
    CHECK(presses(0x0B, 1) == 1 && presses(0x0B, 0) == 1);
    CHECK(run(l, {cc(1, 80, 64), cc(1, 80, 100), cc(1, 80, 63), cc(1, 80, 0)}, 10).empty());   // repeats ignored
    CHECK(presses(0x0B, 1) == 2 && presses(0x0B, 0) == 2);
    CHECK(run(l, {noteOn(3, 60, 90), noteOn(3, 60, 0)}, 20).empty());      // velocity 0 = release
    CHECK(run(l, {noteOn(9, 60, 1), noteOff(9, 60), noteOff(9, 60)}, 30).empty());
    CHECK(presses(0x20, 1) == 2 && presses(0x20, 0) == 2);
    // Held combinations arrive as sent: Save down, Compare down, Compare up, Save up.
    p.calls.clear();
    run(l, {cc(1, 80, 127), noteOn(1, 60, 100), noteOff(1, 60), cc(1, 80, 0)}, 40);
    CHECK(p.calls.size() == 4 && p.calls[0].id == 0x0B && p.calls[0].value == 1 && p.calls[1].id == 0x20 && p.calls[1].value == 1 &&
          p.calls[2].id == 0x20 && p.calls[2].value == 0 && p.calls[3].id == 0x0B && p.calls[3].value == 0);
    Event e;
    CHECK(!l.takeEvent(e));                                               // both edges came: no warning
    // One-edge controllers: a press with no release within 2 s, or a second press: warned once (the binding stays).
    l.bind({button(0x0C), 1, false, 81, false});                          // Dump, as after learning it
    run(l, {cc(1, 81, 127)}, 1000);
    run(l, {}, 2900);
    CHECK(!l.takeEvent(e));
    run(l, {}, 3100);
    CHECK(l.takeEvent(e) && e.kind == Event::OneEdge && e.control == button(0x0C) && !l.takeEvent(e));
    run(l, {}, 9000);
    CHECK(!l.takeEvent(e));                                               // once
    Binding b;
    CHECK(l.bindingOf(button(0x0C), b));
    CHECK(msgOneEdge(b, kNoLabels) == "DUMP = CC 81  ONE EDGE ONLY: SET BUTTON MOMENTARY");   // 50 columns: no channel
    l.bind({button(0x0B), 1, false, 80, false});                          // Save again: a toggle sends 127 twice
    run(l, {cc(1, 80, 127)}, 10000);
    run(l, {cc(1, 80, 127)}, 10500);
    CHECK(l.takeEvent(e) && e.control == button(0x0B));
    CHECK(l.bindingOf(button(0x0B), b) && msgOneEdge(b, kNoLabels).size() <= kMessageCols);
    CHECK(msgOneEdge(b, kNoLabels) == "SAVE = CC 80  ONE EDGE ONLY: SET BUTTON MOMENTARY");   // the channel dropped to fit
    // Forgetting (or moving, or replacing all) a held button releases it first, once.
    p.calls.clear();
    CHECK(l.held(button(0x0B)));
    CHECK(l.forget(button(0x0B)));
    CHECK(p.calls.size() == 1 && p.calls[0].id == 0x0B && p.calls[0].value == 0 && !l.held(button(0x0B)));
    CHECK(run(l, {cc(1, 80, 0)}).size() == 1);                            // not bound any more: passes
    run(l, {cc(1, 81, 0)}, 11000);                                        // Dump released
    run(l, {cc(1, 81, 127)}, 11000);
    p.calls.clear();
    std::vector<int> moved;
    l.bind({button(0x19), 1, false, 81, false}, &moved);                  // CC 81 moves from Dump to Bank
    CHECK(moved.size() == 1 && moved[0] == button(0x0C) && p.calls.size() == 1 && p.calls[0].id == 0x0C && p.calls[0].value == 0);
    run(l, {cc(1, 81, 0)}, 11100);                                        // Bank was never pressed: nothing
    CHECK(p.calls.size() == 1);
    run(l, {cc(1, 81, 127)}, 11200);
    p.calls.clear();
    l.setBindings({});
    CHECK(p.calls.size() == 1 && p.calls[0].id == 0x19 && p.calls[0].value == 0);
    std::printf("midi_learn_test: buttons (both edges, repeats ignored, one-edge warning, held released)\n");
}

void testBindings() {
    Recorder p;
    Learn l(p);
    // Learn: the first eligible message, on the channel it came on; it is swallowed, other messages pass.
    l.arm(knob(9));
    CHECK(l.armed() == knob(9));
    std::vector<Msg> left = run(l, {noteOn(1, 60, 100), {0xE0, 0, 0x40}, cc(3, 21, 77), cc(3, 21, 78), noteOff(1, 60)});
    CHECK(left.size() == 3 && left[0] == noteOn(1, 60, 100) && left[2] == noteOff(1, 60));   // keys still play
    CHECK(l.armed() == kNone);
    CHECK(p.calls.size() == 1 && p.calls[0].id == 9 && p.calls[0].value == raw7(78));   // until committed: provisional
    Event e;
    CHECK(l.takeEvent(e) && e.kind == Event::Learned && e.control == knob(9) && e.channel == 3 && !e.note && e.number == 21);
    std::vector<int> moved;
    CHECK(l.commit(e, &moved) && moved.empty());
    Binding b;
    CHECK(l.bindingOf(knob(9), b) && b.channel == 3 && b.number == 21);
    CHECK(msgBound(b, kNoLabels) == "PERF 1 = CC 21 CH 3");
    // A knob learns from CCs only (notes play on); a button from a CC or a note.
    l.arm(knob(22));
    CHECK(run(l, {noteOn(1, 64, 10)}).size() == 1 && l.armed() == knob(22));
    l.arm(button(0x0B));
    CHECK(run(l, {noteOn(2, 64, 10)}).empty() && l.takeEvent(e) && e.note && e.channel == 2 && e.number == 64);
    l.commit(e);
    // Channel mode messages, wheels, pressure, keys: never learned.
    l.arm(knob(22));
    CHECK(run(l, {cc(1, 121, 0), {0xE0, 0, 0x40}, {0xD0, 9}}).size() == 3 && l.armed() == knob(22));
    l.arm(kNone);
    CHECK(!learnable(23) && !learnable(24) && !learnable(25) && !learnable(-1) && !learnable(0x200));
    l.arm(23);
    CHECK(l.armed() == kNone);
    CHECK(!l.bind({23, 1, false, 30, false}) && !l.bind({knob(22), 1, false, 120, false}) && !l.bind({knob(22), 1, true, 60, false}) &&
          !l.bind({knob(22), 17, false, 30, false}) && !l.bind({button(0x0B), 1, false, 127, false}));
    // One binding per control: a new one replaces it.
    CHECK(l.bind({knob(9), 1, false, 30, false}));
    CHECK(l.bindingOf(knob(9), b) && b.number == 30 && l.controlAt(3, false, 21) == kNone);
    // One control per (channel, CC): the new holder takes it ("CC 30 MOVED  PERF 1 -> CUTOFF").
    moved.clear();
    CHECK(l.bind({knob(22), 1, false, 30, false}, &moved) && moved.size() == 1 && moved[0] == knob(9));
    CHECK(!l.bindingOf(knob(9), b) && l.bindingOf(knob(22), b));
    CHECK(msgMoved(b, moved, kNoLabels) == "CC 30 MOVED  PERF 1 -> CUTOFF");
    // Any takes the CC on every channel and replaces its single-channel bindings, and the other way round.
    l.bind({knob(0), 2, false, 40, false});
    l.bind({knob(1), 3, false, 40, false});
    moved.clear();
    CHECK(l.bind({knob(2), 0, false, 40, false}, &moved) && moved.size() == 2);
    CHECK(run(l, {cc(7, 40, 1), cc(16, 40, 2)}).empty());
    moved.clear();
    CHECK(l.bind({knob(4), 5, false, 40, false}, &moved) && moved.size() == 1 && moved[0] == knob(2));
    CHECK(run(l, {cc(7, 40, 1)}).size() == 1 && run(l, {cc(5, 40, 1)}).empty());
    // Different channels hold the same CC independently.
    l.bind({knob(7), 6, false, 40, false});
    CHECK(l.controlAt(5, false, 40) == knob(4) && l.controlAt(6, false, 40) == knob(7));
    // Learning a CC another control holds moves it there too.
    l.arm(knob(20));
    run(l, {cc(6, 40, 3)});
    CHECK(l.takeEvent(e));
    moved.clear();
    CHECK(l.commit(e, &moved) && moved.size() == 1 && moved[0] == knob(7) && l.controlAt(6, false, 40) == knob(20));
    // Forget all.
    l.forgetAll();
    CHECK(l.bindings().empty() && run(l, {cc(5, 40, 1), cc(1, 30, 1)}).size() == 2);
    std::printf("midi_learn_test: bindings (learn, channel heard, Any, moves, refusals)\n");
}

void testText() {
    const std::vector<Binding> list = {{knob(9), 1, false, 21, true}, {knob(22), 0, false, 74, false}, {button(0x0B), 16, false, 80, false},
                                       {button(0x20), 0, true, 60, false}};
    const std::string t = Learn::toText(list);
    CHECK(t == "k9:1:c21:f k22:0:c74 b0B:16:c80 b20:0:n60");
    CHECK(Learn::fromText(t) == list);
    CHECK(Learn::fromText("").empty() && Learn::toText({}).empty());
    CHECK(Learn::fromText("k23:1:c5 k9:17:c5 k9:1:c120x b0B:1:x5 junk k9:1:c5:g b80:1:c5").empty());
    Recorder p;
    Learn l(p);
    l.setBindings(Learn::fromText(t));
    CHECK(l.bindings() == list);                                          // the 14-bit flag kept
    // Names and texts.
    PerfLabels labels{"Q", "", "W", "E", "R"};
    CHECK(controlName(knob(9), labels) == "Q (Perf 1)" && controlName(knob(13), labels) == "Perf 2" && controlName(knob(22), labels) == "Cutoff");
    CHECK(controlName(button(0x0E), labels) == "\xe2\x88\x92/No" && controlName(button(0x2A), labels) == "Button 0x2A");
    CHECK(shortName(knob(9), labels) == "Q" && shortName(knob(13), labels) == "PERF 2" && shortName(button(0x0E), labels) == "-/NO");
    Binding b{knob(9), 1, false, 21, false};
    CHECK(header(knob(9), &b, labels) == "Q (Perf 1) \xc2\xb7 CC 21, ch 1" && header(knob(9), nullptr, labels) == "Q (Perf 1) \xc2\xb7 not learned");
    CHECK(listEntry(b, labels) == "Q (Perf 1)  CC 21 ch 1");
    b.channel = 0; b.fine = true;
    CHECK(sourceText(b) == "CC 21 (14-bit), any channel" && msgBound(b, labels) == "Q = CC 21 ANY CH");
    CHECK(msgLearn(knob(9), labels) == "LEARN Q  MOVE A CONTROL..." && msgForgotten(b, labels) == "Q: CC 21 FORGOTTEN");
    CHECK(osUse(1, labels) == "OS: mod wheel" && osUse(12, labels) == "OS: Q knob, SYS3" && osUse(71, labels) == "OS: Perf 2 knob" &&
          osUse(74, labels) == "OS: W knob" && osUse(79, labels) == "OS: E knob" && osUse(78, labels) == "OS: R knob, SYS4" && osUse(21, labels).empty());
    for (int c = 0; c < 0x180; ++c)
        if (learnable(c)) CHECK(msgLearn(c, labels).size() <= kMessageCols && !controlName(c, labels).empty());
    std::printf("midi_learn_test: text form, names and messages\n");
}

// The bound path on the audio thread: no allocation, and it never waits on the engine's lock.
void testRealtime(Engine& e) {
    struct ToEngine : Panel {
        Engine& eng;
        explicit ToEngine(Engine& x) : eng(x) {}
        void control(int cc, int raw) override { eng.setControl(cc, raw); }
        void button(int raw, bool down) override { eng.pressRaw(raw, down); }
    } panel(e);
    Learn l(panel);
    l.bind({knob(9), 1, false, 21, false});
    l.bind({button(0x0B), 1, false, 80, false});
    l.bind({button(0x20), 0, true, 60, false});
    const std::vector<Msg> msgs = {cc(1, 21, 3), cc(1, 53, 4), cc(1, 80, 127), noteOn(2, 60, 9), noteOff(2, 60), cc(1, 80, 0), noteOn(1, 61, 9)};
    std::vector<Ev> ev;
    for (const Msg& m : msgs) ev.push_back({0, m.data(), int(m.size())});
    std::atomic<bool> held{false}, release{false}, done{false};
    std::thread locker([&] {
        e.withMachineForTest([&](Machine&) { held = true; while (!release) std::this_thread::yield(); });
        done = true;                                                      // (no machine: the callback never ran)
    });
    while (!held && !done) std::this_thread::yield();
    CHECK(held);
    const auto t0 = std::chrono::steady_clock::now();
    g_allocs = 0;
    int left = 0;
    for (int i = 0; i < 1000; ++i) { std::vector<Ev> copy = ev; g_countAllocs = true; left = l.process(copy.data(), int(copy.size()), i); g_countAllocs = false; }
    g_countAllocs = false;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    release = true;
    locker.join();
    CHECK(left == 1);                                                     // only the unbound note
    CHECK(g_allocs.load() == 0);
    CHECK(ms < 1000);                                                     // never blocked by the engine's lock
    std::printf("midi_learn_test: bound path: %d allocations, %.1f ms for 1000 blocks with the engine's lock held elsewhere\n", g_allocs.load(), ms);
    l.setBindings({});
}

bool waitRunning(Engine& e) {                                         // (generous: sanitizer builds boot slowly)
    for (int i = 0; i < 6000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return e.state() == Engine::State::Running;
}

// The panel path: two engines running the same program, one driven by the mouse calls, one by learned MIDI.
void testPanelPath(const std::string& os, const std::string& wave) {
    Engine a, b;
    a.setRoms(os, wave, "os-md5", "wave-md5");
    b.setRoms(os, wave, "os-md5", "wave-md5");
    CHECK(waitRunning(a) && waitRunning(b));
    a.prepare(44100, 256);
    b.prepare(44100, 256);
    a.withMachineForTest([](Machine& m) { m.panel.keepLog = true; m.panel.transcript.clear(); });
    b.withMachineForTest([](Machine& m) { m.panel.keepLog = true; m.panel.transcript.clear(); });
    struct ToEngine : Panel {
        Engine& eng;
        explicit ToEngine(Engine& x) : eng(x) {}
        void control(int cc, int raw) override { eng.setControl(cc, raw); }
        void button(int raw, bool down) override { eng.pressRaw(raw, down); }
    } panel(b);
    Learn l(panel);
    l.bind({knob(9), 1, false, 21, false});
    l.bind({knob(6), 1, false, 22, false});
    l.bind({button(0x0B), 1, false, 80, false});
    l.bind({button(0x0D), 0, true, 62, false});
    std::vector<float> L(256), R(256);
    // Block by block: the same moves, by setControl / pressRaw on A, by learned MIDI on B (with a few unbound
    // messages for B that A gets too).
    struct Step { std::vector<Msg> midi; };
    const std::vector<Step> steps = {
        {{cc(1, 21, 0)}}, {{cc(1, 21, 40), cc(1, 21, 64)}}, {{cc(1, 22, 127)}}, {{cc(1, 80, 127)}}, {{noteOn(5, 62, 100)}},
        {{noteOff(5, 62), cc(1, 80, 0)}}, {{cc(1, 21, 90), cc(1, 53, 100)}}, {{}}, {{cc(1, 21, 127)}}, {{}}, {{}}, {{cc(1, 80, 127), cc(1, 80, 127)}},
        {{cc(1, 80, 0)}}, {{}}, {{}}};
    bool fine = false, saveDown = false, yesDown = false;
    int msb = 0;
    for (const Step& s : steps) {
        for (const Msg& m : s.midi) {                                     // A: what the panel would do
            if ((m[0] & 0xF0) == 0xB0 && m[1] == 21) { msb = m[2]; a.setControl(9, fine ? raw14(m[2] << 7) : raw7(m[2])); }
            if ((m[0] & 0xF0) == 0xB0 && m[1] == 53) { fine = true; a.setControl(9, raw14(msb << 7 | m[2])); }
            if ((m[0] & 0xF0) == 0xB0 && m[1] == 22) a.setControl(6, raw7(m[2]));
        }
        for (const Msg& m : s.midi) {
            if ((m[0] & 0xF0) == 0xB0 && m[1] == 80) { const bool d = m[2] >= 64; if (d != saveDown) { saveDown = d; a.pressRaw(0x0B, d); } }
            if ((m[0] & 0xF0) == 0x90 && m[1] == 62) { const bool d = m[2] > 0; if (d != yesDown) { yesDown = d; a.pressRaw(0x0D, d); } }
            if ((m[0] & 0xF0) == 0x80 && m[1] == 62) { if (yesDown) { yesDown = false; a.pressRaw(0x0D, false); } }
        }
        std::vector<Ev> ev;
        for (const Msg& m : s.midi) ev.push_back({0, m.data(), int(m.size())});
        const int n = l.process(ev.data(), int(ev.size()), 0);
        CHECK(n == 0);
        a.process(L.data(), R.data(), 256, nullptr, 0);
        b.process(L.data(), R.data(), 256, nullptr, 0);
    }
    std::vector<PanelEvent> ta, tb;
    a.withMachineForTest([&](Machine& m) { ta = m.panel.transcript; });
    b.withMachineForTest([&](Machine& m) { tb = m.panel.transcript; });
    int knobs = 0, buttons = 0;
    for (const PanelEvent& p : ta) if (!p.fromOs) { knobs += (p.bytes[0] & 0xF0) == 0xB0; buttons += p.bytes[0] == 0x81 || p.bytes[0] == 0x80; }
    CHECK(knobs >= 2 && buttons == 6);
    CHECK(ta.size() == tb.size());
    bool same = ta.size() == tb.size();
    for (size_t i = 0; same && i < ta.size(); ++i) same = ta[i].cycle == tb[i].cycle && ta[i].bytes == tb[i].bytes && ta[i].fromOs == tb[i].fromOs;
    CHECK(same);
    CHECK(a.controlMessagesForTest() == b.controlMessagesForTest() && a.buttonMessagesForTest() == b.buttonMessagesForTest());
    for (int cc = 0; cc < 26; ++cc) CHECK(a.controlPosition(cc) == b.controlPosition(cc));
    std::printf("midi_learn_test: panel path: %zu panel messages, the same bytes at the same cycles as the mouse calls (%d knob, %d button)\n",
                ta.size(), knobs, buttons);
    testRealtime(b);

    // The button queue takes several producers at once (the message thread, the skin's render thread, the audio
    // thread's learned buttons): nothing lost, nothing doubled, each producer's edges in its order.
    b.process(L.data(), R.data(), 256, nullptr, 0);                      // what is still queued from above
    b.withMachineForTest([](Machine& m) { m.panel.transcript.clear(); });
    const uint64_t before = b.buttonMessagesForTest();
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t)
        producers.emplace_back([&b, t] { for (int i = 0; i < 200; ++i) b.pressRaw(0x10 + t, i % 2 == 0); });
    for (auto& t : producers) t.join();
    b.process(L.data(), R.data(), 256, nullptr, 0);
    CHECK(b.buttonMessagesForTest() == before + 800);
    std::vector<PanelEvent> tq;
    b.withMachineForTest([&](Machine& m) { tq = m.panel.transcript; });
    int edges[4] = {0, 0, 0, 0};
    bool ordered = true;
    for (const PanelEvent& p : tq) {
        if (p.fromOs || p.bytes.size() != 2 || p.bytes[1] < 0x10 || p.bytes[1] > 0x13) continue;
        int& n = edges[p.bytes[1] - 0x10];
        ordered = ordered && p.bytes[0] == (n % 2 == 0 ? 0x81 : 0x80);
        ++n;
    }
    CHECK(ordered && edges[0] == 200 && edges[1] == 200 && edges[2] == 200 && edges[3] == 200);
    std::printf("midi_learn_test: button queue: 4 producers x 200 edges, all delivered in each producer's order\n");

    // The Built-in skin's -/No and +/Yes (OS buttons 0 and 1) and the panel id the running OS gives them: the same
    // message to the OS by either call (so a binding on that id drives the same button).
    for (int osButton = 0; osButton < 2; ++osButton) {
        const int raw = b.osButtonRaw(osButton);
        CHECK(raw >= 0);
        std::vector<PanelEvent> byOs, byRaw;
        auto edges = [&](std::vector<PanelEvent>& into, bool os) {
            b.process(L.data(), R.data(), 256, nullptr, 0);
            b.withMachineForTest([](Machine& m) { m.panel.transcript.clear(); });
            if (os) { b.pressButton(osButton, true); b.pressButton(osButton, false); }
            else { b.pressRaw(raw, true); b.pressRaw(raw, false); }
            b.process(L.data(), R.data(), 256, nullptr, 0);
            b.withMachineForTest([&](Machine& m) { into = m.panel.transcript; });
        };
        edges(byOs, true);
        edges(byRaw, false);
        CHECK(byOs.size() == 2 && byRaw.size() == 2);
        for (size_t i = 0; i < byOs.size() && i < byRaw.size(); ++i) CHECK(byOs[i].bytes == byRaw[i].bytes && byOs[i].bytes[1] == raw);
    }
    std::printf("midi_learn_test: OS buttons: pressButton and pressRaw(osButtonRaw) send the same message\n");
}
}  // namespace

int main() {
    testMapping();
    testSwallow();
    testButtons();
    testBindings();
    testText();

    // The engine: a small MIDI-echo program of our own (as engine_test) and an empty wave image.
    const fs::path dir = fs::temp_directory_path() / "phyzo_midi_learn_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        const uint16_t kEcho[] = {0x13FC, 0x0005, 0x00F0, 0x071A, 0x41F9, 0x0BE0, 0x0200, 0x0839, 0x0000, 0x00F0, 0x0719,
                                  0x67F6, 0x10F9, 0x00F0, 0x071B, 0x60EE};
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kEcho / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kEcho[i] >> 8); os[0x81 + 2 * i] = uint8_t(kEcho[i]); }
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    testPanelPath((dir / "os").string(), (dir / "wave").string());
    fs::remove_all(dir);
    std::printf("midi_learn_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
