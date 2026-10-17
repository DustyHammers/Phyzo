// The plugin window (v0.9.0): one window over the panel (and the editor unit) with four tabs.
//  - SYSTEM: this instance (version and format, the ROMs, the data folder, the startup log) and Developer (the RmlUi
//    debugger, Write skin log).
//  - LIBRARY: the preset library (docs/editor/LIBRARY.md): files, their presets, the synth's two banks; Save with a
//    name, Rename, Delete, Load a bank into Bank 1 or 2, Save a bank as a file, Import.
//  - SKIN: the skins, Reload, the skins folder, the editor unit, Zoom.
//  - MIDI: the virtual port, MIDI to the host, Sync to host tempo, the MIDI learn bindings.
// The footer carries the window's messages, confirmations, progress and the Save name box, whatever tab shows.
// It never changes the plugin's size; Esc or the round close button closes it.
// Size and place (v0.9.1): laid out at kLayoutW x kLayoutH dp and drawn kDrawnWidth dp of the skin wide (the layout
// scaled uniformly), centred in the plugin area (the panel and the editor unit), scaled down only when the area less
// ui::dp::inset on each side is too small. The rest of the area is the window's backdrop: dimmed, with the window's
// shadow; a click there closes the window as Esc does (an open box or question is cancelled first); the panel under
// it gets no mouse events, except in the plugin's bottom-right corner (the skin's resize grip).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>
#include "library/service.h"
#include "ui/Widgets.h"

class PhyzoEditor;
class PhyzoProcessor;

class PhyzoWindow : public juce::Component, private juce::Timer {
public:
    enum Tab { kSystem = 0, kLibrary = 1, kSkin = 2, kMidi = 3 };
    static constexpr float kLayoutW = 1180, kLayoutH = 720, kDrawnWidth = 1000;
    static constexpr int kGripSize = 24;             // the plugin's bottom-right corner, in points: the skin's grip
    PhyzoWindow(PhyzoEditor& editor, PhyzoProcessor& proc, const ui::Theme& theme, ui::MenuLookAndFeel& menus);
    ~PhyzoWindow() override;

    void showTab(int tab);
    int tab() const { return tab_; }
    // Rack SAVE: Save with a name (LIBRARY): refused while the Compare LED is lit; else the current preset's name in
    // the footer's box.
    void startSave();
    // A message in the footer (the library's, MIDI learn's).
    void message(const juce::String& text);
    void refresh();                                  // re-read what the tabs show (a setting changed, the skin, the font)
    bool cancelEditing();                            // Esc: an inline rename or a footer question closed (false: none)
    // The backdrop over `area` (the plugin's area in the parent) and the window centred in it; `pointsPerDp`: points
    // per dp of the skin (Built-in: per pixel of its 100 % size).
    void place(juce::Rectangle<int> area, float pointsPerDp);
    // The window as drawn, in the parent's coordinates; its points per layout dp.
    juce::Rectangle<float> windowArea() const { return placement_.bounds + getPosition().toFloat(); }
    float scale() const { return placement_.scale; }

    // Tests
    ui::Frame& frame() { return frame_; }
    juce::Component* page(int tab);
    void tickForTest() { timerCallback(); }
    void confirmLoadForTest(const juce::String& path, const juce::String& file, int bank) { confirmLoad(path, file, bank); }
    void renameForTest(const juce::String& path, const juce::String& name);   // the inline Rename box, as its menu item
    bool renamingForTest() const;

    void resized() override;
    void paint(juce::Graphics&) override;            // the backdrop
    bool keyPressed(const juce::KeyPress&) override;
    bool hitTest(int x, int y) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override {}   // never to the panel
    void mouseMagnify(const juce::MouseEvent&, float) override {}
    void visibilityChanged() override;               // its timer runs only while it shows

private:
    class SystemPage;
    class SkinPage;
    class MidiPage;
    class LibraryPage;
    friend class LibraryPage;
    void timerCallback() override;
    void showFooter();                               // the footer for the current state
    library::Service* library();

    // Footer state (whatever tab shows)
    enum class Ask { None, SaveWait, SaveName, Delete, Load };
    void ask(Ask a) { ask_ = a; showFooter(); }
    void confirmDelete(const juce::String& path, const juce::String& name);
    void confirmLoad(const juce::String& path, const juce::String& file, int bank);
    void doSave();

    PhyzoEditor& editor_;
    PhyzoProcessor& proc_;
    const ui::Theme& theme_;
    ui::MenuLookAndFeel& menus_;
    ui::Frame frame_;
    std::unique_ptr<SystemPage> system_;
    std::unique_ptr<SkinPage> skin_;
    std::unique_ptr<MidiPage> midi_;
    std::unique_ptr<LibraryPage> library_;
    int tab_ = kSystem;
    ui::Placement placement_;
    float pointsPerDp_ = 1.0f;

    Ask ask_ = Ask::None;
    library::Service::SaveDraft draft_;
    juce::String askPath_, askName_;
    int askBank_ = 0;
    juce::String message_;
    juce::uint32 messageAt_ = 0;
    juce::String shownFooter_;                       // what the idle footer shows (repaint on change only)
    bool bankJobShown_ = false;
    static constexpr int kMessageMs = 8000;
};
