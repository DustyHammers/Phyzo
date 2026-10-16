// A file skin (RmlUi document from ~/Documents/Phyzo/skins/<name>/) drawn with OpenGL.
//
// Everything RmlUi and Lua do for this window runs on the window's OpenGL thread: the skin view is created there
// when the GL context starts, input is queued to it, and it is destroyed there when the context closes. Results
// (skin loaded or failed) come back to the message thread through onLoaded.
#pragma once
#include <juce_opengl/juce_opengl.h>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "PluginProcessor.h"

class RmlGLRenderer;
namespace skin { class SkinView; }

class RmlSkinComponent : public juce::Component, private juce::OpenGLRenderer, private juce::Timer {
public:
    explicit RmlSkinComponent(PhyzoProcessor& p);
    ~RmlSkinComponent() override;

    // Loads <folder>/<name>.rml (asynchronously). onLoaded(ok, error) follows on the message thread.
    void loadSkin(const juce::File& folder, const juce::String& name);
    void reload();
    void setZoom(float zoom) { zoom_ = zoom; }
    void setDebugger(bool on);
    bool debuggerOn() const { return debugger_; }
    // A short message across the top of the skin (e.g. why another skin is shown); empty hides it.
    void showMessage(const juce::String& text, int seconds = 10);
    const juce::String& message() const { return message_; }   // the message shown now (empty: none)
    // A message that stays until cleared (missing ROMs); shown above the other one.
    void setPersistentMessage(const juce::String& text);
    // The editor unit (F-30): whether the loaded skin has one (<name>-editor.rml; known when onLoaded comes), and
    // showing it below the panel (kept across reloads).
    bool hasEditor() const { return hasEditor_.load(); }
    void setEditorVisible(bool on);
    // The loaded skin's UI font file (<meta name="ui-font">; empty: none or missing), known when onLoaded comes.
    const juce::String& uiFontFile() const { return uiFont_; }
    // A line for the skin log (written only while Write skin log is on).
    void writeSkinLog(const juce::String& line) { writeLog(this, line.toStdString()); }

    std::function<void(bool ok, const juce::String& error)> onLoaded;
    // Right-click outside the editor unit's values: the main menu, with the panel control under the mouse (MIDI
    // learn: a knob's cc, 0x100 | a panel button's id; -1: none or not learnable).
    std::function<void(const juce::MouseEvent&, int control)> onRightClick;
    // Esc: true if the plugin used it (a MIDI learn cancelled); otherwise Esc releases the held panel buttons and goes
    // on to the host.
    std::function<bool()> onEscape;
    // Right-click on a value of the editor unit (its reference): the plugin menu with the value's Revert on top (if
    // edited). Returns false to show the plain menu instead.
    std::function<bool(int ref)> onEditorValueMenu;
    // A key for the editor unit (its focused value, typed entry); returns true if the editor used it.
    std::function<bool(const juce::KeyPress&)> onEditorKey;
    // Drags in the bottom-right corner resize the window (the OpenGL view would hide the editor's own resizer):
    // phase 0 = start, 1 = drag (offset from the start, screen pixels), 2 = end.
    std::function<void(int phase, juce::Point<int> offset)> onCornerResize;
    static constexpr int kCornerSize = 24;

    // Frames drawn so far (GL thread; for tests and the UI rate log).
    uint64_t framesDrawn() const { return framesDrawn_.load(); }
    // True while the window can be seen: showing, not minimised and, on macOS, not fully covered by other windows.
    bool visibleOnScreen() const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    void newOpenGLContextCreated() override;
    void renderOpenGL() override;
    void openGLContextClosing() override;
    void timerCallback() override;
    void post(std::function<void()> f);
    void startLoad();
    int toPx(float v) const;
    static int mods(const juce::ModifierKeys& m);
    static void writeLog(void* self, const std::string& line);

    PhyzoProcessor& proc_;
    juce::File logFile_;
    juce::OpenGLContext gl_;
    std::unique_ptr<RmlGLRenderer> renderer_;
    std::unique_ptr<skin::SkinView> view_;
    std::mutex queueMtx_;
    std::vector<std::function<void()>> queue_;
    std::atomic<int> width_{1}, height_{1};
    std::atomic<float> zoom_{1.0f}, scale_{1.0f};
    std::string folder_, name_;            // the skin to show (GL thread)
    int viewW_ = 0, viewH_ = 0;            // last viewport given to the view (GL thread)
    float viewDp_ = 0;
    bool debugger_ = false;
    bool cornerDrag_ = false;
    juce::Point<int> cornerStart_;
    juce::String message_, persistent_, uiFont_;
    juce::uint32 messageUntil_ = 0;
    juce::String glError_;

    // Frames only when needed (B-11, F-27): the timer asks for one when the synth's panel changed (engine UI
    // counter), input arrived, or the skin's next wake time came, and never while the window cannot be seen.
    std::atomic<bool> frameWanted_{true};
    std::atomic<bool> hasEditor_{false}, editorVisible_{false};
    std::atomic<bool> viewReady_{false};                   // the GL thread has a skin view (it answers hit tests)
    uint32_t lastLearnSerial_ = 0;                         // MIDI learn's knob moves seen (a frame shows them)
    uint32_t lastEditorSerial_ = 0;
    std::atomic<double> nextWake_{0};
    std::atomic<int> wakeSource_{0};                       // skin::SkinView::Wake of nextWake_
    uint32_t lastSerial_ = 0;
    std::atomic<uint64_t> framesDrawn_{0};
    // UI rate log (message thread): counts at the start of the current second
    double rateStart_ = 0;
    uint64_t rateFrames_ = 0, rateMessages_ = 0, rateLua_ = 0, rateChanged_ = 0;
    uint32_t rateSerial_ = 0;
    // why frames were asked for in the current second: mouse/keys/loading, panel change, flashing LED, beat LED,
    // onframe, RmlUi animation or transition; and mouse events received
    enum { kWhyInput, kWhyPanel, kWhyFlash, kWhyBeat, kWhyFrame, kWhyAnimation, kWhyCount };
    uint32_t why_[kWhyCount] = {};
    uint32_t mouseEvents_ = 0;
    std::atomic<uint64_t> luaCalls_{0}, changedFrames_{0};   // copied from the view by the GL thread
    void logRates(double now);
};
