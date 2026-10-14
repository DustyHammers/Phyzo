// MIDI learn in the plugin (F-55, docs/MIDI_LEARN.md; no ROMs, no editor window): the project state (version 6/7 with
// the bindings, byte-identical round trip; versions 1-5 load as before and get the default bindings; an empty list
// stays empty; with no bindings the state is v0.8.3.5's plus the version, the empty list and the sync byte), the defaults in the
// settings, a held learned button released when a project loads, and the host's MIDI in processBlock (a bound CC
// moves its panel control). The settings' default bindings are put back as they were.
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>
#include "PluginProcessor.h"
#include "SkinSettings.h"
#include "state_io.h"

using midilearn::Binding;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// A stand-in engine state (no ROMs here: the engine keeps it, as it would until it has ROMs).
std::vector<uint8_t> engineBlob() {
    StateWriter w;
    w.raw("PHZE", 4); w.put(uint32_t(1)); w.str("test-os-md5"); w.str("test-wave-md5"); w.vec(std::vector<uint8_t>(1000, 7));
    return w.bytes;
}

// A project state as processor version `version` writes it (the layout in PluginProcessor.cpp; version 7 ends with
// Sync to host tempo, 1 = While playing).
juce::MemoryBlock projectState(int version, const std::vector<Binding>& bindings, float scale = 1.0f, int sync = 1) {
    const std::vector<uint8_t> engine = engineBlob();
    juce::MemoryBlock dest;
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(0x505A4850);
    out.writeInt(version);
    out.writeInt(1);
    juce::MemoryBlock packed;
    {
        juce::MemoryOutputStream raw(packed, false);
        juce::GZIPCompressorOutputStream gz(raw, 6);
        gz.write(engine.data(), engine.size());
    }
    out.writeInt64(juce::int64(engine.size()));
    out.writeInt64(juce::int64(packed.getSize()));
    out.write(packed.getData(), packed.getSize());
    if (version >= 2) { out.writeInt(1); out.writeString("vol"); out.writeInt(700); }
    if (version >= 3) out.writeFloat(scale);
    if (version >= 4) out.writeByte(0);
    if (version >= 5) out.writeByte(0);
    if (version >= 6) {
        out.writeInt(int(bindings.size()));
        for (const Binding& b : bindings) {
            out.writeInt(b.control); out.writeByte(char(b.channel)); out.writeByte(char(b.number));
            out.writeByte(b.note ? 1 : 0); out.writeByte(b.fine ? 1 : 0);
        }
    }
    if (version >= 7) out.writeByte(char(sync));
    out.flush();
    return dest;
}

juce::MemoryBlock stateOf(juce::AudioProcessor& p) { juce::MemoryBlock b; p.getStateInformation(b); return b; }
void load(juce::AudioProcessor& p, const juce::MemoryBlock& s) { p.setStateInformation(s.getData(), int(s.getSize())); }
std::unique_ptr<PhyzoProcessor> make() { return std::unique_ptr<PhyzoProcessor>(static_cast<PhyzoProcessor*>(createPluginFilter())); }
}  // namespace

int main() {
    juce::ScopedJuceInitialiser_GUI gui;
    const juce::String savedDefaults = SkinSettings::midiLearnDefaults();
    SkinSettings::clearMidiLearnDefaults();
    const std::vector<Binding> some = {{midilearn::knob(9), 1, false, 21, true}, {midilearn::knob(22), 0, false, 74, false},
                                       {midilearn::button(0x0B), 3, false, 80, false}, {midilearn::button(0x20), 0, true, 60, false}};
    const std::vector<Binding> defaults = {{midilearn::knob(6), 2, false, 30, false}};

    // A new instance with no defaults saved: no bindings. Its state: version 7, the list empty, then the sync byte.
    {
        auto p = make();
        CHECK(p->learn().bindings().empty());
        load(*p, projectState(5, {}));
        const juce::MemoryBlock s = stateOf(*p);
        CHECK(s == projectState(7, {}));                              // = v5 + the version + an empty list + While playing
    }
    // Version 6 and 7 with bindings: they load (14-bit flags included); a version 7 state comes back byte for byte, a
    // version 6 one as version 7 with While playing.
    {
        auto p = make();
        load(*p, projectState(6, some, 1.25f));
        CHECK(p->learn().bindings() == some);
        CHECK(stateOf(*p) == projectState(7, some, 1.25f));
        const juce::MemoryBlock s = projectState(7, some, 1.25f);
        load(*p, s);
        CHECK(p->learn().bindings() == some);
        CHECK(stateOf(*p) == s);
        // ... into another instance too.
        auto q = make();
        load(*q, stateOf(*p));
        CHECK(q->learn().bindings() == some && stateOf(*q) == s);
    }
    // Defaults: saved from an instance, given to new instances and to projects from before version 6; a version 6
    // project with an empty list stays empty. Clear default: new instances start with none again.
    {
        auto p = make();
        load(*p, projectState(6, defaults));
        p->saveLearnDefaults();
        CHECK(SkinSettings::midiLearnDefaults() == "k6:2:c30");
        auto fresh = make();
        CHECK(fresh->learn().bindings() == defaults);
        for (int v = 1; v <= 5; ++v) {
            auto old = make();
            load(*old, projectState(v, some));                         // (older versions have no list)
            CHECK(old->learn().bindings() == defaults);
            if (v >= 3) CHECK(old->editorScale.load() == 1.0f);        // the rest as before
        }
        auto empty = make();
        load(*empty, projectState(6, {}));
        CHECK(empty->learn().bindings().empty());
        CHECK(stateOf(*empty) == projectState(7, {}));
        p->clearLearnDefaults();
        CHECK(SkinSettings::midiLearnDefaults().isEmpty());
        auto none = make();
        CHECK(none->learn().bindings().empty());
    }
    // The host's MIDI: a bound CC moves its panel control (the same call as the mouse) and is taken out; a held
    // learned button is released when a project loads.
    {
        auto p = make();
        p->prepareToPlay(44100, 256);
        load(*p, projectState(6, some));
        juce::AudioBuffer<float> buf(2, 256);
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::controllerEvent(1, 21, 64), 0);
        midi.addEvent(juce::MidiMessage::controllerEvent(3, 80, 127), 10);
        midi.addEvent(juce::MidiMessage::noteOn(1, 64, uint8_t(100)), 20);
        p->processBlock(buf, midi);
        CHECK(p->engine().controlPosition(9) == midilearn::raw14(64 << 7));   // 14-bit seen: the MSB is the coarse value
        CHECK(p->learn().held(midilearn::button(0x0B)) && p->engine().anyButtonHeld());
        load(*p, projectState(6, {}));
        CHECK(!p->learn().held(midilearn::button(0x0B)) && !p->engine().anyButtonHeld());
        // Learn from the host's MIDI: the channel heard, committed by the timer; the message that completed it taken.
        p->armLearn(midilearn::knob(13));
        midi.clear();
        midi.addEvent(juce::MidiMessage::controllerEvent(7, 2, 10), 0);
        p->processBlock(buf, midi);
        juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
        Binding b;
        CHECK(p->learn().bindingOf(midilearn::knob(13), b) && b.channel == 7 && b.number == 2 && !b.note);
    }
    if (savedDefaults.isNotEmpty()) SkinSettings::setMidiLearnDefaults(savedDefaults);
    else SkinSettings::clearMidiLearnDefaults();
    std::printf("midi_learn_plugin_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
