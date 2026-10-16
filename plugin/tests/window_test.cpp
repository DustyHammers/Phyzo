// The plugin window (v0.9.0; no ROMs; needs a display): the shared theme (the skin's UI font or JUCE's default sans,
// "…" for text that does not fit), the right-click routing (MIDI learn's menu or Revert with "Open window...", else the
// window; the old Skin / Reload / Zoom / Editor / MIDI / Developer items gone), rack legends, Esc, the SYSTEM, SKIN and
// MIDI tabs driving the same settings the old menu did, the LIBRARY tab against a library service with made-up hidden
// machines (Save with a name, Delete, Load a bank: the footer's questions and messages, the Memory column's title),
// and project state version 8 (older versions load with no last-loaded names).
//
//   window_test <test skin folder> [folder for PNG snapshots]
#include <juce_audio_processors/juce_audio_processors.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include "LibraryText.h"
#include "PhyzoWindow.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "RmlSkinComponent.h"
#include "SkinSettings.h"
#include "library/syx.h"
#include "skin_view.h"
#include "state_io.h"
#include "ui/Widgets.h"
#if JUCE_LINUX
#include <X11/Xlib.h>
#undef KeyPress
#endif

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

void pump(int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil(ms); }

juce::Component* findId(juce::Component* c, const juce::String& id) {
    if (c == nullptr) return nullptr;
    if (c->getComponentID() == id) return c;
    for (auto* child : c->getChildren()) if (auto* r = findId(child, id)) return r;
    return nullptr;
}
template <class T> T* find(juce::Component* root, const juce::String& id) { return dynamic_cast<T*>(findId(root, id)); }

juce::StringArray menuTexts(const juce::PopupMenu& m) {
    juce::StringArray out;
    for (juce::PopupMenu::MenuItemIterator it(m); it.next();) out.add(it.getItem().isSeparator ? juce::String("---") : it.getItem().text);
    return out;
}

void snapshot(juce::Component& c, const juce::File& dir, const juce::String& name) {
    if (dir == juce::File()) return;
    const juce::Image img = c.createComponentSnapshot(c.getLocalBounds(), true, 2.0f);
    juce::File f = dir.getChildFile(name + ".png");
    f.deleteFile();
    juce::FileOutputStream os(f);
    juce::PNGImageFormat().writeImageToStream(img, os);
}

// ---- made-up hidden machines (no OS data): presets and banks carry their names as 16 bytes
Bytes presetMsg(const std::string& name) {
    Bytes m = {0xF0, 0x0F, 0x11, 0x01, 0x00, library::kPresetCommand, library::kPresetType, 0x7F, 0x7F};
    std::string n = name; n.resize(16, ' ');
    for (char c : n) m.push_back(uint8_t(c) & 0x7F);
    m.push_back(0xF7);
    return m;
}
Bytes bankMsg(int bank, const std::vector<std::string>& names) {
    Bytes m = {0xF0, 0x0F, 0x11, 0x01, 0x00, library::kBankCommand, uint8_t(bank), 0x00};
    for (const auto& name : names) { std::string n = name; n.resize(16, ' '); for (char c : n) m.push_back(uint8_t(c) & 0x7F); }
    m.push_back(0xF7);
    return m;
}
std::string trimmed(std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); return s; }

