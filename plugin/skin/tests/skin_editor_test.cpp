// The editor unit end to end, without ROMs (docs/editor/ROUND2_REVIEW.md): the test skin's editor document in the skin
// runtime, the editor controller behind it (the same adapter as the plugin's), and a fake engine link standing in
// for the OS. A mouse click on a preset's name on the glass goes through SkinView into the controller and out as the
// preset's program change, and the current preset is marked: the focused item, its cells blinking, never underlined.
#include <RmlUi/Core.h>
#include <cstdio>
#include <string>
#include <vector>
#include "editor/editor_controller.h"
#include "skin_elements.h"
#include "skin_view.h"

using namespace skin;
using Bytes = std::vector<uint8_t>;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

const std::string kSkins = PHYZO_TEST_SKINS;

// The OS as far as the presets page needs it: stored preset names and the edit buffer's (the current preset's) name.
struct FakeLink : editor::Link {
    std::vector<Bytes> sent, replies;
    std::string current = "Preset 03";
    void setOpen(bool) override {}
    bool send(const Bytes& m) override {
        sent.push_back(m);
        if (m.size() > 20 && m[5] == edproto::kPartialRead && m[6] == edproto::kPreset) {
            edproto::Address a;
            a.type = m[6]; a.number = uint16_t(edproto::get14(&m[7])); a.subtype = m[9];
            a.subnumber = uint16_t(edproto::get14(&m[10])); a.offset = edproto::get32(&m[12]); a.length = edproto::get32(&m[17]);
            std::string name = a.number >= edproto::kStoredPreset ? "Preset " + std::string(a.number - 0x100 < 9 ? "0" : "") +
                                                                        std::to_string(a.number - 0x100 + 1)
                                                                  : current;
            name.resize(16, ' ');
            Bytes r = edproto::partialWrite(a, Bytes(name.begin(), name.end()));
            r[4] = 0; r[5] = edproto::kPartialReply;
            replies.push_back(r);
        }
        return true;
    }
    void setStandingRead(const std::vector<Bytes>&) override {}
    bool popReply(Bytes& m) override { if (replies.empty()) return false; m = replies.front(); replies.erase(replies.begin()); return true; }
    uint64_t ledStateMessages() const override { return 0; }
    uint64_t presetTriggers() const override { return 0; }
    uint64_t panelGestures() const override { return 0; }
    bool panelButtonHeld() const override { return false; }
    int ledMode(int) const override { return 0; }
    std::array<char, 4> display() const override { return {{'P', ' ', '0', '3'}}; }
    bool running() const override { return true; }
};

// The skin's view of the controller, as the plugin's EditorPort adapter does it.
struct Adapter : EditorPort {
    editor::Controller& c;
    explicit Adapter(editor::Controller& ctl) : c(ctl) {}
    bool editorOpen() override { return c.isOpen(); }
    uint32_t editorSerial() override { return c.serial(); }
    void editorState(EditorState& out) override {
        const editor::View v = c.view();
        for (int r = 0; r < EditorState::kRows; ++r) {
            out.rows[size_t(r)] = v.screen.rowText(r);
            out.underline[size_t(r)] = v.screen.underline[size_t(r)];
            out.dot[size_t(r)] = v.screen.dot[size_t(r)];
        }
        out.focusRow = v.focusRow; out.focusCol = v.focusCol; out.focusLen = v.focusLen;
        out.spans.clear();
        for (const auto& s : v.screen.values) out.spans.push_back({s.row, s.col, s.len, s.ref, s.pick});
        out.area = v.area; out.section = v.section; out.page = v.page; out.pages = v.pages;
        out.areas = v.areas; out.sections = v.sections;
        out.serial = v.serial;
    }
    void editorCommand(const std::string& a, const std::string& t) override { c.command(a, t); }
    void editorFollowLeds(const std::array<int, 4>&, const std::array<int, 2>&, const std::array<int, 3>&) override {}
};

class Port : public PanelPort {
public:
    explicit Port(EditorPort& e) : ed(e) {}
    EditorPort& ed;
    void sendControl(int, int) override {}
    int controlPosition(int) override { return 512; }
    void sendButton(int, bool) override {}
    LedReport led(int) override { return {}; }
    std::array<uint8_t, 4> segments() override { return {}; }
    bool knobPosition(const std::string&, int&) override { return false; }
    void setKnobPosition(const std::string&, int) override {}
    EditorPort* editor() override { return &ed; }
};
}  // namespace

int main() {
    FakeLink link;
    editor::Controller c(link);
    edtables::OsTables t;                            // no OS tables needed for the presets page
    t.ok = true;
    c.setTables(t);
    Adapter adapter(c);
    Port port(adapter);
    SkinView v(port, {"Phyzo", "DHammers", "9.9.9"}, nullptr);
    v.setViewport(1795, 848 + 394, 1.0f);
    std::string err;
    CHECK(v.load(kSkins + "/test", "test", err));
    v.setEditorVisible(true);
    c.setOpen(true);
    double now = 0, t2 = 100.0;
    auto tick = [&] { for (int i = 0; i < 10; ++i) c.tick(now += 20); v.update(t2 += 0.05); };
    c.command("area", "PRESETS");
    for (int i = 0; i < 40; ++i) tick();             // the names and the current preset's name are read
    editor::View view = c.view();
    CHECK(view.area == "PRESETS" && view.page == 1);
    CHECK(view.focusRow == 1 && view.focusCol == 0 && view.focusLen == 20);   // "03  Preset 03" is marked
    auto byId = [&](const char* id) -> VfdRowElement* {
        for (int i = 0; i < v.context()->GetNumDocuments(); ++i)
            if (Rml::Element* e = v.context()->GetDocument(i)->GetElementById(id)) return dynamic_cast<VfdRowElement*>(e);
        return nullptr;
    };
    VfdRowElement* r1 = byId("r1");
    CHECK(r1 && r1->GetChild(0)->IsClassSet("focus") && r1->GetChild(19)->IsClassSet("focus") && !r1->GetChild(20)->IsClassSet("focus"));
    CHECK(r1 && !r1->GetChild(4)->GetChild(1)->IsClassSet("lit"));            // names are never underlined

    // A click on "06  Preset 06" (row 2, right half): its program change, and the mark moves to it at once.
    link.sent.clear();
    const int x = 149 + (25 + 6) * 30 + 5, y = 848 + 228 + 10;
    v.mouseMove(x, y, 0);
    v.mouseDown(0, 0);
    v.mouseUp(0, 0);
    CHECK(!link.sent.empty() && link.sent[0] == edproto::programChange(0, 5));
    link.current = "Preset 06";
    tick();
    view = c.view();
    CHECK(view.focusRow == 2 && view.focusCol == 25);
    CHECK(r1 && !r1->GetChild(0)->IsClassSet("focus"));
    VfdRowElement* r2 = byId("r2");
    CHECK(r2 && r2->GetChild(25)->IsClassSet("focus") && r2->GetChild(44)->IsClassSet("focus"));
    std::printf("skin_editor_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
