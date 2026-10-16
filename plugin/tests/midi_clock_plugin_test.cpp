// Sync to host tempo in the plugin (F-68; no ROMs): the setting in the project state (version 7 and later, round trip;
// version 6 and older load as While playing; new instances start at While playing), the window's MIDI tab (v0.9.0: Off,
// While playing, Always; the current one selected, each sets it; needs a display), and processBlock with a fake
// playhead: the ticks reach the block's MIDI on the host's grid after the host's events at the same offset, the host's
// own F8 is taken out when the setting is not Off, and Off leaves the block's MIDI exactly as the host sent it.
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <cstdio>
#include "PhyzoWindow.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/Widgets.h"
#include "state_io.h"

using midiclock::Mode;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct FakePlayHead : juce::AudioPlayHead {
    bool present = true, hasBpm = true, playing = true;
    double bpm = 120, ppq = 0;
    juce::Optional<PositionInfo> getPosition() const override {
        if (!present) return {};
        PositionInfo p;
        if (hasBpm) p.setBpm(bpm);
        p.setPpqPosition(ppq);
        p.setIsPlaying(playing);
        return p;
    }
};

// A project state as processor version `version` writes it (the layout in PluginProcessor.cpp; no bindings).
juce::MemoryBlock projectState(int version, int sync) {
    StateWriter w;
    w.raw("PHZE", 4); w.put(uint32_t(1)); w.str("test-os-md5"); w.str("test-wave-md5"); w.vec(std::vector<uint8_t>(1000, 7));
    juce::MemoryBlock dest;
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(0x505A4850);
    out.writeInt(version);
    out.writeInt(1);
    juce::MemoryBlock packed;
    {
        juce::MemoryOutputStream raw(packed, false);
        juce::GZIPCompressorOutputStream gz(raw, 6);
        gz.write(w.bytes.data(), w.bytes.size());
    }
    out.writeInt64(juce::int64(w.bytes.size()));
    out.writeInt64(juce::int64(packed.getSize()));
    out.write(packed.getData(), packed.getSize());
    out.writeInt(0);                                 // no knob positions
    out.writeFloat(1.0f);
    out.writeByte(0);
    out.writeByte(0);
    if (version >= 6) out.writeInt(0);               // no MIDI learn bindings
    if (version >= 7) out.writeByte(char(sync));
    if (version >= 8) { out.writeString({}); out.writeString({}); }   // no files loaded into the banks
    out.flush();
    return dest;
}

juce::MemoryBlock stateOf(juce::AudioProcessor& p) { juce::MemoryBlock b; p.getStateInformation(b); return b; }
void load(juce::AudioProcessor& p, const juce::MemoryBlock& s) { p.setStateInformation(s.getData(), int(s.getSize())); }
std::unique_ptr<PhyzoProcessor> make() { return std::unique_ptr<PhyzoProcessor>(static_cast<PhyzoProcessor*>(createPluginFilter())); }

