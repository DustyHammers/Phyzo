// Skin runtime test (no ROMs, no images, no window): loads the text-only test skin with a no-op renderer and a
// recording fake synth, then drives every custom element and the Lua API.
#include <RmlUi/Core.h>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "skin_discovery.h"
#include "skin_elements.h"
#include "skin_system.h"
#include "skin_view.h"

using namespace skin;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { std::printf("FAIL %s:%d: %s == %s\n  got: %s\n", __FILE__, __LINE__, #a, #b, toText(va).c_str()); ++failures; } } while (0)
std::string toText(const std::string& s) { return s; }
std::string toText(int v) { return std::to_string(v); }
std::string toText(size_t v) { return std::to_string(v); }
std::string toText(bool v) { return v ? "true" : "false"; }

const std::string kSkins = PHYZO_TEST_SKINS;

class FakePort : public PanelPort {
public:
    std::vector<std::string> sent;
    std::map<std::string, int> knobs;
    std::array<int, 26> positions{};
    std::array<LedReport, 256> ledReports{};
    std::array<uint8_t, 4> segs{};
    FakePort() { positions.fill(512); positions[11] = 1023; positions[3] = 2; }   // as F-02's fresh values

    void sendControl(int cc, int raw) override { positions[size_t(cc)] = raw; sent.push_back("cc " + std::to_string(cc) + " " + std::to_string(raw)); }
    int controlPosition(int cc) override { return positions[size_t(cc)]; }
    void sendButton(int raw, bool down) override {
        char b[32]; std::snprintf(b, sizeof b, "%s %02X", down ? "81" : "80", raw); sent.push_back(b);
    }
    LedReport led(int code) override { return ledReports[size_t(code)]; }
    std::array<uint8_t, 4> segments() override { return segs; }
    bool knobPosition(const std::string& id, int& raw) override {
        auto it = knobs.find(id); if (it == knobs.end()) return false; raw = it->second; return true;
    }
    void setKnobPosition(const std::string& id, int raw) override { knobs[id] = raw; }
    std::string last() const { return sent.empty() ? std::string() : sent.back(); }
};

// The editor unit as the plugin's controller would show it; records the clicks and the Follow LEDs.
class FakeEditor : public EditorPort {
public:
    EditorState st;
    bool open = true;
    std::vector<std::string> commands;
    std::array<int, 4> sound{}; std::array<int, 2> osc{}; std::array<int, 3> env{};
    int noise = 0, lfo = 0, filterKbd = 0;
    std::array<std::string, 5> knobLabels;
    bool editorOpen() override { return open; }
    uint32_t editorSerial() override { return st.serial; }
    void editorState(EditorState& out) override { out = st; }
    void editorCommand(const std::string& a, const std::string& t) override { commands.push_back(a + " " + t); }
    void editorFollowLeds(const std::array<int, 4>& s, const std::array<int, 2>& o, const std::array<int, 3>& e) override { sound = s; osc = o; env = e; }
    void editorFollowExtraLeds(int n, int l, int f) override { noise = n; lfo = l; filterKbd = f; }
    void editorKnobLabels(const std::array<std::string, 5>& k) override { knobLabels = k; }
};
class EditorFakePort : public FakePort {
public:
    FakeEditor ed;
    EditorPort* editor() override { return &ed; }
};

Rml::Element* byId(SkinView& v, const char* id) {
    for (int i = 0; i < v.context()->GetNumDocuments(); ++i)
        if (Rml::Element* e = v.context()->GetDocument(i)->GetElementById(id)) return e;
    return nullptr;
}

// The panel document (the skin may also have its editor unit's document).
Rml::ElementDocument* panelDoc(SkinView& v) {
    for (int i = 0; i < v.context()->GetNumDocuments(); ++i)
        if (v.context()->GetDocument(i)->GetElementById("vol")) return v.context()->GetDocument(i);
    return v.context()->GetNumDocuments() ? v.context()->GetDocument(0) : nullptr;
}
template <class T> T* get(SkinView& v, const char* id) { return dynamic_cast<T*>(byId(v, id)); }
bool hasClass(SkinView& v, const char* id, const char* cls) {
    Rml::Element* e = byId(v, id);
    return e && e->IsClassSet(cls);
}
std::string sprite(SkinView& v, const char* id) { auto* e = get<SpriteElement>(v, id); return e ? e->shownSprite() : "(missing)"; }
void click(SkinView& v, int x, int y, int mods = 0) { v.mouseMove(x, y, 0); v.mouseDown(0, mods); v.mouseUp(0, mods); }
}  // namespace

int main() {
    // Discovery: the test skin is found by its folder name.
    {
        const auto skins = discoverSkins(kSkins);
        CHECK(skins.size() == 1 && skins[0].name == "test");
        CHECK(discoverSkins(kSkins + "/does-not-exist").empty());
    }

    FakePort port;
    {
        SkinView v(port, {"Phyzo", "DHammers", "9.9.9"}, nullptr);
        v.setViewport(1795, 848, 1.0f);
        std::string err;
        CHECK(v.load(kSkins + "/test", "test", err));
        if (!v.loaded()) { std::printf("load error: %s\n", err.c_str()); for (auto& l : v.log()) std::printf("  %s\n", l.c_str()); return 1; }
        double t = 10.0;
        v.update(t);

        // Document events and plugin info.
        CHECK_EQ(v.evalForTest("events[1]"), std::string("load"));
        CHECK_EQ(v.evalForTest("events[2]"), std::string("plugin Phyzo DHammers 9.9.9"));
        CHECK_EQ(v.evalForTest("shownCount"), std::string("1"));               // element onshow
        CHECK_EQ(v.evalForTest("frames"), std::string("1"));
        v.update(t + 0.005);                                                     // frame events capped at 60 Hz
        CHECK_EQ(v.evalForTest("frames"), std::string("1"));
        v.update(t += 0.02);
        CHECK_EQ(v.evalForTest("frames"), std::string("2"));
        bool logged = false;
        for (auto& l : v.log()) logged |= l.find("test skin loaded") != std::string::npos;
        CHECK(logged);                                                           // Log.Message

        // Knob: initial frame from the cc position (1023 -> last of 61 frames), nothing sent.
        CHECK_EQ(sprite(v, "vol"), std::string("image(knob_060)"));
        CHECK(port.sent.empty());
        // Vertical drag: 64 px down at ratio 1 = 64 dp = -256 raw. One move, one message.
        v.mouseMove(50, 50, 0); v.mouseDown(0, 0); v.mouseMove(50, 114, 0); v.mouseUp(0, 0);
        CHECK_EQ(port.last(), std::string("cc 11 767"));
        CHECK_EQ(port.sent.size(), size_t(1));
        CHECK_EQ(port.knobs["vol"], 767);
        CHECK_EQ(sprite(v, "vol"), std::string("image(knob_045)"));            // round(767 * 60 / 1023)
        CHECK_EQ(v.evalForTest("lastEvent()"), std::string("knob11 767"));
        // Shift = fine (a tenth): 100 px down = -40.
        v.mouseMove(50, 50, ModShift); v.mouseDown(0, ModShift); v.mouseMove(50, 150, ModShift); v.mouseUp(0, ModShift);
        CHECK_EQ(port.last(), std::string("cc 11 727"));
        // Mouse wheel: one notch up = +16, with Shift +1.
        v.mouseMove(50, 50, 0); v.mouseWheel(-1, 0);
        CHECK_EQ(port.last(), std::string("cc 11 743"));
        v.mouseWheel(-1, ModShift);
        CHECK_EQ(port.last(), std::string("cc 11 744"));
        // dp ratio 2 (zoom x display scale): the same 64 dp drag is 128 px.
        v.setViewport(3590, 1696, 2.0f);
        v.update(t += 0.02);
        v.mouseMove(100, 100, 0); v.mouseDown(0, 0); v.mouseMove(100, 228, 0); v.mouseUp(0, 0);
        CHECK_EQ(port.last(), std::string("cc 11 488"));
        v.setViewport(1795, 848, 1.0f);
        v.update(t += 0.02);

        // Stacked knobs share cc 3; each keeps its own position.
        v.mouseMove(250, 50, 0); v.mouseDown(0, 0); v.mouseMove(250, 0, 0); v.mouseUp(0, 0);
        CHECK_EQ(port.last(), std::string("cc 3 202"));                        // 2 + 50 dp x 4
        CHECK_EQ(port.knobs["envA"], 202);
        get<Rml::Element>(v, "envA")->SetClass("hidden", true);
        get<Rml::Element>(v, "envB")->SetClass("hidden", false);
        v.update(t += 0.02);
        CHECK_EQ(sprite(v, "envB"), std::string("image(knob_000)"));           // its own (fresh) position
        v.mouseMove(250, 50, 0); v.mouseDown(0, 0); v.mouseMove(250, 25, 0); v.mouseUp(0, 0);
        CHECK_EQ(port.last(), std::string("cc 3 102"));
        CHECK_EQ(port.knobs["envA"], 202);
        CHECK_EQ(port.knobs["envB"], 102);
        get<Rml::Element>(v, "envB")->SetClass("hidden", true);
        get<Rml::Element>(v, "envA")->SetClass("hidden", false);
        v.update(t += 0.02);
        CHECK_EQ(sprite(v, "envA"), std::string("image(knob_012)"));           // round(202 * 60 / 1023)
        port.sent.clear();

        // Panel button: 81 on press, 80 on release, class pressed while down; onButton fires.
        v.mouseMove(120, 210, 0); v.mouseDown(0, 0);
        CHECK_EQ(port.last(), std::string("81 13"));
        CHECK(hasClass(v, "bPlain", "pressed"));
        CHECK_EQ(v.evalForTest("lastEvent()"), std::string("button13 true"));
        v.mouseMove(600, 600, 0); v.mouseUp(0, 0);                              // released outside the button
        CHECK_EQ(port.last(), std::string("80 13"));
        CHECK(!hasClass(v, "bPlain", "pressed"));
        CHECK_EQ(v.evalForTest("lastEvent()"), std::string("button13 false"));
        // Right-click never presses.
        port.sent.clear();
        v.mouseMove(120, 210, 0); v.mouseDown(1, 0); v.mouseUp(1, 0);
        CHECK(port.sent.empty());
        // Alt-click latches; another click releases; Esc releases all.
        click(v, 120, 210, ModAlt);
        CHECK_EQ(port.sent.size(), size_t(1));
        CHECK_EQ(port.last(), std::string("81 13"));
        CHECK(hasClass(v, "bPlain", "held"));
        CHECK_EQ(v.evalForTest("panel.isHeld(0x13)"), std::string("true"));
        click(v, 120, 210);
        CHECK_EQ(port.last(), std::string("80 13"));
        CHECK(!hasClass(v, "bPlain", "held"));
        click(v, 120, 210, ModAlt);
        click(v, 20, 210, ModAlt);
        CHECK_EQ(port.last(), std::string("81 12"));
        port.sent.clear();
        v.releaseHeldButtons();
        CHECK_EQ(port.sent.size(), size_t(2));
        CHECK(!hasClass(v, "bEnv", "held") && !hasClass(v, "bPlain", "held"));

        // LEDs: on, flash (2.5 Hz, 50 %), beat flash (min(100 ms, half the interval)), audioclip unlit.
        port.ledReports[0x21].mode = LedMode::On;
        port.ledReports[0x20].mode = LedMode::On;
        port.ledReports[0x22].mode = LedMode::Flash;
        v.update(t = 100.05);                                                    // flash phase 0.05 s of 0.4 s: lit
        CHECK(hasClass(v, "ledOn", "lit"));
        CHECK(hasClass(v, "bEnv", "lit"));
        CHECK(!hasClass(v, "bPlain", "lit"));
        CHECK(hasClass(v, "ledFlash", "lit"));
        CHECK(!hasClass(v, "clip", "lit"));
        CHECK_EQ(v.evalForTest("hasEvent('led21 1')"), std::string("true"));
        CHECK_EQ(v.evalForTest("panel.led(0x22)"), std::string("2"));
        v.update(t = 100.25);                                                    // phase 0.25: unlit
        CHECK(!hasClass(v, "ledFlash", "lit"));
        port.ledReports[0x23] = {LedMode::Beat, 1, 0};
        v.update(t = 200.0);
        CHECK(hasClass(v, "ledBeat", "lit"));
        CHECK_EQ(v.evalForTest("panel.led(0x23)"), std::string("2"));
        v.update(t = 200.09);
        CHECK(hasClass(v, "ledBeat", "lit"));
        v.update(t = 200.11);                                                    // 100 ms passed
        CHECK(!hasClass(v, "ledBeat", "lit"));
        port.ledReports[0x23] = {LedMode::Beat, 2, 120};                         // 120 ms interval: lit 60 ms
        v.update(t = 201.0);
        CHECK(hasClass(v, "ledBeat", "lit"));
        v.update(t = 201.05);
        CHECK(hasClass(v, "ledBeat", "lit"));
        v.update(t = 201.07);
        CHECK(!hasClass(v, "ledBeat", "lit"));
        CHECK_EQ(v.evalForTest("(function() local n = 0 for _, e in ipairs(events) do if e == 'beat23' then n = n + 1 end end return n end)()"), std::string("2"));
        port.ledReports[0x21].mode = LedMode::Off;
        v.update(t += 0.02);
        CHECK(!hasClass(v, "ledOn", "lit"));
        CHECK_EQ(v.evalForTest("lastEvent()"), std::string("led21 0"));

        // Display: sprite = segment byte & 0x7F, pos 0 leftmost; onDisplay and panel.display().
        port.segs = {0x3D, 0x00, 0x7E, 0x86};
        v.update(t += 0.02);
        CHECK_EQ(sprite(v, "d0"), std::string("image(vfd_061)"));
        CHECK_EQ(sprite(v, "d1"), std::string("image(vfd_000)"));
        CHECK_EQ(sprite(v, "d2"), std::string("image(vfd_126)"));
        CHECK_EQ(sprite(v, "d3"), std::string("image(vfd_006)"));
        CHECK_EQ(v.evalForTest("lastEvent()"), std::string("display 3D 00 7E 86"));
        CHECK_EQ(v.evalForTest("select(4, panel.display())"), std::string("134"));

        // Lua API: knob, setKnob (moves the visible knob of that cc), press/release.
        CHECK_EQ(v.evalForTest("panel.knob(11)"), std::string("488"));
        port.sent.clear();
        v.evalForTest("panel.setKnob(11, 100)");
        CHECK_EQ(port.last(), std::string("cc 11 100"));
        v.update(t += 0.02);
        CHECK_EQ(sprite(v, "vol"), std::string("image(knob_006)"));
        CHECK_EQ(port.knobs["vol"], 100);
        CHECK_EQ(v.evalForTest("lastEvent()"), std::string("knob11 100"));
        v.evalForTest("panel.press(0x40)");
        CHECK_EQ(port.last(), std::string("81 40"));
        v.evalForTest("panel.release(0x40)");
        CHECK_EQ(port.last(), std::string("80 40"));
        CHECK(v.evalForTest("panel.setKnob(30, 1)").rfind("error:", 0) == 0);  // checked arguments

        // Sandbox: only base, coroutine, table, string, math, utf8; no file access; text chunks only.
        CHECK_EQ(v.evalForTest("io == nil and os == nil and debug == nil and package == nil and require == nil"), std::string("true"));
        CHECK_EQ(v.evalForTest("dofile == nil and loadfile == nil"), std::string("true"));
        CHECK_EQ(v.evalForTest("type(coroutine.wrap) .. type(table.concat) .. type(string.format) .. type(math.floor) .. type(utf8.char)"),
                 std::string("functionfunctionfunctionfunctionfunction"));
        CHECK_EQ(v.evalForTest("load('return 1 + 1')()"), std::string("2"));
        CHECK_EQ(v.evalForTest("select(2, load(string.dump(function() end)))"), std::string("attempt to load a binary chunk (mode is 't')"));

        // Errors are logged, never thrown: a failing callback and the 3 s time limit.
        v.evalForTest("panel.onDisplay(function() error('boom') end)");
        port.segs = {1, 2, 3, 4};
        v.update(t += 0.02);
        bool boom = false;
        for (auto& l : v.log()) boom |= l.find("boom") != std::string::npos;
        CHECK(boom);
        const auto t0 = std::chrono::steady_clock::now();
        const std::string r = v.evalForTest("while true do end");
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(r.find("longer than 3 s") != std::string::npos);
        CHECK(secs > 2.5 && secs < 6);
        v.evalForTest("panel.onDisplay(function() while true do end end)");
        port.segs = {5, 6, 7, 8};
        v.update(t += 0.02);
        CHECK_EQ(sprite(v, "d3"), std::string("image(vfd_008)"));               // still working afterwards

        // Files only from the skin folder, by plain name.
        const Rml::FileHandle ok = Rml::GetFileInterface()->Open(kSkins + "/test/../test/test.lua");
        CHECK(ok != 0);
        if (ok) Rml::GetFileInterface()->Close(ok);
        CHECK(Rml::GetFileInterface()->Open(kSkins + "/../skin_test.cpp") == 0);

        // A second view (another plugin instance) has its own Lua globals and its own synth.
        FakePort port2;
        {
            SkinView w(port2, {"Phyzo", "DHammers", "9.9.9"}, nullptr);
            w.setViewport(1795, 848, 1.0f);
            CHECK(w.load(kSkins + "/test", "test", err));
            w.update(t);
            v.evalForTest("mine = 'v'");
            CHECK_EQ(w.evalForTest("mine"), std::string("nil"));
            CHECK_EQ(w.evalForTest("#events"), std::string("4"));             // load, plugin, editor load, the first onDisplay
            CHECK_EQ(w.evalForTest("events[4]"), std::string("display 00 00 00 00"));
            w.evalForTest("panel.press(0x55)");
            CHECK_EQ(port2.last(), std::string("81 55"));
            CHECK(port.last() != "81 55");
            CHECK_EQ(v.evalForTest("mine"), std::string("v"));
        }
        v.update(t += 0.02);                                                     // still fine after the other closed

        // Reload (F5): a fresh environment, positions kept.
        CHECK(v.load(kSkins + "/test", "test", err));
        v.update(t += 0.02);
        CHECK_EQ(v.evalForTest("#events"), std::string("4"));
        CHECK_EQ(v.evalForTest("mine"), std::string("nil"));
        CHECK_EQ(sprite(v, "vol"), std::string("image(knob_006)"));

        // An empty or malformed document is a failed load (the plugin then falls back to another skin).
        {
            const std::string tmp = (std::filesystem::temp_directory_path() / "phyzo_skin_test").string();
            std::filesystem::remove_all(tmp);
            std::filesystem::create_directories(tmp + "/empty");
            std::filesystem::create_directories(tmp + "/broken");
            std::ofstream(tmp + "/empty/empty.rml").close();
            std::ofstream(tmp + "/broken/broken.rml") << "<rml><body><div>x</body>\n";
            CHECK(!v.load(tmp + "/empty", "empty", err));
            CHECK(err.find("has no content") != std::string::npos);
            CHECK(!v.load(tmp + "/broken", "broken", err));
            CHECK(err.find("parse error") != std::string::npos);
            std::filesystem::remove_all(tmp);
        }

        // A missing skin is reported, not crashed on.
        CHECK(!v.load(kSkins + "/test", "nothere", err));
        CHECK(err.find("nothere.rml is missing") != std::string::npos);
        CHECK(!v.loaded());
        v.update(t += 0.02);
    }
    // The last view released RmlUi and Lua; a new one starts them again.
    {
        SkinView v(port, {"Phyzo", "DHammers", "9.9.9"}, nullptr);
        std::string err;
        CHECK(v.load(kSkins + "/test", "test", err));
        v.update(1.0);
        CHECK_EQ(v.evalForTest("events[1]"), std::string("load"));
    }

    // UI batching (B-11): the panel is read once per frame, latest state wins; Lua hears only real changes; a still
    // panel needs no frames.
    {
        FakePort p2;
        SkinView v(p2, {"Phyzo", "DHammers", "9.9.9"}, nullptr);
        std::string err;
        CHECK(v.load(kSkins + "/test", "test", err));
        Rml::ElementDocument* doc = panelDoc(v);
        { ViewScope scope(&v); doc->RemoveAttribute("onframe"); }                  // no frame handler
        double t = 50.0;
        v.update(t);
        v.update(t += 0.02);
        CHECK(std::isinf(v.nextWake()));                                         // nothing to animate: no frames
        // An empty or blank onframe is no handler: no frame events, no frames.
        for (const char* blank : {"", "  \t "}) {
            const auto luaBlank = v.stats().luaCalls;
            { ViewScope scope(&v); doc->SetAttribute("onframe", Rml::String(blank)); }
            v.update(t += 0.02);
            v.update(t += 0.02);
            CHECK(std::isinf(v.nextWake()));
            CHECK_EQ(int(v.stats().luaCalls - luaBlank), 0);
        }
        { ViewScope scope(&v); doc->RemoveAttribute("onframe"); }
        v.update(t += 0.02);
        auto count = [&](const std::string& e) { return v.evalForTest("(function() local n = 0 for _, x in ipairs(events) do if x == '" + e + "' then n = n + 1 end end return n end)()"); };
        // 120 LED messages between two frames (on/off/on ...), ending on: one onLed call with the final state.
        const auto lua0 = v.stats().luaCalls;
        for (int i = 0; i < 120; ++i) p2.ledReports[0x21].mode = (i % 2) ? LedMode::Off : LedMode::On;
        p2.ledReports[0x21].mode = LedMode::On;
        v.update(t += 0.02);
        CHECK_EQ(count("led21 1"), std::string("1"));
        CHECK_EQ(int(v.stats().luaCalls - lua0), 1);
        v.update(t += 0.02);                                                       // unchanged: no call, no change
        CHECK_EQ(count("led21 1"), std::string("1"));
        const auto changed0 = v.stats().changedFrames;
        v.update(t += 0.02);
        CHECK_EQ(int(v.stats().changedFrames - changed0), 0);
        CHECK(std::isinf(v.nextWake()));
        // A flashing LED wakes the view at its next toggle (2.5 Hz: every 0.2 s), not every frame.
        p2.ledReports[0x22].mode = LedMode::Flash;
        v.update(t = 60.05);
        CHECK(std::fabs(v.nextWake() - 60.2) < 1e-9);
        p2.ledReports[0x22].mode = LedMode::Off;
        v.update(t += 0.02);
        CHECK(std::isinf(v.nextWake()));
        // panel.setKnob with the value already sent: no message, no onKnob.
        v.evalForTest("panel.setKnob(11, 600)");
        v.evalForTest("panel.setKnob(11, 600)");
        CHECK_EQ(count("knob11 600"), std::string("1"));
        CHECK_EQ(int(std::count(p2.sent.begin(), p2.sent.end(), std::string("cc 11 600"))), 1);
        // panel.press twice without a release: one press edge.
        v.evalForTest("panel.press(0x13)"); v.evalForTest("panel.press(0x13)"); v.evalForTest("panel.release(0x13)");
        CHECK_EQ(count("button13 true"), std::string("1"));
        CHECK_EQ(count("button13 false"), std::string("1"));
        // The display: one onDisplay per change, however often it was rewritten in between.
        p2.segs = {1, 2, 3, 4};
        v.update(t += 0.02);
        v.update(t += 0.02);
        CHECK_EQ(count("display 01 02 03 04"), std::string("1"));
    }


    // The editor unit (F-30): <name>-editor.rml below the body, the glass rows, the legends, the Lua editor API.
    {
        EditorFakePort p3;
        SkinView v(p3, {"Phyzo", "DHammers", "9.9.9"}, nullptr);
        v.setViewport(1795, 848 + 394, 1.0f);
        std::string err;
        CHECK(v.load(kSkins + "/test", "test", err));
        CHECK(v.hasEditor() && !v.editorVisible());
        CHECK_EQ(v.evalForTest("hasEvent('editor load')"), std::string("true"));
        { ViewScope scope(&v); panelDoc(v)->RemoveAttribute("onframe"); }      // only the editor's own wake-ups below
        CHECK(p3.ed.sound[0] == 0x0B && p3.ed.sound[3] == 0x07 && p3.ed.osc[1] == 0x15 && p3.ed.env[1] == 0x0C);
        CHECK(p3.ed.noise == 0x02 && p3.ed.lfo == 0x0F && p3.ed.filterKbd == 0x08);
        CHECK((p3.ed.knobLabels == std::array<std::string, 5>{"P", "H", "Y", "Z", "O"}));   // editor.knobLabels in onload
        CHECK(v.evalForTest("editor.knobLabels{'A', 'B'}").find("exactly 5") != std::string::npos);
        CHECK(v.evalForTest("editor.knobLabels{'A', 'B', 'C', 'D', 'LONGER'}").find("longer than 4") != std::string::npos);
        CHECK(v.evalForTest("editor.knobLabels{'A', 'B', 'C', 'D', 5}").find("not a string") != std::string::npos);
        CHECK(p3.ed.knobLabels[0] == "P");                                          // a refused call changes nothing
        double t = 70.0;
        EditorState& st = p3.ed.st;
        st.rows = {std::string("FILTER CUTOFF") + std::string(32, ' ') + "64   ", std::string(50, ' '), std::string(50, ' '), std::string(50, ' ')};
        for (int c = 45; c < 47; ++c) st.underline[0][size_t(c)] = true;
        st.area = "SOUND"; st.section = "FILTER"; st.areas = {"SOUND", "OSC"}; st.sections = {"FILTER", "WAVE"};
        st.soundTargets = true; st.sound = 1; st.page = 2; st.pages = 3; st.next = true; st.follow = false; st.data = true;
        st.serial = 1;
        v.update(t);
        auto* r0 = dynamic_cast<VfdRowElement*>(byId(v, "r0"));
        CHECK(r0 && r0->shownChar(0) == 0);                                      // hidden: nothing applied
        v.setEditorVisible(true);
        v.update(t += 0.02);
        CHECK(r0 && r0->shownChar(0) == 'F' && r0->shownChar(45) == '6');
        CHECK(r0 && r0->GetNumChildren() == 50);
        Rml::Element* cell0 = r0 ? r0->GetChild(0) : nullptr;
        Rml::Element* cell13 = r0 ? r0->GetChild(13) : nullptr;
        CHECK(cell0 && !cell0->IsClassSet("ghost") && cell13 && cell13->IsClassSet("ghost"));
        const Rml::Property* deco = cell0 ? cell0->GetChild(0)->GetLocalProperty("decorator") : nullptr;
        CHECK(deco && deco->ToString().find("chr_070") != std::string::npos);    // 'F' = 70
        CHECK(r0 && r0->GetChild(45)->GetChild(1)->IsClassSet("lit") && !r0->GetChild(44)->GetChild(1)->IsClassSet("lit"));
        auto lit = [&](const char* id) { Rml::Element* e = byId(v, id); return e && e->IsClassSet("lit"); };
        auto avail = [&](const char* id) { Rml::Element* e = byId(v, id); return e && e->IsClassSet("avail"); };
        CHECK(lit("lSound") && avail("lSound") && !avail("lReverb") && !lit("lReverb"));
        CHECK(lit("lFilter") && lit("lS2") && lit("lP2") && avail("lP2") && !avail("lP5"));
        CHECK(lit("lNext") && !lit("lFollow") && avail("lFollow") && lit("lData"));
        CHECK_EQ(v.evalForTest("lastEvent()"), std::string("editor SOUND FILTER 2/3"));
        CHECK_EQ(v.evalForTest("editor.state().rows[1]:sub(1, 13)"), std::string("FILTER CUTOFF"));
        CHECK_EQ(v.evalForTest("editor.isOpen()"), std::string("true"));
        // Clicks on legends (the editor unit sits 848 dp down): to the editor; Lua can click too.
        click(v, 388 + 10, 848 + 55 + 5);
        click(v, 1458 + 10, 848 + 55 + 5);
        click(v, 1601 + 10, 848 + 55 + 5);                                       // an indicator: not a control
        v.evalForTest("editor.click('page', '3')");
        CHECK((p3.ed.commands == std::vector<std::string>{"area SOUND", "toggle_follow FOLLOW", "page 3"}));
        // The library controls (v0.8.3 step 2): available on any page once there is a library; LIBRARY lit while the
        // open editor shows the library, the actions never lit; a click sends action and value.
        CHECK(!avail("lSave") && !avail("lLibrary") && !avail("lLibMain"));   // no library
        st.areas.insert("LIBRARY"); st.open = true; ++st.serial;
        v.update(t += 0.02);
        CHECK(avail("lSave") && avail("lExport") && avail("lLibrary") && !lit("lLibrary") && !lit("lSave"));   // on SOUND
        CHECK(avail("lLibMain") && !lit("lLibMain"));
        st.area = "LIBRARY"; st.section = "SOURCES"; st.sections.clear(); ++st.serial;
        v.update(t += 0.02);
        CHECK(lit("lLibrary") && lit("lLibMain") && !lit("lSave"));
        p3.ed.commands.clear();
        click(v, 400 + 10, 848 + 91 + 5);
        click(v, 460 + 10, 848 + 91 + 5);
        click(v, 1600 + 10, 600 + 5);                                            // the main document's LIBRARY
        CHECK((p3.ed.commands == std::vector<std::string>{"library SAVE", "library EXPORT", "library LIBRARY"}));
        st.area = "SOUND"; st.section = "FILTER"; st.areas.erase("LIBRARY"); st.sections = {"FILTER", "WAVE"}; st.open = false; ++st.serial;
        v.update(t += 0.02);
        p3.ed.commands.clear();
        // Values on the glass (spans from the editor): drag (1 step per 4 dp, Shift per 16 dp), double-click for typed
        // entry, a name to pick; right-click asks which value is under the mouse. The wheel never changes a value.
        st.rows[1] = "01  Big Pad" + std::string(39, ' ');
        st.spans = {{0, 45, 2, 7, false}, {1, 0, 20, 9, true}};
        ++st.serial;
        v.update(t += 0.02);
        p3.ed.commands.clear();
        const int x45 = 149 + 45 * 30 + 5, y0 = 848 + 118 + 10, y1 = 848 + 173 + 10;
        v.mouseMove(x45, y0, 0); v.mouseDown(0, 0);
        v.mouseMove(x45, y0 - 9, 0);
        v.mouseMove(x45, y0 - 10, 0);                                            // still 2 steps: nothing new
        v.mouseMove(x45, y0 + 4, 0);
        v.mouseUp(0, 0);
        CHECK((p3.ed.commands == std::vector<std::string>{"drag_begin 7", "drag 2", "drag -1", "drag_end "}));
        p3.ed.commands.clear();
        v.mouseMove(x45, y0, 0); v.mouseDown(0, ModShift);
        v.mouseMove(x45, y0 - 20, ModShift);
        v.mouseUp(0, 0);
        v.mouseMove(x45, y0, 0); v.mouseDown(0, 0, 2); v.mouseUp(0, 0);        // the second press of a double-click
        v.mouseMove(x45, y0, 0); v.mouseWheel(3, 0);
        click(v, 149 + 3 * 30 + 5, y1);
        click(v, 149 + 30 * 30 + 5, y1);                                         // no span there
        CHECK((p3.ed.commands == std::vector<std::string>{"drag_begin 7", "drag 1", "drag_end ", "type 7", "pick 9"}));
        CHECK(v.editorValueAt(x45, y0) == 7 && v.editorValueAt(149 + 3 * 30 + 5, y1) == -1 && v.editorValueAt(20, y0) == -1);
        CHECK(v.editorValueAt(x45, 300) == -1);                                  // the panel above
        // The serial unchanged: nothing re-applied, no Lua call.
        const auto lua0 = v.stats().luaCalls;
        v.update(t += 0.02);
        CHECK_EQ(int(v.stats().luaCalls - lua0), 0);
        // A focused value blinks at 2.5 Hz (wake-ups, not continuous frames).
        st.focusRow = 0; st.focusCol = 45; st.focusLen = 2; ++st.serial;
        v.update(t = 80.05);
        CHECK(r0 && r0->GetChild(45)->IsClassSet("focus") && !r0->GetChild(44)->IsClassSet("focus"));
        CHECK(std::fabs(v.nextWake() - 80.2) < 1e-9);
        v.update(t = 80.25);
        CHECK(r0 && r0->GetChild(45)->IsClassSet("blink"));
        v.update(t = 80.45);
        CHECK(r0 && !r0->GetChild(45)->IsClassSet("blink"));
        st.focusRow = -1; ++st.serial;
        v.update(t += 0.02);
        v.update(t += 0.02);
        CHECK(std::isinf(v.nextWake()));
        v.setEditorVisible(false);
        CHECK(!v.editorVisible());
        // With the editor unit closed, the main document's library control still works: avail, lit, clicks.
        st.areas.insert("LIBRARY"); st.area = "LIBRARY"; st.open = false; ++st.serial;
        v.update(t += 0.02);
        CHECK(avail("lLibMain") && !lit("lLibMain"));                           // closed: the library is not shown
        p3.ed.commands.clear();
        click(v, 1600 + 10, 600 + 5);
        CHECK((p3.ed.commands == std::vector<std::string>{"library LIBRARY"}));  // the plugin opens the editor on it
        st.open = true; ++st.serial;
        v.update(t += 0.02);
        CHECK(lit("lLibMain"));
    }
    // A broken editor file: logged; the panel still loads, without an editor unit.
    {
        const auto tmp = std::filesystem::temp_directory_path() / "phyzo_skin_editor_test" / "brk";
        std::filesystem::remove_all(tmp.parent_path());
        std::filesystem::create_directories(tmp);
        std::ofstream(tmp / "brk.rml") << "<rml><body><led id=\"a\" code=\"1\"/></body></rml>";
        std::ofstream(tmp / "brk-editor.rml") << "<rml><body><vfdrow row=\"0\"></body>";
        EditorFakePort p4;
        SkinView v(p4, {"Phyzo", "DHammers", "9.9.9"}, nullptr);
        std::string err;
        CHECK(v.load(tmp.u8string(), "brk", err));
        CHECK(!v.hasEditor());
        bool warned = false;
        for (const auto& l : v.log()) warned |= l.find("brk-editor.rml could not be read") != std::string::npos;
        CHECK(warned);
        std::filesystem::remove_all(tmp.parent_path());
    }
    // legendState: the rules for every legend kind.
    {
        EditorState s;
        s.areas = {"OSC"}; s.area = "OSC"; s.oscTargets = true; s.soundTargets = true; s.osc = 1; s.pages = 2; s.page = 1;
        bool l, a;
        legendState(s, "target", "O2", l, a); CHECK(l && a);
        legendState(s, "target", "O1", l, a); CHECK(!l && a);
        s.oscBoth = true;                                                 // both oscillators edited: both lit
        legendState(s, "target", "O1", l, a); CHECK(l && a);
        legendState(s, "target", "O2", l, a); CHECK(l && a);
        s.oscTargets = false;
        legendState(s, "target", "O2", l, a); CHECK(!l && !a);
        legendState(s, "page", "3", l, a); CHECK(!l && !a);
        legendState(s, "page_prev", "", l, a); CHECK(!l && !a);
        legendState(s, "label", "PAGE", l, a); CHECK(l && !a);
        legendState(s, "area", "ARP", l, a); CHECK(!l && !a);
        legendState(s, "library", "SAVE", l, a); CHECK(!l && !a);       // LIBRARY's actions: only in that area
        s.areas.insert("LIBRARY"); s.area = "LIBRARY";
        legendState(s, "library", "FOLDER", l, a); CHECK(!l && a);
        legendState(s, "area", "LIBRARY", l, a); CHECK(l && a);
    }

    std::printf("skin_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
