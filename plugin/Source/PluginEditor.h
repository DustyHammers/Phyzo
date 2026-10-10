// The plugin window: shows the chosen skin (a file skin from ~/Documents/Phyzo/skins/, drawn by RmlUi, or the
// compiled-in "Built-in" skin) at the chosen zoom, and the right-click menu (skin, reload, zoom, developer).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include "PluginProcessor.h"

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
    // The preset library's controls (message thread): LIBRARY with the editor unit closed opens it; EXPORT's menu.
    void openEditorUnit();
    void showExportMenu();
    // MIDI learn's messages while the editor unit is closed: the window's banner, 3 s.
    void showLearnMessage(const juce::String& text);

private:
    juce::StringArray skinNames() const;             // discovered file skins
    juce::File skinFolder(const juce::String& name) const;
    void showSkin(const juce::String& name, const juce::String& notice = {});
    void showBuiltIn(const juce::String& notice);
    void skinFailed(const juce::String& name, const juce::String& reason);
    void applyScale(float scale);
    void cornerResize(int phase, juce::Point<int> offset);    // window = 100 % size (winscale) x scale (0.5-2.0)
    juce::Point<int> baseSize() const;               // 1795 x 848 for file skins (+ 394 with the editor unit), Built-in's own size
    void updateEditorUnit();                         // shows the editor unit if the project wants it and the skin has one
    void showMenu(const juce::MouseEvent&, int control = -1);   // control: MIDI learn's items on top (-1: none)
    // The plugin menu; on top: MIDI learn's items for a panel control, or Revert for an edited editor value (-1: none).
    void openMenu(int control, int valueRef);
    void addLearnItems(juce::PopupMenu& menu, int control);
    juce::PopupMenu learnListMenu();                 // MIDI > MIDI learn
    bool showValueMenu(int ref);                     // an editor value: the plugin menu, Revert on top if it was edited
    bool editorKey(const juce::KeyPress& k);         // the editor unit's focused value and typed entry
    void timerCallback() override;                   // missing-ROM message on file skins; saves the size

    PhyzoProcessor& proc;
    std::unique_ptr<BuiltinView> builtin_;
    std::unique_ptr<RmlSkinComponent> rml_;
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
