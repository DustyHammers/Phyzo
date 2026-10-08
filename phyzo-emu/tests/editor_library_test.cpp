// Editor LIBRARY area test (no ROM data; v0.8.3 step 2): the area's legends and page layout (8 entries per page,
// across the rows as the PRESETS area), progress in the count column, messages on row 4 for 3 seconds, a preset click
// sending exactly its stored message and nothing else, SAVE refused while the Compare LED is lit, the other actions,
// and nothing sent while the editor is closed.
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "editor/editor_controller.h"
#include "editor/editor_tables.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { std::printf("FAIL %s:%d: %s == %s\n  got [%s]\n", __FILE__, __LINE__, #a, #b, std::string(va).c_str()); ++failures; } } while (0)
using Bytes = std::vector<uint8_t>;

struct FakeLink : editor::Link {
    std::vector<Bytes> sent;
    std::map<int, int> leds;
    bool run = true;
    bool opened = false;
    void setOpen(bool on) override { opened = on; }
    bool send(const Bytes& m) override { sent.push_back(m); return true; }
    void setStandingRead(const std::vector<Bytes>&) override {}
    bool popReply(Bytes&) override { return false; }
    uint64_t ledStateMessages() const override { return 0; }
    uint64_t presetTriggers() const override { return 0; }
    uint64_t panelGestures() const override { return 0; }
    int ledMode(int code) const override { auto it = leds.find(code); return it == leds.end() ? 0 : it->second; }
    std::array<char, 4> display() const override { return {'P', ' ', '0', '1'}; }
    bool running() const override { return run; }
    bool panelButtonHeld() const override { return false; }
};

struct FakeLibrary : editor::LibraryLink {
    std::vector<Source> list;
    std::map<int, std::vector<Entry>> entryLists;
    std::vector<std::string> notices;
    uint64_t ser = 1;
    int opens = 0, saves = 0, imports = 0, reveals = 0;
    std::vector<int> exports;
    void open() override { ++opens; }
    uint64_t serial() const override { return ser; }
    std::vector<Source> sources() const override { return list; }
    std::vector<Entry> entries(int source) const override { auto it = entryLists.find(source); return it == entryLists.end() ? std::vector<Entry>() : it->second; }
    bool message(int source, int entry, Bytes& out) const override {
        auto it = entryLists.find(source);
        if (it == entryLists.end() || entry < 0 || entry >= int(it->second.size()) || !it->second[size_t(entry)].accepted) return false;
        out = {0xF0, 0x0F, 0x11, 0x01, 0x00, 0x02, 0x17, uint8_t(source), uint8_t(entry), 0xF7};
        return true;
    }
    bool takeNotice(std::string& t) override { if (notices.empty()) return false; t = notices.front(); notices.erase(notices.begin()); return true; }
    void save() override { ++saves; }
    void exportBank(int bank) override { exports.push_back(bank); }
    void importFiles() override { ++imports; }
    void reveal() override { ++reveals; }
};

edtables::OsTables tables() {
    edtables::OsTables t;
    t.ok = true;
    for (int i = 0; i < 4; ++i) {
        edtables::Field f; f.name = "S" + std::to_string(i); f.offset = uint32_t(i); f.type = edtables::kUnsignedByte; f.min = 0; f.max = 127;
        t.system.fields.push_back(f);
    }
    return t;
}

std::string row(const editor::View& v, int r) { return v.screen.rowText(r); }
std::string pad(std::string s, size_t n) { s.resize(n, ' '); return s; }
std::string halves(const std::string& a, const std::string& b) { return pad(a, 25) + pad(b, 25); }
std::string line(const std::string& a) { return pad(a, 50); }