struct Model {
    std::vector<std::string> stored = std::vector<std::string>(64);
    std::string edit;
    bool compare = false;
    int current = 0;
};
struct Shared {
    std::mutex mtx;
    Model running;
    std::vector<Bytes> sent;
    Shared() { for (int i = 0; i < 64; ++i) running.stored[size_t(i)] = "Factory " + std::to_string(i + 1); running.edit = running.stored[0]; }
};
class FakeOps : public library::Ops {
public:
    FakeOps(Model m) : m_(std::move(m)) {}
    bool receive(const Bytes& msg, const library::Progress&) override {
        int bank = -1;
        const library::Kind k = library::classify(msg, &bank);
        if (k == library::Kind::Preset) { m_.edit = trimmed(std::string(msg.begin() + 9, msg.begin() + 25)); m_.compare = true; }
        if (k == library::Kind::Bank)
            for (int i = 0; i < 32; ++i) m_.stored[size_t(bank * 32 + i)] = trimmed(std::string(msg.begin() + 8 + 16 * i, msg.begin() + 24 + 16 * i));
        return true;
    }
    bool programChange(int p) override { m_.current = p; m_.edit = m_.stored[size_t(p)]; m_.compare = false; return true; }
    bool readName(uint16_t n, std::string& name) override {
        if (n == 0) name = m_.edit; else if (n >= 0x40 && n < 0x80) name = m_.stored[size_t(n - 0x40)]; else return false;
        return true;
    }
    bool compareLit() override { return m_.compare; }
    bool waitForPreset() override { return true; }
    int displayedPreset() override { return m_.current; }
    bool dumpPreset(Bytes& msg) override { msg = presetMsg(m_.edit); return true; }
    bool dumpBank(int bank, Bytes& msg, const library::Progress&) override {
        msg = bankMsg(bank, std::vector<std::string>(m_.stored.begin() + bank * 32, m_.stored.begin() + bank * 32 + 32));
        return true;
    }
    std::string error() const override { return {}; }
private:
    Model m_;
};
Model factoryModel() { Shared s; return s.running; }
class FakeMachines : public library::Machines {
public:
    explicit FakeMachines(Shared& sh) : sh_(sh) {}
    std::unique_ptr<library::Ops> fresh(const std::atomic<bool>&, std::string&) override { return std::make_unique<FakeOps>(factoryModel()); }
    std::unique_ptr<library::Ops> clone(const Bytes&, const std::atomic<bool>&, std::string&) override {
        std::lock_guard<std::mutex> lk(sh_.mtx);
        return std::make_unique<FakeOps>(sh_.running);
    }
private:
    Shared& sh_;
};

bool waitFor(std::function<bool()> f, int ms = 10000) {
    for (int i = 0; i < ms / 10; ++i) { if (f()) return true; pump(10); }
    return f();
}

// A project state as processor `version` writes it (the layout in PluginProcessor.cpp; no knobs, no bindings).
juce::MemoryBlock projectState(int version, const juce::String& bank1 = {}, const juce::String& bank2 = {}) {
    StateWriter w;
    w.raw("PHZE", 4); w.put(uint32_t(1)); w.str("test-os-md5"); w.str("test-wave-md5"); w.vec(std::vector<uint8_t>(1000, 7));
    juce::MemoryBlock dest;
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(0x505A4850);
    out.writeInt(version);
    out.writeInt(1);
    juce::MemoryBlock packed;
    { juce::MemoryOutputStream raw(packed, false); juce::GZIPCompressorOutputStream gz(raw, 6); gz.write(w.bytes.data(), w.bytes.size()); }
    out.writeInt64(juce::int64(w.bytes.size()));
    out.writeInt64(juce::int64(packed.getSize()));
    out.write(packed.getData(), packed.getSize());
    out.writeInt(0);
    out.writeFloat(1.0f);
    out.writeByte(0);
    out.writeByte(0);
    if (version >= 6) out.writeInt(0);
    if (version >= 7) out.writeByte(1);
    if (version >= 8) { out.writeString(bank1); out.writeString(bank2); }
    out.flush();
    return dest;
}
juce::MemoryBlock stateOf(juce::AudioProcessor& p) { juce::MemoryBlock b; p.getStateInformation(b); return b; }
void load(juce::AudioProcessor& p, const juce::MemoryBlock& s) { p.setStateInformation(s.getData(), int(s.getSize())); }
std::unique_ptr<PhyzoProcessor> make() { return std::unique_ptr<PhyzoProcessor>(static_cast<PhyzoProcessor*>(createPluginFilter())); }

