// The plugin's editor: shows the chosen skin (a file skin from ~/Documents/Phyzo/skins/, drawn by RmlUi, or the
// compiled-in "Built-in" skin) at the chosen zoom, and over it, when opened, the plugin window (PhyzoWindow: SYSTEM,
// LIBRARY, SKIN, MIDI). Right-click: on a learnable control MIDI learn's menu, on an edited editor value its Revert,
// each with "Open window..."; anywhere else the window opens.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include "PluginProcessor.h"
#include "PhyzoWindow.h"
#include "ui/Widgets.h"

class RmlSkinComponent;

// The compiled-in skin (Phase 4 approved mockup): title and version, the display drawn from the OS's segment bytes,
// previous/next preset, Status, and the Debug section with CPU per block. No image files.
class BuiltinView : public juce::Component, private juce::Timer {
public:
    static constexpr int kWidth = 520, kHeight = 316;
    explicit BuiltinView(PhyzoProcessor&);
    ~BuiltinView() override;
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void setNotice(const juce::String& text, int seconds = 15);   // e.g. why this skin is shown instead of the chosen one
    bool keyPressed(const juce::KeyPress&) override;
    // Right-click: the main menu, with MIDI learn's items for -/No or +/Yes when the click was on one of them and it
    // is learnable (control: 0x100 | its panel id; -1: none).
    std::function<void(const juce::MouseEvent&, int control)> onRightClick;
    std::function<bool()> onEscape;                  // Esc: true if used (a MIDI learn cancelled)
    // MIDI learn: -/No and +/Yes are the same panel controls as the rack skin's (raw 0x0E and 0x0D) when the running
    // OS gives its two buttons those ids (then pressButton and pressRaw send it the same message); else not learnable.
    int learnControl(int osButton) const;
    std::function<void(int phase, juce::Point<int> offset)> onCornerResize;   // as RmlSkinComponent's

private:
    bool cornerDrag_ = false;
    juce::Point<int> cornerStart_;
    class PanelButton : public juce::Component {     // presses an OS button (0 = -/No, 1 = +/Yes) while held
    public:
        PanelButton(PhyzoProcessor& p, int osButton, juce::String label) : proc(p), id(osButton), text(std::move(label)) {}
        void paint(juce::Graphics&) override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;
        int osButton() const { return id; }
    private:
        PhyzoProcessor& proc;
        int id;
        juce::String text;
        bool down = false;
    };
    void timerCallback() override;
    void paintDisplay(juce::Graphics&, juce::Rectangle<int> area);
    void paintStatus(juce::Graphics&, int y);
    void paintDebug(juce::Graphics&, int y);

    PhyzoProcessor& proc;
    PanelButton minus, plus;
    juce::ToggleButton debugToggle{"Debug"};
    juce::String notice_;
    juce::uint32 noticeUntil_ = 0;
    std::size_t lastSignature_ = 0;                  // what was last painted (repaint only on change)
};

class PhyzoEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit PhyzoEditor(PhyzoProcessor&);
    ~PhyzoEditor() override;
    void resized() override;
    // Tests: the usable screen area the window must fit (F-32); empty: the display the window is on.
    static juce::Rectangle<int> screenAreaForTest;
    float maxScale() const;                          // the largest scale that fits the screen (50-200 %)
    // MIDI learn's and the library's messages: the window's footer while it is open, else the banner (3 s).
    void showLearnMessage(const juce::String& text);

    // ---- the plugin window (v0.9.0)
    void openWindow(int tab);                        // PhyzoWindow::Tab; opened if closed
    void closeWindow();
    bool windowOpen() const { return window_ != nullptr && window_->isVisible(); }
    PhyzoWindow* window() { return windowOpen() ? window_.get() : nullptr; }
    // A rack legend (skin): EDITOR shows or hides the editor unit, SAVE opens the window's Save with a name.
    void rackCommand(const juce::String& value);
    // A right-click outside the editor unit's values: MIDI learn's menu for a learnable panel control (0x100 | a button
    // id, or a knob's cc), else the window on SYSTEM.
    void rightClick(int control);
    // The right-click menu for a learnable panel control or an editor value: MIDI learn's items or the value's Revert,
    // then "Open window...". Empty: no menu (the window opens instead).
    juce::PopupMenu menuFor(int control, int valueRef);
    void showThemedMenu(juce::PopupMenu& menu);
    // What the window's tabs drive (the old right-click menu's Skin, Reload, Zoom, Editor, Developer).
    juce::StringArray skinNames() const;             // discovered file skins
    const juce::String& currentSkin() const { return current_; }
    void chooseSkin(const juce::String& name);
    void reloadSkin();
    bool fileSkin() const { return rml_ != nullptr; }
    bool hasEditorUnit() const;
    bool editorUnitShown() const { return editorShown_; }
    void setEditorUnit(bool on);
    float scale() const { return scale_; }
    void setZoomStep(int percent);
    bool debuggerOn() const { return debugger_; }
    void setDebugger(bool on);
    // The window's and the menus' theme: the skin's UI font (<meta name="ui-font">), else JUCE's default sans.
    const ui::Theme& theme() const { return theme_; }
    ui::MenuLookAndFeel& menuLookAndFeel() { return menus_; }

private:
    juce::File skinFolder(const juce::String& name) const;
    void placeWindow();                              // the window over the whole plugin area, on top
    void loadUiFont(const juce::String& fontFile);   // the skin's ui-font (empty: none); a font JUCE cannot read is logged
    void showSkin(const juce::String& name, const juce::String& notice = {});
    void showBuiltIn(const juce::String& notice);
    void skinFailed(const juce::String& name, const juce::String& reason);
    void applyScale(float scale);
    void cornerResize(int phase, juce::Point<int> offset);    // window = 100 % size (winscale) x scale (0.5-2.0)
    juce::Point<int> baseSize() const;               // 1795 x 848 for file skins (+ 394 with the editor unit), Built-in's own size
    void updateEditorUnit();                         // shows the editor unit if the project wants it and the skin has one
    void showMenu(const juce::MouseEvent&, int control = -1);   // control: a learnable panel control (-1: none)
    void addLearnItems(juce::PopupMenu& menu, int control);
    bool showValueMenu(int ref);                     // an editor value: its Revert if it was edited, else the window
    bool editorKey(const juce::KeyPress& k);         // the editor unit's focused value and typed entry
    void timerCallback() override;                   // missing-ROM message on file skins; saves the size

    PhyzoProcessor& proc;
    ui::Theme theme_;
    ui::MenuLookAndFeel menus_{theme_};
    std::unique_ptr<BuiltinView> builtin_;
    std::unique_ptr<RmlSkinComponent> rml_;
    std::unique_ptr<PhyzoWindow> window_;            // made when first opened; hidden when closed
    juce::String current_;                           // the skin being shown
    float scale_ = 1.0f;
    bool editorShown_ = false;                       // the editor unit is attached below the panel
    bool applying_ = false;
    bool scaleDirty_ = false;                        // global setting not yet written
    juce::ComponentBoundsConstrainer constrainer_;
    juce::Rectangle<int> dragStartBounds_;
    bool debugger_ = false;
    uint32_t editorNotice_ = 0;                      // the editor's last message shown (its serial)
    juce::String editorNoticeText_;                  // its text while the banner may still show it
    static constexpr int kEditorNoticeSeconds = 5;
};
