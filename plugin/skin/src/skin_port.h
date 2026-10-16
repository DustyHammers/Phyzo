// What a skin needs from the synth. The plugin implements it over the Engine; tests use a recording fake.
// No JUCE and no emulator types here: the skin runtime (plugin/skin) builds and tests on its own.
#pragma once
#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace skin {

enum class LedMode : uint8_t { Off = 0, On = 1, Flash = 2, Beat = 3 };   // panel messages 90, 91, 92, 9D

struct LedReport {
    LedMode mode = LedMode::Off;
    uint32_t beats = 0;              // number of 9D messages so far for this code
    float beatIntervalMs = 0;        // machine time between the last two 9D messages (0: only one so far)
};

// The editor unit (F-30; docs/editor/EDITOR_SPEC.md): what the skin's editor document shows and the legends it can
// click. The plugin implements it over the editor controller.
struct EditorSpan {                                  // a value's cells on the glass and its reference
    int row = 0, col = 0, len = 0, ref = -1;
    bool pick = false;                               // a name to pick (a preset), not a value
};
struct EditorState {
    static constexpr int kRows = 4, kCols = 50;
    std::array<std::string, kRows> rows;             // kCols characters each
    std::array<std::array<bool, kCols>, kRows> underline{}, dot{};
    int focusRow = -1, focusCol = 0, focusLen = 0;   // the focused value (blinks); row -1: none
    bool typing = false;                             // typed entry in the focused value
    std::vector<EditorSpan> spans;                   // where the values are (clicks, drags, right-click)
    std::string area, section;                       // legends of the current area and section
    int sound = 0, osc = 0;                          // 0-3, 0-1
    bool oscBoth = false;                            // both oscillators edited: O1 and O2 both lit
    bool soundTargets = false, oscTargets = false;   // S1-S4 / O1 / O2 apply to the area
    int page = 0, pages = 0;                         // 1-based page in the section, pages in the section
    bool prev = false, next = false, follow = true, data = false, edited = false;
    std::set<std::string> areas, sections;           // legends that lead somewhere
    bool open = false;                               // the editor unit is shown
    uint32_t serial = 0;                             // changes when anything above changes
};

class EditorPort {
public:
    virtual ~EditorPort() = default;
    virtual bool editorOpen() = 0;
    virtual uint32_t editorSerial() = 0;
    virtual void editorState(EditorState& out) = 0;
    // A legend was clicked (action and text as in docs/editor/EDITOR_PANEL_LAYOUT.json), or a gesture on a value
    // (text = the span's reference): "focus", "pick", "drag_begin", "drag" (text = steps), "drag_end", "type".
    virtual void editorCommand(const std::string& action, const std::string& text) = 0;
    // The LEDs Follow watches: Sound 1-4, OSC 1/2, envelope Pitch / Filter / Amp (-1: none).
    virtual void editorFollowLeds(const std::array<int, 4>& sound, const std::array<int, 2>& osc, const std::array<int, 3>& env) = 0;
    // More LEDs for Follow on panel knobs: Noise, LFO, Filter Keyboard (-1: none).
    virtual void editorFollowExtraLeds(int /*noise*/, int /*lfo*/, int /*filterKbd*/) {}
    // The performance knobs' labels on the COMMON page (perf1-perf5, at most 4 characters each).
    virtual void editorKnobLabels(const std::array<std::string, 5>& /*labels*/) {}
};

class PanelPort {
public:
    virtual ~PanelPort() = default;
    virtual void sendControl(int cc, int raw) = 0;            // one panel message Bx cc vv, absolute raw 0-1023
    virtual int controlPosition(int cc) = 0;                  // the last value sent for the cc (F-02 defaults when fresh)
    virtual void sendButton(int raw, bool down) = 0;          // 81 raw / 80 raw
    virtual LedReport led(int code) = 0;
    virtual std::array<uint8_t, 4> segments() = 0;            // display segment bytes, leftmost digit first
    // Positions of individual knob elements (stacked knobs share a cc), kept in the plugin state.
    virtual bool knobPosition(const std::string& id, int& raw) = 0;
    virtual void setKnobPosition(const std::string& id, int raw) = 0;
    virtual bool audioClip() { return false; }                // Input Clip LED (F-19); unlit until then
    // MIDI learn (docs/MIDI_LEARN.md): a learned control moved this knob's cc from the host's MIDI (the engine already
    // has it). Taken once; the view shows it on the cc's visible knobs as panel.setKnob does, without sending it again.
    virtual bool takeLearnedKnob(int /*cc*/, int& /*raw*/) { return false; }
    virtual EditorPort* editor() { return nullptr; }          // the editor unit (F-30); none: no editor
    // A rack legend was clicked (v0.9.0, `<legend action="rack" value="...">`): EDITOR (show or hide the editor unit)
    // or SAVE (the plugin window's LIBRARY with the Save footer). Called from the skin's thread; the plugin passes
    // it to its message thread.
    virtual void rackCommand(const std::string& /*value*/) {}
};

struct PluginInfo {
    std::string name = "Phyzo", vendor = "DHammers", version;
};

}  // namespace skin