void test() {
    FakeLink link;
    FakeLibrary lib;
    lib.list = {{"FACTORY BANK 1", 32, 0, 0, false}, {"FACTORY BANK 2", 32, 0, 0, false}, {"My Pads", 32, 0, 0, false},
                {"Live Set 2", 0, 12, 32, true}, {"SINGLES", 7, 0, 0, false}};
    std::vector<std::string> names = {"Alpha Pad", "Beta Arp", "Gamma Bass", "Delta Lead", "Epsilon Bell", "Zeta Kit", "Eta Strings", "Theta Sync"};
    std::vector<editor::LibraryLink::Entry> bank;
    for (int i = 0; i < 32; ++i) bank.push_back({i < 8 ? names[size_t(i)] : "Preset " + std::to_string(i + 1), true, true});
    bank[6].accepted = false;
    lib.entryLists[0] = bank;
    lib.entryLists[2] = std::vector<editor::LibraryLink::Entry>(32, {"Pad", true, true});
    editor::Controller c(link);
    c.setTables(tables());
    CHECK(!c.view().areas.count("LIBRARY"));                 // no library: no area
    c.setLibrary(&lib);
    double now = 0;
    auto tick = [&](double ms) { now += ms; c.tick(now); };
    c.setOpen(true);
    tick(10);
    editor::View v = c.view();
    CHECK(v.areas.count("LIBRARY") == 1);
    CHECK(lib.opens == 0);
    c.command("area", "LIBRARY");
    CHECK(lib.opens == 1);                                   // rescans when opened
    v = c.view();
    CHECK(v.area == "LIBRARY" && v.section == "SOURCES" && v.sections == std::set<std::string>({"SOURCES", "PRESETS"}));
    CHECK(v.page == 1 && v.pages == 1 && !v.prev && !v.next);
    // SOURCES: across the rows, as the PRESETS area; the count at the right of each half, progress while read.
    CHECK_EQ(row(v, 0), line("FACTORY BANK 1       32  FACTORY BANK 2       32"));
    CHECK_EQ(row(v, 1), line("My Pads              32  Live Set 2        12/32"));
    CHECK_EQ(row(v, 2), line("SINGLES               7"));
    CHECK_EQ(row(v, 3), std::string(50, ' '));
    // Opening a source shows its presets: 8 per page, "nn  name"; a bank is 4 pages.
    c.command("pick", std::to_string(editor::Controller::kLibrarySourceRef + 0));
    v = c.view();
    CHECK(v.section == "PRESETS" && v.page == 1 && v.pages == 4 && v.next);
    CHECK_EQ(row(v, 0), halves("01  Alpha Pad", "02  Beta Arp"));
    CHECK_EQ(row(v, 3), halves("07  Eta Strings", "08  Theta Sync"));
    c.command("page_next", "NEXT");
    v = c.view();
    CHECK(v.page == 2 && v.prev);
    CHECK_EQ(row(v, 0), halves("09  Preset 9", "10  Preset 10"));
    c.command("page", "4");
    CHECK(c.view().page == 4 && !c.view().next);
    c.command("page", "1");
    // A click sends exactly that preset's stored message, nothing else; marked; the message on row 4 for 3 s.
    link.sent.clear();
    c.command("pick", std::to_string(editor::Controller::kLibraryEntryRef + 1));
    CHECK(link.sent.size() == 1 && link.sent[0] == Bytes({0xF0, 0x0F, 0x11, 0x01, 0x00, 0x02, 0x17, 0, 1, 0xF7}));
    v = c.view();
    CHECK(v.focusRow == 0 && v.focusCol == 25 && v.focusLen == 20);
    CHECK_EQ(row(v, 3), line("02 Beta Arp IN COMPARE  SAVE ON PANEL TO KEEP"));
    tick(2900);
    CHECK_EQ(row(c.view(), 3), line("02 Beta Arp IN COMPARE  SAVE ON PANEL TO KEEP"));
    tick(200);
    CHECK_EQ(row(c.view(), 3), halves("07  Eta Strings", "08  Theta Sync"));   // the entries again
    CHECK(link.sent.size() == 1);                            // nothing more was sent
    // A preset the OS did not accept: a message, nothing sent.
    c.command("pick", std::to_string(editor::Controller::kLibraryEntryRef + 6));
    CHECK(link.sent.size() == 1);
    CHECK_EQ(row(c.view(), 3), line("07 Eta Strings  NOT ACCEPTED BY THE OS"));
    tick(3100);
    // A long name: the number goes first.
    lib.entryLists[0][2].name = "Sixteen Chars Nm";
    c.command("pick", std::to_string(editor::Controller::kLibraryEntryRef + 2));
    CHECK_EQ(row(c.view(), 3), line("Sixteen Chars Nm IN COMPARE  SAVE ON PANEL TO KEEP"));
    tick(3100);
    // The marked preset stays marked on its page only.
    c.command("page", "2");
    CHECK(c.view().focusRow == -1);
    c.command("page", "1");
    CHECK(c.view().focusRow == 1 && c.view().focusCol == 0);
    // SAVE: refused while the Compare LED is lit (on or flashing); otherwise the library's.
    link.leds[editor::Controller::kCompareLed] = 1;
    c.command("library", "SAVE");
    CHECK(lib.saves == 0);
    CHECK_EQ(row(c.view(), 3), line("UNSAVED EDITS  SAVE ON THE PANEL FIRST"));
    link.leds[editor::Controller::kCompareLed] = 2;
    c.command("library", "SAVE");
    CHECK(lib.saves == 0);
    link.leds[editor::Controller::kCompareLed] = 0;
    c.command("library", "SAVE");
    CHECK(lib.saves == 1);
    c.command("library", "EXPORT B1");
    c.command("library", "EXPORT B2");
    c.command("library", "IMPORT");
    c.command("library", "FOLDER");
    CHECK((lib.exports == std::vector<int>{0, 1}) && lib.imports == 1 && lib.reveals == 1);
    tick(3100);
    // The library's messages: row 4, 3 s each, in order.
    lib.notices = {"SAVED  Singles/Beta Arp.syx", "EXPORTED BANK 1"};
    tick(20);
    CHECK_EQ(row(c.view(), 3), line("SAVED  Singles/Beta Arp.syx"));
    tick(3000);
    CHECK_EQ(row(c.view(), 3), line("EXPORTED BANK 1"));
    tick(3000);
    CHECK_EQ(row(c.view(), 3), halves("07  Eta Strings", "08  Theta Sync"));
    // Progress shows in the count column as the library changes.
    c.command("section", "SOURCES");
    lib.list[3].done = 13; ++lib.ser;
    tick(20);
    CHECK_EQ(row(c.view(), 1), line("My Pads              32  Live Set 2        13/32"));
    // A source still being read, opened: its progress until its presets are known.
    c.command("pick", std::to_string(editor::Controller::kLibrarySourceRef + 3));
    CHECK_EQ(row(c.view(), 0), line("Live Set 2  READING  13/32"));
    // Closed: a click sends nothing; messages from while it was closed are not shown when it opens again.
    c.command("pick", std::to_string(editor::Controller::kLibrarySourceRef + 0));
    c.setOpen(false);
    link.sent.clear();
    c.command("pick", std::to_string(editor::Controller::kLibraryEntryRef + 0));
    CHECK(link.sent.empty());
    lib.notices = {"OLD NEWS"};
    c.setOpen(true);
    tick(20);
    CHECK(row(c.view(), 3).find("OLD NEWS") == std::string::npos);
    CHECK(lib.opens >= 2);                                   // opened again on the LIBRARY area: rescanned
    // Other areas are untouched by the library.
    c.command("area", "SYSTEM");
    CHECK(c.view().area == "SYSTEM");
    std::printf("editor_library_test: legends, layout, progress, messages, picks, actions\n");
}
}  // namespace

int main() {
    test();
    std::printf("editor_library_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
