// One loaded skin in one editor window: an RmlUi context with the skin's document, its Lua environment, and the
// bindings between the custom elements (knob, pbutton, led, vfdigit) and the synth (PanelPort).
//
// Threading: RmlUi and Lua are not thread-safe and their state is shared by every plugin instance in the process.
// Every SkinView method takes the global skin lock, so views on different threads (one render thread per editor)
// take turns. Call all methods of one view from one thread (the plugin uses its OpenGL thread).
#pragma once
#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "skin_port.h"

namespace Rml { class Context; class ElementDocument; class Element; class RenderInterface; }

namespace skin {

class KnobElement;
class PButtonElement;
class LedElement;
class VfDigitElement;

// Assumed timings (named so they are easy to find and change).
constexpr double kFlashHz = 2.5;              // 92 flash: blink rate, 50 % duty, all flashing LEDs in phase
constexpr double kBeatLitMs = 100;            // 9D beat flash: lit for min(this, half the interval between beats)
constexpr double kFrameEventHz = 60;          // body onframe at most this often
constexpr double kLuaTimeoutSeconds = 3;      // longest a single entry into Lua may run
constexpr float kBodyWidthDp = 1795, kBodyHeightDp = 848;   // document body size; window = body x zoom

// Modifier bits for input (same values as Rml::Input::KeyModifier).
enum Modifier { ModCtrl = 1, ModShift = 2, ModAlt = 4, ModMeta = 8 };

class SkinView {
public:
    // render: the renderer for this view's context (nullptr: a no-op renderer, for tests). Must outlive the view.
    SkinView(PanelPort& port, PluginInfo info, Rml::RenderInterface* render);
    ~SkinView();
    SkinView(const SkinView&) = delete;
    SkinView& operator=(const SkinView&) = delete;

    // Loads <folder>/<name>.rml. On failure returns false with a plain-language reason (the view is then empty).
    bool load(const std::string& folder, const std::string& name, std::string& error);
    bool loaded() const { return document_ != nullptr; }
    const std::string& folder() const { return folder_; }

    // Window size in physical pixels and the dp ratio (zoom x display scale).
    void setViewport(int widthPx, int heightPx, float dpRatio);

    // Once per displayed frame: reads the synth (LEDs, display, knob positions), runs Lua callbacks and the frame
    // event, and lays out the document. now = seconds on a steady clock.
    void update(double now);
    void render();
    // When this view next needs a frame although the synth did not change (a flashing or beat-flash LED changes, an
    // RmlUi animation or transition runs, the skin has an onframe handler): seconds on update()'s clock, infinity
    // when the panel is still. Valid after update(). A host draws a frame only then, or when the synth or input
    // changed something, never continuously for an unchanged panel.
    double nextWake() const { return nextWake_; }
    // What set nextWake() (for the UI rate log): a flashing LED, a beat LED going out, the onframe handler, or an
    // RmlUi animation or transition.
    enum class Wake { None, Flash, Beat, Frame, Animation };
    Wake nextWakeSource() const { return wakeSource_; }
    // Counts for the UI rate log: frames updated, frames in which something on the panel changed, Lua callbacks.
    struct Stats { uint64_t frames = 0, changedFrames = 0, luaCalls = 0; };
    Stats stats() const;
    void countLuaCall() { ++luaCalls_; }

    // Input in physical pixels (same space as setViewport). button: 0 left, 1 right, 2 middle.
    void mouseMove(int x, int y, int mods);
    void mouseDown(int button, int mods);
    void mouseUp(int button, int mods);
    void mouseWheel(float deltaY, int mods);   // positive: towards the user (scroll down)
    void mouseLeave();
    void releaseHeldButtons();                 // Esc
    void keyDown(int rmlKey, int mods);
    void keyUp(int rmlKey, int mods);

    void setDebuggerVisible(bool on);
    bool debuggerVisible() const { return debugger_; }

    // Tests: runs Lua code in this view's environment; returns the first result as text, or "error: ...".
    std::string evalForTest(const std::string& code);