struct Ev { int offset; std::vector<uint8_t> bytes; };
std::vector<Ev> blockEvents(const PhyzoProcessor& p) {
    std::vector<Ev> v;
    for (const auto& e : p.blockEventsForTest()) v.push_back({e.offset, std::vector<uint8_t>(e.data, e.data + e.size)});
    return v;
}
bool isClock(const Ev& e) { return e.bytes.size() == 1 && e.bytes[0] == 0xF8; }
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;

    // The setting and the project state.
    {
        auto p = make();
        CHECK(p->syncMode() == Mode::WhilePlaying);  // a new instance
        load(*p, projectState(8, 1));                // a stand-in engine state (no ROMs: the engine keeps it)
        for (Mode m : {Mode::Off, Mode::Always, Mode::WhilePlaying}) {
            p->setSyncMode(m);
            auto q = make();
            q->setSyncMode(m == Mode::Off ? Mode::Always : Mode::Off);
            load(*q, stateOf(*p));
            CHECK(q->syncMode() == m);
            CHECK(stateOf(*q) == stateOf(*p));
        }
        for (int s = 0; s <= 2; ++s) {
            auto q = make();
            load(*q, projectState(8, s));
            CHECK(int(q->syncMode()) == s);
            CHECK(stateOf(*q) == projectState(8, s));   // byte for byte
            load(*q, projectState(7, s));               // version 7: the same setting
            CHECK(int(q->syncMode()) == s && stateOf(*q) == projectState(8, s));
        }
        for (int v = 1; v <= 6; ++v) {               // older projects: While playing
            auto q = make();
            q->setSyncMode(Mode::Off);
            load(*q, projectState(v, 0));
            CHECK(q->syncMode() == Mode::WhilePlaying);
        }
        auto q = make();
        q->setSyncMode(Mode::Always);
        load(*q, projectState(7, 9));                // an unknown value: While playing
        CHECK(q->syncMode() == Mode::WhilePlaying);
        std::printf("midi_clock_plugin_test: the setting round-trips; version 6 and older load as While playing\n");
    }

    // The window's MIDI tab (v0.9.0; the old menu's three items): the current one selected; choosing one sets it.
    {
        auto p = make();
        std::unique_ptr<juce::AudioProcessorEditor> base(p->createEditor());
        auto& ed = static_cast<PhyzoEditor&>(*base);
        ed.openWindow(PhyzoWindow::kMidi);
        juce::Component* found = nullptr;
        std::function<void(juce::Component*)> look = [&](juce::Component* c) {
            if (c->getComponentID() == "sync") found = c;
            for (auto* k : c->getChildren()) look(k);
        };
        look(ed.window());
        auto* sync = dynamic_cast<ui::Segmented*>(found);
        CHECK(sync != nullptr);
        if (sync) {
            CHECK((sync->items() == juce::StringArray{"Off", "While playing", "Always"}));
            for (Mode cur : {Mode::Off, Mode::WhilePlaying, Mode::Always}) {
                p->setSyncMode(cur);
                ed.window()->refresh();
                CHECK(sync->selected() == int(cur));
            }
            for (int i : {0, 1, 2}) { sync->choose(i); CHECK(int(p->syncMode()) == i); }
        }
        base.reset();
        std::printf("midi_clock_plugin_test: the MIDI tab has the three choices, the current one selected\n");
    }

    // processBlock with a fake playhead: 120 bpm at 48 kHz, a tick every 1000 samples.
    {
        auto p = make();
        FakePlayHead ph;
        p->setPlayHead(&ph);
        p->prepareToPlay(48000, 512);
        juce::AudioBuffer<float> buf(2, 512);
        auto run = [&](Mode mode, double ppq, const juce::MidiBuffer& in) {
            p->setSyncMode(mode);
            ph.ppq = ppq;
            juce::MidiBuffer midi(in);
            p->processBlock(buf, midi);
            return blockEvents(*p);
        };
        juce::MidiBuffer host;
        host.addEvent(juce::MidiMessage::noteOn(1, 60, uint8_t(100)), 0);
        host.addEvent(juce::MidiMessage::midiClock(), 3);          // the host's own clock
        host.addEvent(juce::MidiMessage::controllerEvent(1, 7, 99), 200);
        // While playing, from ppq 0: our tick at 0 after the host's note; the host's F8 is gone.
        std::vector<Ev> e = run(Mode::WhilePlaying, 0.0, host);
        CHECK(e.size() == 3);
        if (e.size() == 3) {
            CHECK(e[0].offset == 0 && e[0].bytes.size() == 3 && e[0].bytes[0] == 0x90);
            CHECK(e[1].offset == 0 && isClock(e[1]));
            CHECK(e[2].offset == 200 && e[2].bytes[0] == 0xB0);
        }
        // The next block (ppq 512 / 24000): no tick in [512, 1024) except the one at 1000 -> offset 488.
        e = run(Mode::WhilePlaying, 512.0 / 24000.0, {});
        CHECK(e.size() == 1 && e[0].offset == 488 && isClock(e[0]));
        // Always: the same on the grid; the host's F8 out too.
        e = run(Mode::Always, 1024.0 / 24000.0, host);
        CHECK(std::count_if(e.begin(), e.end(), isClock) == 0 && e.size() == 2);   // [1024, 1536): no tick; F8 gone
        // Stopped: While playing sends nothing (and still takes the host's F8 out); Always free-runs.
        ph.playing = false;
        e = run(Mode::WhilePlaying, 0.0, host);
        CHECK(e.size() == 2 && std::count_if(e.begin(), e.end(), isClock) == 0);
        int ticks = 0;
        for (int b = 0; b < 10; ++b) { e = run(Mode::Always, 0.0, {}); ticks += int(std::count_if(e.begin(), e.end(), isClock)); }
        CHECK(ticks == 5 || ticks == 6);             // 5120 samples at 1000 a tick
        // No tempo (no playhead; a playhead without bpm): nothing in any mode.
        ph.playing = true;
        for (Mode mode : {Mode::WhilePlaying, Mode::Always}) {
            ph.present = false;
            e = run(mode, 0.0, {});
            CHECK(e.empty());
            ph.present = true; ph.hasBpm = false;
            e = run(mode, 0.0, {});
            CHECK(e.empty());
            ph.hasBpm = true;
        }
        // Off: the block's MIDI exactly as the host sent it, the host's F8 included, no ticks.
        e = run(Mode::Off, 0.0, host);
        CHECK(e.size() == 3);
        if (e.size() == 3) {
            CHECK(e[0].offset == 0 && e[0].bytes[0] == 0x90);
            CHECK(e[1].offset == 3 && isClock(e[1]));
            CHECK(e[2].offset == 200 && e[2].bytes[0] == 0xB0);
        }
        // Block sizes from 1 to 4096 on the same timeline give the same ticks (123.4 bpm).
        ph.bpm = 123.4;
        auto timeline = [&](int n) {
            p->setSyncMode(Mode::WhilePlaying);
            juce::AudioBuffer<float> b(2, n);
            std::vector<int64_t> at;
            for (int64_t s = 0; s < 96000; s += n) {
                ph.ppq = double(s) * ph.bpm / (60.0 * 48000.0);
                juce::MidiBuffer midi;
                p->processBlock(b, midi);
                for (const auto& ev : p->blockEventsForTest())
                    if (ev.size == 1 && ev.data[0] == 0xF8 && s + ev.offset < 96000) at.push_back(s + ev.offset);
            }
            return at;
        };
        p->prepareToPlay(48000, 4096);
        const std::vector<int64_t> ref = timeline(4096);
        CHECK(ref.size() > 90);
        for (int n : {1, 7, 64, 441, 512, 1024}) {
            p->prepareToPlay(48000, n);
            CHECK(timeline(n) == ref);
        }
        p->setPlayHead(nullptr);
        std::printf("midi_clock_plugin_test: ticks on the host's grid, host F8 taken out unless Off, Off untouched\n");
    }

    std::printf("midi_clock_plugin_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