juce::File systemFont() {
    for (const char* p : {"/System/Library/Fonts/Supplemental/Arial.ttf", "/System/Library/Fonts/Supplemental/Courier New.ttf",
                          "/Library/Fonts/Arial.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                          "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf"})
        if (juce::File(p).existsAsFile()) return juce::File(p);
    return {};
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { std::printf("usage: window_test <test skin folder> [snapshot folder]\n"); return 2; }
    juce::ScopedJuceInitialiser_GUI gui;
#if JUCE_LINUX
    XSetErrorHandler([](Display*, XErrorEvent*) { return 0; });
#endif
    const juce::File shots = argc > 2 ? juce::File(argv[2]) : juce::File();
    if (shots != juce::File()) shots.createDirectory();
    const juce::String previousSkin = SkinSettings::skin();
    const float previousScale = SkinSettings::scale();
    const bool previousLog = SkinSettings::skinLog(), previousHost = SkinSettings::midiToHost();
    PhyzoEditor::screenAreaForTest = {0, 0, 6000, 4000};

    // ---- 1. The theme: the skin's font or JUCE's default sans; one line of text ends in "…" when it does not fit.
    {
        ui::Theme t;
        CHECK(t.font(14).getTypefaceName() == juce::Font::getDefaultSansSerifFontName());
        const juce::File tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("phyzo_window_test_font.ttf");
        tmp.replaceWithText("not a font");
        CHECK(ui::Theme::loadTypeface(tmp) == nullptr);
        CHECK(ui::Theme::loadTypeface(tmp.getSiblingFile("phyzo_missing.ttf")) == nullptr);
        tmp.deleteFile();
        if (const juce::File f = systemFont(); f != juce::File()) {
            t.typeface = ui::Theme::loadTypeface(f);
            CHECK(t.typeface != nullptr && t.font(14).getTypefacePtr() == t.typeface);
        } else {
            std::printf("window_test: no system font file found here: the font load itself not checked\n");
        }
        ui::Theme d;
        const juce::Font f = d.font(14);
        const juce::String longText = "A preset name far too long for its little cell";
        const juce::String cut = ui::ellipsize(longText, f, 80);
        CHECK(cut.endsWith(juce::String::fromUTF8("\xe2\x80\xa6")) && ui::textWidth(f, cut) <= 80 && cut.length() < longText.length());
        CHECK(ui::ellipsize("Pad", f, 200) == "Pad");
        CHECK(ui::ellipsize(longText, f, 1).isEmpty());
        std::printf("window_test: theme font and fallback, ellipsis\n");
    }

    // ---- 2. Built-in: the right-click routing, rack legends, Esc, the tabs and their settings.
    {
        SkinSettings::setSkin(SkinSettings::kBuiltIn);
        auto p = make();
        std::unique_ptr<juce::AudioProcessorEditor> base(p->createEditor());
        auto& ed = static_cast<PhyzoEditor&>(*base);
        ed.addToDesktop(0);
        ed.setVisible(true);
        pump(200);
        // A learnable control: MIDI learn's items, then "Open window..."; the old items are gone.
        const juce::StringArray learn = menuTexts(ed.menuFor(midilearn::knob(11), -1));
        CHECK(learn.contains("MIDI learn") && learn.contains("Set CC") && learn.contains("Channel"));
        CHECK(learn[learn.size() - 1] == juce::String::fromUTF8("Open window\xe2\x80\xa6") && learn[learn.size() - 2] == "---");
        for (const char* old : {"Skin", "Reload skin    F5", "Zoom", "Editor", "MIDI", "Developer"}) CHECK(!learn.contains(old));
        CHECK(ed.menuFor(-1, -1).getNumItems() == 0 && ed.menuFor(0x7F, -1).getNumItems() == 0);   // no menu: the window
        CHECK(ed.menuFor(-1, 12345).getNumItems() == 0);                                  // a value with nothing to revert
        CHECK(!ed.windowOpen());
        ed.rightClick(-1);
        CHECK(ed.windowOpen() && ed.window()->tab() == PhyzoWindow::kSystem);
        PhyzoWindow& w = *ed.window();
        CHECK(w.getParentComponent() == &ed);
        // It covers the whole plugin area and never changes the plugin's size.
        const auto covered = w.getBoundsInParent();
        CHECK(covered.getX() == 0 && covered.getY() == 0 && std::abs(covered.getRight() - ed.getWidth()) <= 2 && std::abs(covered.getBottom() - ed.getHeight()) <= 2);
        CHECK(ed.getWidth() == BuiltinView::kWidth && ed.getHeight() == BuiltinView::kHeight);
        snapshot(ed, shots, "builtin-system");
        // Esc: the window's (closed); then nothing of ours is open: Esc goes to the host.
        CHECK(w.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        CHECK(!ed.windowOpen());
        if (auto* b = dynamic_cast<BuiltinView*>(ed.getChildComponent(0))) CHECK(!b->keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)));
        // Rack legends: SAVE opens LIBRARY (no library without the ROMs: a message); EDITOR: Built-in has no editor unit.
        ed.rackCommand("SAVE");
        CHECK(ed.windowOpen() && w.tab() == PhyzoWindow::kLibrary);
        CHECK(w.frame().footer().text() == "NOT SAVED  THE SYNTH IS NOT RUNNING");
        ed.rackCommand("EDITOR");
        CHECK(!p->editorWanted() && !ed.editorUnitShown());
        // The tabs: header shows the four.
        CHECK((w.frame().tabs().tabs() == juce::StringArray{"SYSTEM", "LIBRARY", "SKIN", "MIDI"}));
        w.frame().tabs().choose(PhyzoWindow::kSystem);
        CHECK(w.tab() == PhyzoWindow::kSystem && w.page(PhyzoWindow::kSystem)->isVisible() && !w.page(PhyzoWindow::kLibrary)->isVisible());
        // SYSTEM: the debugger only for file skins; Write skin log is the global setting.
        auto* dbg = find<ui::Switch>(&w, "debugger");
        auto* slog = find<ui::Switch>(&w, "skinLog");
        CHECK(dbg && !dbg->isEnabled() && slog);
        if (slog) {
            SkinSettings::setSkinLog(false); w.refresh();
            slog->toggle();
            CHECK(SkinSettings::skinLog() && slog->isOn());
            slog->toggle();
            CHECK(!SkinSettings::skinLog());
        }
        // SKIN: the list, Zoom (the steps that do not fit greyed out), the editor unit (none here).
        w.showTab(PhyzoWindow::kSkin);
        auto* zoom = find<ui::Segmented>(&w, "zoom");
        auto* unit = find<ui::Switch>(&w, "editorUnit");
        CHECK(zoom && unit && !unit->isEnabled());
        if (zoom) {
            zoom->choose(5);                                                     // 150 %
            CHECK(std::abs(ed.scale() - 1.5f) < 1e-4f && std::abs(SkinSettings::scale() - 1.5f) < 1e-4f && zoom->selected() == 5);
            CHECK(ed.getWidth() == juce::roundToInt(BuiltinView::kWidth * 1.5f));
            snapshot(ed, shots, "builtin-skin-150");
            PhyzoEditor::screenAreaForTest = {0, 0, 800, 600};
            w.refresh();
            CHECK(zoom->itemEnabled(0) && !zoom->itemEnabled(7));
            zoom->choose(7);                                                     // 200 %: does not fit, nothing happens
            CHECK(std::abs(ed.scale() - 1.5f) < 1e-4f);
            PhyzoEditor::screenAreaForTest = {0, 0, 6000, 4000};
            zoom->choose(2);
            CHECK(std::abs(ed.scale() - 1.0f) < 1e-4f);
        }
        // MIDI: Send MIDI to host, Sync to host tempo, the MIDI learn table.
        w.showTab(PhyzoWindow::kMidi);
        auto* host = find<ui::Switch>(&w, "midiToHost");
        auto* sync = find<ui::Segmented>(&w, "sync");
        CHECK(host && sync && sync->items().size() == 3);
        if (host) {
            SkinSettings::setMidiToHost(false); w.refresh();
            host->toggle();
            CHECK(SkinSettings::midiToHost());
            host->toggle();
            CHECK(!SkinSettings::midiToHost());
        }
        if (sync) {
            for (int m : {0, 2, 1}) { sync->choose(m); CHECK(int(p->syncMode()) == m); }
            CHECK(sync->selected() == 1);
        }
        p->setLearnCc(midilearn::knob(11), 21);
        w.refresh();
        auto* forgetAll = find<ui::Button>(&w, "forgetAll");
        CHECK(forgetAll && forgetAll->isEnabled());
        snapshot(ed, shots, "builtin-midi");
        if (forgetAll) forgetAll->click();
        CHECK(p->learn().bindings().empty());
        w.refresh();
        CHECK(forgetAll && !forgetAll->isEnabled());
        // The close button.
        w.frame().closeButton().onClick();
        CHECK(!ed.windowOpen());
        base.reset();
        pump(100);
        std::printf("window_test: right-click routing, rack legends, Esc, SYSTEM / SKIN / MIDI tabs (Built-in)\n");
    }

    // ---- 3. A file skin: the window over it (the panel and the editor unit), the editor unit switch and the rack
    // EDITOR legend, the skin's UI font (missing: the default sans and one skin-log line).
    {
        const juce::File skins = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Phyzo/skins/wintest");
        skins.deleteRecursively();
        skins.createDirectory();
        for (const char* f : {"test.rml", "test.rcss", "test.lua", "test-editor.rml"})
            juce::File(argv[1]).getChildFile(f).copyFileTo(skins.getChildFile(juce::String(f).replace("test", "wintest")));
        const juce::File log = skins.getParentDirectory().getSiblingFile("skin-log.txt");
        log.deleteFile();
        SkinSettings::setSkinLog(true);
        SkinSettings::setSkin("wintest");
        auto p = make();
        p->setEditorWanted(false);
        std::unique_ptr<juce::AudioProcessorEditor> base(p->createEditor());
        auto& ed = static_cast<PhyzoEditor&>(*base);
        ed.addToDesktop(0);
        ed.setVisible(true);
        CHECK(waitFor([&] { return ed.fileSkin() && ed.hasEditorUnit(); }));
        CHECK(ed.theme().typeface == nullptr);                                  // MissingFont.ttf: JUCE's default sans
        int fontLines = 0;
        juce::StringArray lines;
        lines.addLines(log.loadFileAsString());
        for (const auto& l : lines) fontLines += l.contains("ui-font");
        CHECK(fontLines == 1);
        ed.openWindow(PhyzoWindow::kSkin);
        PhyzoWindow& w = *ed.window();
        CHECK(dynamic_cast<RmlSkinComponent*>(w.getParentComponent()) != nullptr);
        auto* unit = find<ui::Switch>(&w, "editorUnit");
        CHECK(unit && unit->isEnabled() && !unit->isOn());
        const int h0 = ed.getHeight();
        if (unit) unit->toggle();
        CHECK(p->editorWanted() && ed.editorUnitShown() && ed.getHeight() > h0);
        // The window covers the panel and the editor unit: its dp size is the whole body.
        CHECK(std::abs(float(w.getHeight()) - (skin::kBodyHeightDp + skin::kEditorHeightDp)) < 2.0f);
        ed.rackCommand("EDITOR");                                                 // the rack legend: off again
        CHECK(!p->editorWanted() && !ed.editorUnitShown() && ed.getHeight() == h0);
        CHECK(w.isVisible());                                                     // still open over the smaller panel
        snapshot(w, shots, "file-skin");
        // The SKIN list switches skins (Built-in and back); the window stays open.
        auto* list = find<juce::Component>(&w, "skinList");
        CHECK(list != nullptr);
        ed.chooseSkin(SkinSettings::kBuiltIn);
        CHECK(!ed.fileSkin() && SkinSettings::skin() == SkinSettings::kBuiltIn && ed.windowOpen() && w.getParentComponent() == &ed);
        ed.chooseSkin("wintest");
        CHECK(waitFor([&] { return ed.fileSkin(); }) && SkinSettings::skin() == "wintest" && ed.windowOpen());
        // With a real font file as its ui-font, the theme uses it.
        if (const juce::File f = systemFont(); f != juce::File()) {
            f.copyFileTo(skins.getChildFile("MissingFont.ttf"));
            ed.reloadSkin();
            CHECK(waitFor([&] { return ed.theme().typeface != nullptr; }));
        }
        base.reset();
        pump(200);
        skins.deleteRecursively();
        std::printf("window_test: file skin: window over panel and editor unit, editor unit switch, rack EDITOR, ui-font\n");
    }

    // ---- 4. LIBRARY with a library of made-up hidden machines.
    {
        const fs::path data = fs::temp_directory_path() / "phyzo_window_test_data";
        const fs::path trash = fs::temp_directory_path() / "phyzo_window_test_trash";
        fs::remove_all(data); fs::remove_all(trash);
        fs::create_directories(data / "Library" / "Singles");
        std::vector<std::string> mine(32);
        for (int i = 0; i < 32; ++i) mine[size_t(i)] = "Mine " + std::to_string(i + 1);
        auto write = [](const fs::path& f, const Bytes& b) { fs::create_directories(f.parent_path()); std::ofstream(f, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), long(b.size())); };
        write(data / "Library" / "My Bank.syx", bankMsg(1, mine));
        write(data / "Library" / "Singles" / "Old Lead.syx", presetMsg("Old Lead"));
        Shared sh;
        library::Service::Config cfg;
        cfg.dataDir = data.string();
        cfg.osMd5 = "0123456789abcdef0123456789abcdef";
        cfg.machineState = [] { return Bytes{1, 2, 3}; };
        cfg.sendToMachine = [&sh](const Bytes& m) {
            std::lock_guard<std::mutex> lk(sh.mtx);
            sh.sent.push_back(m);
            for (int i = 0; i < 32; ++i) sh.running.stored[size_t(m[6] * 32 + i)] = trimmed(std::string(m.begin() + 8 + 16 * i, m.begin() + 24 + 16 * i));
            return true;
        };
        cfg.machineInputBusy = [] { return false; };
        cfg.moveToTrash = [trash](const std::string& path) {
            std::error_code ec; fs::create_directories(trash, ec); fs::rename(path, trash / fs::u8path(path).filename(), ec); return !ec;
        };
        SkinSettings::setSkin(SkinSettings::kBuiltIn);
        auto p = make();
        p->setLibraryServiceForTest(std::make_unique<library::Service>(cfg, std::make_unique<FakeMachines>(sh)));
        library::Service& s = *p->libraryService();
        std::unique_ptr<juce::AudioProcessorEditor> base(p->createEditor());
        auto& ed = static_cast<PhyzoEditor&>(*base);
        ed.addToDesktop(0);
        ed.setVisible(true);
        ed.setSize(BuiltinView::kWidth * 2, BuiltinView::kHeight * 2);
        ed.openWindow(PhyzoWindow::kLibrary);                                      // opening it rescans (factory files made)
        PhyzoWindow& w = *ed.window();
        auto settle = [&] { CHECK(waitFor([&] { w.tickForTest(); return !s.busy(); })); w.tickForTest(); };
        settle();
        CHECK(fs::exists(data / "Library" / "Factory" / "Factory Bank 1.syx"));
        snapshot(ed, shots, "library");
        // Save with a name: the draft (P01, the OS's name), the footer's box, sanitized on Save.
        ed.rackCommand("SAVE");
        CHECK(waitFor([&] { w.tickForTest(); return w.frame().footer().field() != nullptr; }));
        ui::Footer& f = w.frame().footer();
        CHECK(f.text() == "Save P01 to Singles as [Factory 1].syx");
        CHECK((f.actionLabels() == juce::StringArray{"Save", "Cancel"}));
        snapshot(ed, shots, "library-save");
        if (f.field()) f.field()->setText("New/Lead");
        f.click("Save");
        settle();
        CHECK(fs::exists(data / "Library" / "Singles" / "New_Lead.syx"));
        CHECK(waitFor([&] { w.tickForTest(); return f.text() == "SAVED  Singles/New_Lead.syx"; }));
        // Compare lit (unsaved edits): the message, and nothing more.
        { std::lock_guard<std::mutex> lk(sh.mtx); sh.running.compare = true; }
        ed.rackCommand("SAVE");
        CHECK(waitFor([&] { w.tickForTest(); return f.text() == libtext::kUnsaved; }));
        CHECK(f.actionLabels().isEmpty() && f.field() == nullptr);
        { std::lock_guard<std::mutex> lk(sh.mtx); sh.running.compare = false; }
        // Esc while the box is open cancels it (nothing written).
        ed.rackCommand("SAVE");
        CHECK(waitFor([&] { w.tickForTest(); return f.field() != nullptr; }));
        CHECK(w.keyPressed(juce::KeyPress(juce::KeyPress::escapeKey)) && ed.windowOpen() && f.field() == nullptr);
        // Load a bank: the confirmation, the backup, the load, LOADED, the Memory column's title (project state).
        const juce::String bankPath = juce::String((data / "Library" / "My Bank.syx").string());
        w.message("x");
        auto* pageComp = w.page(PhyzoWindow::kLibrary);
        CHECK(pageComp != nullptr);
        // The confirmation Load... > a file asks (with the playing preset in the bank: it will change).
        CHECK(libtext::loadConfirm("My Bank", 0, -1) ==
              juce::String::fromUTF8("Load \"My Bank\" into Bank 1 (presets 1\xe2\x80\x93" "32)? Bank 1 is backed up first.").toStdString());
        CHECK(libtext::loadConfirm("My Bank", 1, 39).find("P40 will change; unsaved edits on it will be lost.") != std::string::npos);
        CHECK(libtext::loadConfirm("My Bank", 0, 39).find("will change") == std::string::npos);
        CHECK(s.loadBank(bankPath.toStdString(), 0));
        w.tickForTest();
        settle();
        CHECK(sh.sent.size() == 1 && sh.sent[0][6] == 0);
        CHECK(waitFor([&] { w.tickForTest(); return p->lastLoaded(0) == "My Bank"; }));
        CHECK(libtext::bankTitle(0, p->lastLoaded(0).toStdString()) == "My Bank" && libtext::bankTitle(1, "") == juce::String::fromUTF8("Presets 33\xe2\x80\x93" "64").toStdString());
        bool backup = false;
        for (const auto& e : fs::directory_iterator(data / "Library" / "Backups")) backup = backup || e.path().filename().string().rfind("Bank 1 ", 0) == 0;
        CHECK(backup);
        snapshot(ed, shots, "library-loaded");
        // Delete: to the Trash after the footer's question.
        CHECK(libtext::deleteConfirm("Old Lead") == "Delete Old Lead? It goes to the Trash.");
        CHECK(s.remove((data / "Library" / "Singles" / "Old Lead.syx").string()) == "Deleted Old Lead (in the Trash)");
        CHECK(fs::exists(trash / "Old Lead.syx"));
        // A preset click with no machine running: the footer says why.
        juce::String why;
        CHECK(!p->sendLibraryPreset(0, 0, why) && why == "The synth is not running");
        base.reset();
        pump(100);
        p.reset();
        fs::remove_all(data); fs::remove_all(trash);
        std::printf("window_test: LIBRARY: groups, Save with a name, Compare refused, Esc, bank load, Delete\n");
    }

    // ---- 5. Project state version 8: the last-loaded names round-trip; older versions load with none.
    {
        auto p = make();
        load(*p, projectState(8, "Mine", ""));
        CHECK(p->lastLoaded(0) == "Mine" && p->lastLoaded(1).isEmpty());
        CHECK(stateOf(*p) == projectState(8, "Mine", ""));
        p->setLastLoaded(1, "Theirs");
        auto q = make();
        load(*q, stateOf(*p));
        CHECK(q->lastLoaded(0) == "Mine" && q->lastLoaded(1) == "Theirs");
        load(*q, projectState(7));
        CHECK(q->lastLoaded(0).isEmpty() && q->lastLoaded(1).isEmpty());
        CHECK(stateOf(*q) == projectState(8));                                  // = v7 + the version + two empty names
        std::printf("window_test: state version 8 round-trips; version 7 loads with none\n");
    }

    SkinSettings::setSkin(previousSkin);
    SkinSettings::setScale(previousScale);
    SkinSettings::setSkinLog(previousLog);
    SkinSettings::setMidiToHost(previousHost);
    std::printf("window_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