    // Recent log lines (warnings and Lua errors) of this view, oldest first.
    std::vector<std::string> log() const;
    // Called with every new log line (any thread that holds the skin lock).
    void (*onLog)(void* user, const std::string& line) = nullptr;
    void* onLogUser = nullptr;

    // ---- used by the elements and the Lua API (skin lock held)
    static SkinView* current();                // the view whose code is running
    PanelPort& port() { return port_; }
    const PluginInfo& info() const { return info_; }
    Rml::Context* context() const { return context_; }
    void addLog(const std::string& line);

    void registerElement(KnobElement* e);
    void registerElement(PButtonElement* e);
    void registerElement(LedElement* e);
    void registerElement(VfDigitElement* e);
    void unregisterElement(KnobElement* e);
    void unregisterElement(PButtonElement* e);
    void unregisterElement(LedElement* e);
    void unregisterElement(VfDigitElement* e);

    // Knob value changed by the user or by Lua: send it and tell Lua.
    void knobSent(int cc, int raw);
    void beginKnobDrag(KnobElement* k);
    // Button edges from the mouse, a latching hold or Lua.
    void button(int raw, bool down);
    bool isHeld(int raw) const { return held_.count(raw) != 0; }
    void setHeld(int raw, bool held);
    void setMousePressed(PButtonElement* b) { mousePressed_ = b; }
    void setKnob(int cc, int raw);             // panel.setKnob
    int ledState(int code);                    // panel.led: 0 off, 1 on, 2 flash (beat flash counts as flash)
    bool ledLit(int code) const;
    std::array<uint8_t, 4> displayBytes() const { return segs_; }
    double now() const { return now_; }
    float dpRatio() const { return dpRatio_; }

    // Lua callbacks (registry references), see skin_lua.cpp.
    std::map<int, std::vector<int>> onLedFns, onBeatFns, onKnobFns, onButtonFns;
    std::vector<int> onDisplayFns;
    int envRef = -2;                           // LUA_NOREF until the environment exists

private:
    friend class ViewScope;
    void unload();
    void pollLeds();
    void pollDisplay();
    bool applyLedClasses();
    void computeNextWake();
    void dispatchShow(Rml::Element* e);

    bool systemOk_ = false;
    std::string systemError_;
    PanelPort& port_;
    PluginInfo info_;
    Rml::RenderInterface* render_;
    std::unique_ptr<Rml::RenderInterface> nullRender_;
    Rml::Context* context_ = nullptr;
    Rml::ElementDocument* document_ = nullptr;
    std::string contextName_, folder_;
    int widthPx_ = 1, heightPx_ = 1;
    float dpRatio_ = 1;
    double now_ = 0, lastFrameEvent_ = -1;
    bool debugger_ = false;

    std::set<KnobElement*> knobs_;
    std::set<PButtonElement*> buttons_;
    std::set<LedElement*> leds_;
    std::set<VfDigitElement*> digits_;
    KnobElement* drag_ = nullptr;
    int dragY_ = 0, dragStartY_ = 0, dragStartRaw_ = 0;
    bool dragFine_ = false;
    int mouseX_ = 0, mouseY_ = 0;
    PButtonElement* mousePressed_ = nullptr;
    std::set<int> held_;

    std::array<LedReport, 256> leds{};
    std::array<double, 256> beatSeen_{};
    std::array<bool, 256> lit_{};
    std::array<uint8_t, 4> segs_{};
    bool segsKnown_ = false;
    std::vector<std::string> log_;

    // change tracking: Lua callbacks only for real changes; frames only when needed
    std::array<int, 26> knobSent_{};          // last value sent per cc (-1: none yet)
    std::set<int> down_;                      // buttons currently down (press/release edges only)
    bool changed_ = false;                    // something on the panel changed in this update
    bool hasFrameHandler_ = false;            // the document has onframe
    double nextWake_ = 0;
    Wake wakeSource_ = Wake::None;
    std::atomic<uint64_t> frames_{0}, changedFrames_{0}, luaCalls_{0};
};

}  // namespace skin
