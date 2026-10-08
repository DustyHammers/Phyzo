// Editor engine test, part 1 (no ROM data): messages, OS tables, pages.
//  1. Protocol: 14/32-bit numbers, partial read / write, whole read and NRPN encode as specified; a partial write
//     parses back to the same address and data (the reply format is the same header and data words); the audio
//     thread's classifications (replies, requests, preset triggers).
//  2. Tables: a synthetic image of our own (descriptor, field records with padding and every sub-record kind, names,
//     offsets, value lists with header, compression and marker) is read back exactly; hidden names.
//  3. Pages: name shortening, value formatting, two-column rows (numbered runs, overflow), the stage table, the code
//     grid, the name list, pagination and rendering (underlines on values only).
//  4. Controller (with a fake engine link and tables of our own): nothing sent while closed; the visible page read
//     on opening and after each navigation (and set as the standing read); replies shown; areas, sections, targets,
//     pages, PREV / NEXT; Follow from the Sound / OSC / envelope LEDs; re-reads after an LED burst or a panel gesture
//     (debounced 50 ms) and after a display change with no LED burst (600 ms), never otherwise (no polling); the wave
//     list and preset names.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "editor/editor_controller.h"
#include "editor/editor_pages.h"
#include "editor/editor_proto.h"
#include "editor/editor_tables.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); ++failures; } } while (0)
using Bytes = std::vector<uint8_t>;

void testProto() {
    using namespace edproto;
    Bytes m;
    put14(m, 0x3FFF); put32(m, 0x0BE06F50); put32(m, 0xFFFFFFFF);
    CHECK((m == Bytes{0x7F, 0x7F, 0x50, 0x5E, 0x01, 0x5F, 0x00, 0x7F, 0x7F, 0x7F, 0x7F, 0x0F}));
    CHECK(get14(&m[0]) == 0x3FFF && get32(&m[2]) == 0x0BE06F50 && get32(&m[7]) == 0xFFFFFFFF);

    Address a{kPerformance, kEditBuffer, 1, 2, 4, 2};
    CHECK((partialRead(a) == Bytes{0xF0, 0x0F, 0x11, 0x01, 0x7F, 0x04, 0x02, 0x7F, 0x7F, 0x01, 0x02, 0x00,
                                   0x04, 0, 0, 0, 0, 0x02, 0, 0, 0, 0, 0xF7}));
    CHECK((wholeRead(kSystem, 0) == Bytes{0xF0, 0x0F, 0x11, 0x01, 0x7F, 0x01, 0x00, 0x00, 0x00, 0xF7}));
    const auto n = nrpn(2, 5, 0x39, -3);
    CHECK(n.size() == 6 && (n[0] == Bytes{0xB2, 0x63, 5}) && (n[1] == Bytes{0xB2, 0x62, 0x39}) &&
          (n[2] == Bytes{0xB2, 0x06, 0x7F}) && (n[3] == Bytes{0xB2, 0x26, 0x7D}) && (n[5] == Bytes{0xB2, 0x62, 0x7F}));
    int msb, lsb;
    oscillatorNrpn(3, 1, 0x85, msb, lsb);
    CHECK(msb == 2 + 12 + 2 + 1 && lsb == 5);

    // Data words round trip, odd lengths padded and cut.
    for (uint32_t len : {0u, 1u, 3u, 4u, 5u, 16u, 233u}) {
        Bytes data(len);
        for (uint32_t i = 0; i < len; ++i) data[i] = uint8_t(i * 37 + 11);
        Address w{kSound, 2, 2, 1, 0x39, len};
        const Bytes msg = partialWrite(w, data);
        Message back;
        CHECK(parse(msg.data(), msg.size(), back));
        CHECK(back.command == kPartialWrite && back.address == w && back.data == data);
        // The OS's reply: same layout, command 0x44, device 00.
        Bytes reply = msg;
        reply[4] = 0x00; reply[5] = kPartialReply;
        CHECK(parse(reply.data(), reply.size(), back) && back.command == kPartialReply && back.data == data);
        CHECK(isReply(reply.data(), reply.size()) && !isReply(msg.data(), msg.size()));
    }
    const Bytes rd = partialRead(a);
    CHECK(isRequest(rd.data(), rd.size()));
    const Bytes other = {0xF0, 0x41, 0x10, 0x42, 0x12, 0xF7}, pc = {0xC3, 0x05}, bank = {0xB0, 0x20, 0x01}, cc = {0xB0, 0x07, 0x40};
    CHECK(!isReply(other.data(), other.size()) && !isRequest(other.data(), other.size()));
    CHECK(isPresetTrigger(pc.data(), 2) && isPresetTrigger(bank.data(), 3) && isPresetTrigger(other.data(), other.size()));
    CHECK(!isPresetTrigger(cc.data(), 3));
    std::printf("editor_test: protocol encoders and decoders round-trip\n");
}

// ---- a synthetic OS image (our own made-up content; the layouts of docs/editor/PR_B_ANSWERS.md)
struct ImageBuilder {
    Bytes img = Bytes(0x10000, 0);
    uint32_t next = 0x4100;
    uint32_t addr(size_t off) const { return uint32_t(0x4000 + off); }
    uint32_t alloc(size_t n) { const uint32_t a = next; next += uint32_t((n + 3) & ~size_t(3)); return a; }
    void put8(uint32_t a, uint8_t v) { img[a - 0x4000] = v; }
    void put16(uint32_t a, uint16_t v) { put8(a, uint8_t(v >> 8)); put8(a + 1, uint8_t(v)); }
    void put32(uint32_t a, uint32_t v) { put16(a, uint16_t(v >> 16)); put16(a + 2, uint16_t(v)); }
    uint32_t bytes(const Bytes& b) { const uint32_t a = alloc(b.size()); for (size_t i = 0; i < b.size(); ++i) put8(a + uint32_t(i), b[i]); return a; }
    uint32_t cstr(const std::string& s) { Bytes b(s.begin(), s.end()); b.push_back(0); return bytes(b); }
    uint32_t array(const std::vector<uint32_t>& v) {
        const uint32_t a = alloc(4 + 4 * v.size());
        put32(a, uint32_t(v.size()));
        for (size_t i = 0; i < v.size(); ++i) put32(a + 4 + 4 * uint32_t(i), v[i]);
        return a;
    }
};

Bytes record(uint8_t flags, uint16_t type, int16_t min, int16_t max, const Bytes& subs) {
    Bytes r = {0, flags, uint8_t(type >> 8), uint8_t(type), uint8_t(uint16_t(min) >> 8), uint8_t(min), uint8_t(uint16_t(max) >> 8), uint8_t(max)};
    r.insert(r.end(), subs.begin(), subs.end());
    r[0] = uint8_t(r.size());
    return r;
}
Bytes numberSub(uint16_t fmt, uint32_t param) { return {8, 1, uint8_t(fmt >> 8), uint8_t(fmt), uint8_t(param >> 24), uint8_t(param >> 16), uint8_t(param >> 8), uint8_t(param)}; }
Bytes listSub(uint32_t idx) { return {12, 3, 0, 0, 0, 0, 0, 0, uint8_t(idx >> 24), uint8_t(idx >> 16), uint8_t(idx >> 8), uint8_t(idx)}; }
Bytes fmtSub(uint32_t n) { return {12, 5, 0, 0, 0, 0, 0, 0, 0, 0, 0, uint8_t(n)}; }

void testTables() {
    using namespace edtables;
    ImageBuilder b;
    // Lists: "Low Pass\rLow Shelf\rHigh Pass" compressed; a header list starting at -2; a marker.
    const uint32_t l0 = b.bytes({'L', 'o', 'w', ' ', 'P', 'a', 's', 's', 0x0D, 0x14, 'S', 'h', 'e', 'l', 'f', 0x0D, 'H', 'i', 'g', 'h', ' ', 'P', 'a', 's', 's', 0x00});
    const uint32_t l1 = b.bytes({0x80, 0x05, 0xFF, 0xFE, 'L', '2', 0x0D, 'L', '1', 0x0D, 'C', 'e', 'n', 't', 'e', 'r', 0x0D, 0x11, '1', 0x0D, 0x11, '2', 0x00});
    const uint32_t l2 = b.bytes({'O', 'n', 'e', ' ', 'm', 'o', 'r', 'e', 0x0D, 0x10, 0x0D, 'X', 0x00});
    const std::vector<std::string> names = {"Filter Mode", "Pan", "SPARE 1", "Filter Cutoff", "Env1 Time (1)", "Env1 Level (1)", "Mute", "Wave ID"};
    std::vector<uint32_t> nameAddrs;
    for (auto& n : names) nameAddrs.push_back(b.cstr(n));
    const std::vector<uint32_t> offsets = {0x10, 0x11, 0x12, 0x39, 0x40, 0x41, 0x50, 0x4C};
    Bytes recs;
    auto add = [&](const Bytes& r, int pad) { recs.insert(recs.end(), r.begin(), r.end()); recs.insert(recs.end(), size_t(pad), 0); };
    add(record(0x03, kUnsignedByte, 0, 2, listSub(0)), 2);
    add(record(0x01, kSignedListMinusOne, -2, 2, listSub(1)), 0);
    add(record(0x01, kUnsignedByte, 0, 127, numberSub(0, 2)), 1);
    add(record(0x83, kUnsignedByte, 0, 127, numberSub(0, 0x39)), 0);
    add(record(0x01, kSignedFormatted, -64, 63, fmtSub(6)), 0);
    add(record(0x01, kSignedByte, -99, 99, numberSub(kFmtSigned, 0x41)), 0);
    add(record(0x01, kBitField, int16_t(1 << 8 | 3), 1, numberSub(kFmtOffOn, 0x50)), 3);
    add(record(0x01, kLong, 0, -1, numberSub(0, 0x4C)), 0);
    const uint32_t recA = b.bytes(recs);
    const uint32_t desc = b.alloc(32);
    b.put32(desc, recA);
    b.put32(desc + 4, b.array(nameAddrs));
    b.put32(desc + 8, b.array(offsets));
    b.put32(desc + 12, b.array({l0, l1, l2}));
    b.put32(desc + 20, uint32_t(names.size()) << 16 | 0x0200);

    Table t;
    std::string err;
    CHECK(readTable(b.img, desc, "test", t, err));
    if (!err.empty()) std::printf("editor_test: %s\n", err.c_str());
    CHECK(t.fields.size() == names.size() && t.lists.size() == 3 && t.descriptorFlags == 0x0200);
    for (size_t i = 0; i < t.fields.size() && i < names.size(); ++i) {
        CHECK(t.fields[i].name == names[i] && t.fields[i].offset == offsets[i] && t.fields[i].index == int(i));
    }
    if (t.fields.size() == names.size()) {
        CHECK(t.fields[0].display == Display::List && t.fields[0].listIndex == 0 && t.fields[0].flags == 0x03);
        CHECK(t.fields[1].type == kSignedListMinusOne && t.fields[1].min == -2 && t.fields[1].listIndex == 1);
        CHECK(t.fields[2].hidden && !t.fields[3].hidden);
        CHECK(t.fields[3].param == 0x39 && t.fields[3].flags == 0x83);
        CHECK(t.fields[4].display == Display::Formatter && t.fields[4].formatter == 6);
        CHECK(t.fields[5].format == kFmtSigned);
        CHECK(t.fields[6].type == kBitField && t.fields[6].format == kFmtOffOn);
        CHECK(t.fields[7].type == kLong && t.fields[7].size() == 4);
    }
    CHECK((t.lists[0].entries == std::vector<std::string>{"Low Pass", "Low Shelf", "High Pass"}) && !t.lists[0].hasFirst);
    // 0x11 copies one character from the same position of the previous entry: "Center" -> "C" + "1", then "C" + "2".
    CHECK((t.lists[1].entries == std::vector<std::string>{"L2", "L1", "Center", "C1", "C2"}) && t.lists[1].hasFirst && t.lists[1].first == -2);
    CHECK((t.lists[2].entries == std::vector<std::string>{"One more", "One more", "X"}));
    {   // a list with no terminator, followed directly by the next list's 0x80 start marker: it ends there
        ImageBuilder lb;
        const uint32_t a = lb.bytes({0x80, 'O', 'f', 'f', 0x0D, 'd', 'P', 'r', 'S', 0x80, 0x05, 0x00, 0x00, ' ', ' ', 'O', 'n', 0x00});
        CHECK((decodeList(lb.img, a).entries == std::vector<std::string>{"Off", "dPrS"}));
        CHECK((decodeList(lb.img, a + 9).entries == std::vector<std::string>{"  On"}) && decodeList(lb.img, a + 9).hasFirst);
        const uint32_t e = lb.bytes({0x80, 'A', 0x0D, 'B', 0x0D, 0x80, 'C', 0x00});   // after a separator: no empty entry
        CHECK((decodeList(lb.img, e).entries == std::vector<std::string>{"A", "B"}));
    }

    // Hidden names
    CHECK(isHiddenName("SPARE 3") && isHiddenName("Wave Class") && isHiddenName("WaveChecksum") && isHiddenName("Use Layer") &&
          isHiddenName("Old Keybd Track") && isHiddenName("????") && isHiddenName(" ???"));
    CHECK(!isHiddenName("Filter Cutoff") && !isHiddenName("Wave ID") && !isHiddenName("Layer Mode"));

    // A broken table: a pointer outside the image.
    ImageBuilder bad;
    const uint32_t d2 = bad.alloc(32);
    bad.put32(d2, 0x00FFFFF0);
    Table t2;
    CHECK(!readTable(bad.img, d2, "broken", t2, err) && !err.empty());
    std::printf("editor_test: OS tables read back from a synthetic image\n");

    // ---- values (with the table above)
    using namespace edpages;
    Bytes obj(0x60, 0);
    obj[0x10] = 1; obj[0x11] = uint8_t(-1); obj[0x39] = 99; obj[0x40] = uint8_t(-5); obj[0x41] = 7; obj[0x50] = 0x08;
    obj[0x4C] = 0x12; obj[0x4D] = 0x34; obj[0x4E] = 0x56; obj[0x4F] = 0x78;
    Waves w;
    w.ids = {0x11111111, 0x12345678};
    w.names = {"", "Choir"};
    auto show = [&](int i) { int64_t v; CHECK(rawValue(t.fields[size_t(i)], obj.data(), obj.size(), 0, v)); return formatValue(t.fields[size_t(i)], t, v, &w); };
    if (t.fields.size() == names.size()) {
        CHECK_EQ(show(0), std::string("Low Shelf"));
        CHECK_EQ(show(1), std::string("L1"));
        CHECK_EQ(show(3), std::string("99"));
        CHECK_EQ(show(4), std::string("-5"));
        CHECK_EQ(show(5), std::string("+7"));
        CHECK_EQ(show(6), std::string("On"));
        CHECK_EQ(show(7), std::string("2 Choir"));
        w.names[1] = "";
        CHECK_EQ(show(7), std::string("2"));
        int64_t v;
        CHECK(!rawValue(t.fields[3], obj.data(), 0x20, 0, v));          // not in the bytes read
        CHECK(rawValue(t.fields[3], obj.data() + 0x30, 0x10, 0x30, v) && v == 99);   // a partial read from 0x30
    }
    {   // value type 2: stored = shown + 64; a list on it indexes with the list's start value
        edtables::Field f;
        f.type = kOffsetByte; f.min = -64; f.max = 63;
        Table lt;
        const uint8_t b[] = {64, 70, 0};
        int64_t v;
        CHECK(rawValue(f, b, 3, 0, v) && v == 0 && formatValue(f, lt, v) == "0");
        CHECK(rawValue(f, b + 1, 2, 0, v) && v == 6 && formatValue(f, lt, v) == "+6");
        CHECK(rawValue(f, b + 2, 1, 0, v) && v == -64 && formatValue(f, lt, v) == "-64");
        lt.lists.push_back({true, -64, {}});
        for (int i = -64; i < 64; ++i) lt.lists[0].entries.push_back(i == 0 ? " Center" : i < 0 ? "L" + std::to_string(-i) : "R" + std::to_string(i));
        f.display = Display::List; f.listIndex = 0;
        CHECK(rawValue(f, b, 3, 0, v) && formatValue(f, lt, v) == "Center");
        CHECK(rawValue(f, b + 2, 1, 0, v) && formatValue(f, lt, v) == "L64");
    }
    {   // value type 3: the record's min is mask << 8 | bit; a flag (mask 1) and one field with mask 0x0F and a list at 8
        edtables::Field f;
        f.type = kBitField;
        Table lt;
        const uint8_t b[] = {144, 0x08};
        int64_t v;
        f.min = int16_t(1 << 8 | 3); f.max = 1;
        CHECK(rawValue(f, b + 1, 1, 0, v) && v == 1);
        CHECK(rawValue(f, b, 1, 0, v) && v == 0);
        f.min = int16_t(0x0F04); f.max = int16_t(0x0809);
        CHECK(rawValue(f, b, 1, 0, v) && v == 9);                               // (144 >> 4) & 0x0F
        lt.lists.push_back({true, 8, {"NoteOff", "NoteOn"}});
        f.display = Display::List; f.listIndex = 0;
        CHECK_EQ(formatValue(f, lt, v), std::string("NoteOn"));
    }
    {   // number formats: 0x0001 shown 1-based; 0xC000 three digits, zero-padded, as stored (0-based)
        edtables::Field f;
        f.type = kUnsignedByte; f.min = 0; f.max = 127;
        Table lt;
        f.format = kFmtOneBased;
        CHECK(formatValue(f, lt, 0) == "1" && formatValue(f, lt, 15) == "16");
        f.format = kFmtThreeDigits;
        CHECK(formatValue(f, lt, 0) == "000" && formatValue(f, lt, 7) == "007" && formatValue(f, lt, 127) == "127");
    }
    CHECK_EQ(noteName(60), std::string("C4"));
    CHECK_EQ(noteName(0), std::string("C-1"));
    CHECK_EQ(noteName(127), std::string("G9"));
    std::printf("editor_test: values formatted\n");
}

void testPages() {
    using namespace edpages;
    CHECK_EQ(shortenName("Filter Cutoff", 20), std::string("FILTER CUTOFF"));
    CHECK_EQ(shortenName("Filter Cutoff Keybd Track", 18), std::string("FILTER CUT KBD TRK"));
    CHECK_EQ(shortenName("FilterCutoffKeybdTrack", 16), std::string("FILTER CUTKBDTRK"));
    CHECK_EQ(shortenName("Velocity Modulation Amount", 10), std::string("VEL MODAMT"));
    CHECK_EQ(shortenName("Envelope Release Velocity 2", 9), std::string("ENVRELV 2"));
    CHECK_EQ(shortenName("Envelope Release Velocity 2", 8), std::string("ENVREL 2"));
    CHECK_EQ(shortenName("Envelope Release Velocity 2", 5), std::string("ENV 2"));
    CHECK_EQ(shortenName("Something Very Long Indeed", 6), std::string("SOMETH"));

    // Two-column rows: a run of identically named fields numbered; an overlong value on a full row.
    std::vector<Item> items = {{"Mod Source", "Env 1", 0}, {"Mod Source", "LFO", 1}, {"Cutoff", "64", 2},
                               {"Mode", "A very long value text here", 3}, {"Res", "10", 4}};
    const std::vector<Row> rows = twoColumnRows(items);
    CHECK(rows.size() == 4);
    CHECK(rows[0].kind == RowKind::Pair && rows[0].items[0].name == "Mod Source 1" && rows[0].items[1].name == "Mod Source 2");
    CHECK(rows[1].kind == RowKind::Pair && rows[1].items.size() == 1 && rows[1].items[0].name == "Cutoff");
    CHECK(rows[2].kind == RowKind::Full && rows[3].kind == RowKind::Pair);
    const Screen s = render(Page(rows.begin(), rows.end()));
    CHECK_EQ(s.rowText(0), std::string("MOD SOURCE 1       Env 1 MOD SOURCE 2         LFO "));
    CHECK_EQ(s.rowText(2), std::string("MODE                   A very long value text here"));
    // Underlines: exactly the value cells.
    int underlined = 0;
    for (auto& r : s.underline) for (bool u : r) underlined += u;
    CHECK(underlined == 5 + 3 + 2 + 27 + 2);
    CHECK(s.values.size() == 5 && s.values[0].ref == 0 && s.values[0].len == 5);
    for (int c = 0; c < 18; ++c) CHECK(!s.underline[0][size_t(c)]);   // names never underlined
    // Repeated blocks: two single fields, then two blocks with the same names in the same order -> numbered columns.
    const std::vector<Item> filt = {{"Mode", "2LP", 0}, {"Link", "Indep", 1}, {"Cutoff", "19", 2}, {"Resonance", "16", 3},
                                    {"Key Track", "+4", 4}, {"Cutoff", "19", 5}, {"Resonance", "0", 6}, {"Key Track", "-2", 7}};
    const std::vector<Row> fr = twoColumnRows(filt);
    CHECK(fr.size() == 4);
    CHECK(fr[0].items.size() == 2 && fr[0].items[0].name == "Mode" && fr[0].items[1].name == "Link");
    CHECK(fr[1].items[0].name == "Cutoff 1" && fr[1].items[1].name == "Cutoff 2" && fr[1].items[1].ref == 5);
    CHECK(fr[3].items[0].name == "Key Track 1" && fr[3].items[1].name == "Key Track 2" && fr[3].items[1].value == "-2");
    {   // a field after the blocks is laid out as usual
        std::vector<Item> more = filt;
        more.push_back({"Spread", "3", 8});
        const std::vector<Row> mr = twoColumnRows(more);
        CHECK(mr.size() == 5 && mr[4].items.size() == 1 && mr[4].items[0].name == "Spread");
    }
    CHECK(twoColumnRows({{"A", "1", 0}, {"A", "2", 1}}).size() == 1);    // a run of two: one row, numbered
    CHECK(twoColumnRows({{"A", "1", 0}, {"A", "2", 1}})[0].items[1].name == "A 2");

    // A name uses all the cells its half leaves after its own value and one space.
    {
        Row r;
        r.items = {{"Wave", "58 Some Wave Name", 0, 0}, {"Wave Shift Amount", "7", 1, 2}};
        const Screen ws = render(Page{r});
        CHECK_EQ(ws.rowText(0).substr(25, 24), std::string("WAVE SHIFT AMOUNT      7"));
    }
    // Underlines: from the first to the last character shown, not over blanks (a value with spaces around it).
    {
        Row r;
        r.kind = RowKind::Grid;
        r.items = {{"  id", " 12 ", 0}};
        const Screen us = render(Page{r});
        CHECK_EQ(us.rowText(0).substr(0, 9), std::string("ID    12 "));
        CHECK(!us.underline[0][5] && us.underline[0][6] && us.underline[0][7] && !us.underline[0][8]);
        CHECK(us.values.size() == 1 && us.values[0].col == 6 && us.values[0].len == 2);
    }
    // Names as shown: trimmed, split at camel-case boundaries.
    CHECK_EQ(displayName("SemitoneTuning"), std::string("SemitoneTuning").insert(8, " "));
    CHECK_EQ(displayName("Env1PitchModAmt"), std::string("Env1 Pitch Mod Amt"));
    CHECK_EQ(displayName("NoiseSource Rate"), std::string("Noise Source Rate"));
    CHECK_EQ(displayName("TriggerCtrlHi"), std::string("Trigger Ctrl Hi"));
    CHECK_EQ(displayName("  LFORate  "), std::string("LFO Rate"));
    CHECK_EQ(displayName("Env2 Time (3)"), std::string("Env2 Time (3)"));
    CHECK_EQ(displayName("Resonance (Q)"), std::string("Resonance (Q)"));
    CHECK_EQ(trimmed(" tch"), std::string("tch"));
    std::printf("editor_test: two-column rows, repeated blocks, name room, underlines, names\n");

    // Stage table
    bool isTime; int st;
    CHECK(isStageName("Env2 Time (3)", isTime, st) && isTime && st == 3);
    CHECK(isStageName("Level 4", isTime, st) && !isTime && st == 4);
    CHECK(!isStageName("Velocity", isTime, st));
    std::vector<Item> env = {{"Time (1)", "10", 0}, {"Level (1)", "99", 1}, {"Time (2)", "20", 2}, {"Level (2)", "50", 3},
                             {"Velocity", "4", 4}};
    std::vector<Item> rest;
    const std::vector<Row> stage = stageRows(env, rest);
    CHECK(stage.size() == 3 && rest.size() == 1 && rest[0].name == "Velocity");
    const Screen ss = render(Page(stage.begin(), stage.end()));
    CHECK_EQ(ss.rowText(1).substr(0, 8), std::string("TIME    "));
    CHECK(ss.rowText(1).find("10") != std::string::npos && ss.rowText(2).find("99") != std::string::npos);

    // Code grid and name list
    std::vector<Item> codes;
    for (int i = 0; i < 6; ++i) codes.push_back({"C" + std::to_string(i) + "XX", std::to_string(i * 10), i});
    const std::vector<Row> grid = gridRows(codes);
    CHECK(grid.size() == 2 && grid[0].items.size() == 4 && grid[1].items.size() == 2);
    const Screen gs = render(Page(grid.begin(), grid.end()));
    CHECK_EQ(gs.rowText(0).substr(0, 12), std::string("C0XX 0      "));
    CHECK_EQ(gs.rowText(0).substr(38, 12), std::string("C3XX 30     "));
    std::vector<Item> presets;
    for (int i = 0; i < 64; ++i) presets.push_back({(i < 9 ? "0" : "") + std::to_string(i + 1), "Preset " + std::to_string(i + 1), -1});
    const std::vector<Page> pages = paginate(nameRows(presets));
    CHECK(pages.size() == 8);
    const Screen ns = render(pages[7]);
    CHECK_EQ(ns.rowText(3), std::string("63  Preset 63            64  Preset 64            "));
    for (auto& r : ns.underline) for (bool u : r) CHECK(!u);           // lists to pick from are never underlined
    std::printf("editor_test: stage table, code grid, name list, pages\n");

    // Oscillator sections by field index (docs/editor/PR17_REVIEW.md): every index 0-137 has exactly one, in these counts.
    std::map<std::string, std::vector<int>> bySection;
    for (int i = 0; i < kOscillatorFields; ++i) {
        const char* sec = oscillatorSection(i);
        CHECK(sec != nullptr);
        if (sec) bySection[sec].push_back(i);
    }
    CHECK(oscillatorSection(kOscillatorFields) == nullptr && oscillatorSection(-1) == nullptr);
    CHECK((bySection["WAVE"] == std::vector<int>{51, 53, 54, 55, 56, 57, 58}));
    CHECK((bySection["PITCH"] == std::vector<int>{14, 15, 16, 17, 18, 23, 24, 25, 26, 27, 28, 74}));
    CHECK((bySection["AMP"] == std::vector<int>{12, 13, 61, 62, 63, 64, 65, 66, 67}));
    CHECK((bySection["NOISE"] == std::vector<int>{70, 77}));
    CHECK((bySection["GEN"] == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 19, 20, 75}));
    CHECK((bySection[""] == std::vector<int>{0, 21, 22, 29, 39, 40, 48, 49, 50, 52, 59, 60, 68, 69, 71, 72, 73, 76, 93, 109, 125, 137}));
    CHECK(bySection["FILTER"].size() == 16 && bySection["FILTER"].front() == 30 && bySection["FILTER"].back() == 47);
    CHECK(bySection["LFO"].size() == 11 && bySection["ENV1"].size() == 15 && bySection["ENV2"].front() == 94 && bySection["ENV3"].back() == 124);
    CHECK(soundSection("Low Key") == "RANGE" && soundSection("Rcv Pitch Bend") == "RECEIVE" && soundSection("Volume") == "TRACK");
}

// ---- controller
struct FakeLink : editor::Link {
    bool open = false, run = true;
    std::vector<Bytes> sent, standing;
    std::vector<Bytes> replies;
    uint64_t leds = 0, triggers = 0, gestures = 0;
    std::map<int, int> ledModes;
    std::array<char, 4> disp{{'P', ' ', '0', '1'}};
    void setOpen(bool on) override { open = on; }
    bool send(const Bytes& m) override { sent.push_back(m); return true; }
    void setStandingRead(const std::vector<Bytes>& m) override { standing = m; }
    bool popReply(Bytes& m) override { if (replies.empty()) return false; m = replies.front(); replies.erase(replies.begin()); return true; }
    uint64_t ledStateMessages() const override { return leds; }
    uint64_t presetTriggers() const override { return triggers; }
    uint64_t panelGestures() const override { return gestures; }
    bool panelButtonHeld() const override { return false; }
    int ledMode(int code) const override { auto it = ledModes.find(code); return it == ledModes.end() ? 0 : it->second; }
    std::array<char, 4> display() const override { return disp; }
    bool running() const override { return run; }
    // The OS's answer to a partial read: the bytes from `object` at the requested range.
    void answer(const Bytes& request, const Bytes& object) {
        edproto::Address a;                          // the request's address (F0 0F 11 01 dev 04, then the address)
        a.type = request[6]; a.number = uint16_t(edproto::get14(&request[7])); a.subtype = request[9];
        a.subnumber = uint16_t(edproto::get14(&request[10])); a.offset = edproto::get32(&request[12]); a.length = edproto::get32(&request[17]);
        Bytes data;
        for (uint32_t i = 0; i < a.length; ++i) data.push_back(a.offset + i < object.size() ? object[a.offset + i] : 0);
        Bytes r = edproto::partialWrite(a, data);
        r[4] = 0x00; r[5] = edproto::kPartialReply;
        replies.push_back(r);
    }
};

edtables::Field mkField(const std::string& name, uint32_t offset, uint16_t type, int min, int max, uint16_t fmt = 0, int list = -1) {
    edtables::Field f;
    f.name = name; f.offset = offset; f.type = type; f.min = int16_t(min); f.max = int16_t(max); f.format = fmt;
    if (list >= 0) { f.display = edtables::Display::List; f.listIndex = list; }
    f.hidden = edtables::isHiddenName(name);
    return f;
}

void testController() {
    using namespace edtables;
    OsTables t;
    t.ok = true;
    for (int i = 0; i < 20; ++i) t.system.fields.push_back(mkField("S" + std::to_string(100 + i).substr(1), uint32_t(i), kUnsignedByte, 0, 127));
    t.system.fields.push_back(mkField("????", 30, kUnsignedByte, 0, 1));
    t.oscillator.lists.push_back({false, 0, {"Low Pass", "High Pass"}});
    // The oscillator table: 138 fields, sections by index (spares everywhere but the fields under test).
    for (int i = 0; i < edpages::kOscillatorFields; ++i) t.oscillator.fields.push_back(mkField("SPARE " + std::to_string(i), 0xF0, kUnsignedByte, 0, 1));
    t.oscillator.fields[51] = mkField("Wave ID", 0x4C, kLong, 0, -1);
    t.oscillator.fields[30] = mkField("Filter Mode", 0x3A, kUnsignedByte, 0, 1, 0, 0);
    t.oscillator.fields[32] = mkField("Filter Cutoff", 0x39, kUnsignedByte, 0, 127);
    t.oscillator.fields[78] = mkField("Env1 Time (1)", 0x60, kUnsignedByte, 0, 99);
    t.oscillator.fields[79] = mkField("Env1 Level (1)", 0x61, kSignedByte, -99, 99);
    t.oscillator.fields[94] = mkField("Env2 Time (1)", 0x70, kUnsignedByte, 0, 99);
    t.oscillator.fields[110] = mkField("Env3 Time (1)", 0x74, kUnsignedByte, 0, 99);
    t.oscillator.fields[126] = mkField("LFO Rate", 0x80, kUnsignedByte, 0, 99);
    t.oscillator.fields[14] = mkField("Fine Tune", 0x20, kSignedByte, -64, 63);
    t.oscillator.fields[29] = mkField("Ramp Level", 0x21, kSignedByte, -64, 63);   // hidden by its index (not AMP)
    t.soundSettings.fields = {mkField("Volume", 4, kUnsignedByte, 0, 127), mkField("Pan", 5, kSignedByte, -64, 63),
                              mkField("Low Key", 6, kUnsignedByte, 0, 127, kFmtNote)};
    t.soundCommon.fields = {mkField("Glide Mode", 24, kUnsignedByte, 0, 3)};
    for (size_t i = 0; i < t.oscillator.fields.size(); ++i) t.oscillator.fields[i].index = int(i);

    FakeLink link;
    editor::Controller c(link);
    c.setTables(t);
    c.setFollowLeds({0x0B, 0x13, 0x03, 0x07}, {0x06, 0x15}, {0x11, 0x0C, 0x0E});
    c.tick(0);
    CHECK(link.sent.empty() && !link.open);                                   // closed: nothing
    editor::View v = c.view();
    CHECK(v.area == "SYSTEM" && v.pages == 2 && v.page == 1 && !v.prev && v.next);
    CHECK((v.areas == std::set<std::string>{"SYSTEM", "PRESETS", "COMMON", "SOUND", "OSC"}));   // no ARP: these tables have no arp texts

    // Open: the visible page is read once, and set as the standing read.
    c.setOpen(true);
    CHECK(link.open);
    c.tick(10);
    CHECK(link.sent.size() == 1 && link.standing == link.sent);
    const Bytes sysRead = edproto::partialRead({edproto::kSystem, 0, 0, 0, 0, 16});
    CHECK(link.sent[0] == sysRead);
    Bytes system(50);
    for (size_t i = 0; i < system.size(); ++i) system[i] = uint8_t(i * 3);
    link.answer(link.sent[0], system);
    c.tick(20);
    v = c.view();
    CHECK_EQ(v.screen.rowText(0).substr(0, 12), std::string("S00  0      "));
    CHECK_EQ(v.screen.rowText(0).substr(12, 12), std::string("S01  3      "));
    CHECK(v.screen.underline[0][5] && !v.screen.underline[0][0]);
    // No polling: time passes, nothing is sent.
    for (int ms = 30; ms < 5000; ms += 10) c.tick(ms);
    CHECK(link.sent.size() == 1);

    // PREV / NEXT, PAGE
    c.command("page_next", "");
    v = c.view();
    CHECK(v.page == 2 && v.prev && !v.next && link.sent.size() == 2);
    CHECK(link.sent[1] == edproto::partialRead({edproto::kSystem, 0, 0, 0, 16, 4}));   // the 4 fields of page 2 (the ???? slot hidden)
    c.command("page", "1");
    CHECK(c.view().page == 1);
    c.command("page", "7");                                                    // no such page: nothing
    CHECK(c.view().page == 1);
    c.command("area", "REVERB");                                              // not available: nothing
    CHECK(c.view().area == "SYSTEM");

    // OSC: sections from the names; the wave list read once; the wave number shown.
    link.sent.clear();
    c.command("area", "OSC");
    v = c.view();
    CHECK(v.area == "OSC" && v.section == "WAVE" && v.oscTargets && v.soundTargets);
    CHECK((v.sections == std::set<std::string>{"WAVE", "PITCH", "FILTER", "LFO", "ENV1", "ENV2", "ENV3"}));
    CHECK(link.sent.size() == 2);
    CHECK(link.sent[0] == edproto::partialRead({edproto::kSound, 0, 2, 0, 0x4C, 4}));
    CHECK(v.pages == 1 && !v.prev && !v.next);
    CHECK(link.sent[1] == edproto::partialRead({edproto::kRawMemory, 0, 0, 0, edtables::kWaveList + 4, 232}));
    Bytes osc(0x100, 0);
    osc[0x4C] = 0xAB; osc[0x4D] = 0xCD; osc[0x4E] = 0x00; osc[0x4F] = 0x07; osc[0x39] = 64; osc[0x3A] = 1; osc[0x60] = 12; osc[0x61] = uint8_t(-3);
    link.answer(link.sent[0], osc);
    Bytes ram(0x0BE06F50 + 232 - 0x0BE06F50, 0);
    ram[4 * 6 + 0] = 0xAB; ram[4 * 6 + 1] = 0xCD; ram[4 * 6 + 3] = 0x07;      // wave 7
    {
        edproto::Address a{edproto::kRawMemory, 0, 0, 0, edtables::kWaveList + 4, 232};
        Bytes r = edproto::partialWrite(a, ram); r[4] = 0; r[5] = edproto::kPartialReply;
        link.replies.push_back(r);
    }
    c.setWaveNames(editor::parseWaveNames("7\tChoir\r\n8\tBell\nx\ty\n"));
    c.tick(5100);
    v = c.view();
    CHECK(v.screen.rowText(0).find("7 Choir") != std::string::npos);
    c.command("section", "FILTER");
    link.answer(link.sent.back(), osc);
    c.tick(5110);
    v = c.view();
    CHECK(v.section == "FILTER" && v.screen.rowText(0).find("64") != std::string::npos && v.screen.rowText(0).find("High Pass") != std::string::npos);
    c.command("section", "ENV1");                                             // the stage table
    link.answer(link.sent.back(), osc);
    c.tick(5120);
    v = c.view();
    CHECK(v.screen.rowText(1).rfind("TIME", 0) == 0 && v.screen.rowText(1).find("12") != std::string::npos);
    CHECK(v.screen.rowText(2).find("-3") != std::string::npos);

    // Targets: S3 / O2 pick what is read and switch Follow off.
    link.sent.clear();
    c.command("target", "S3");
    c.command("target", "O2");
    v = c.view();
    CHECK(v.sound == 2 && v.osc == 1 && !v.follow);
    CHECK(link.sent.back() == edproto::partialRead({edproto::kSound, 2, 2, 1, 0x60, 2}));

    // Follow on: the panel's LEDs pick Sound 1 / OSC 1 and the envelope section (Filter LED -> Env 2).
    link.ledModes = {{0x0B, 1}, {0x06, 1}, {0x0C, 1}};
    c.command("toggle_follow", "");
    v = c.view();
    CHECK(v.follow && v.sound == 0 && v.osc == 0 && v.section == "ENV2" && !v.oscBoth);
    // Only a solid LED counts: the sound being edited is solid, the other active sounds flash. Both OSC LEDs solid:
    // both oscillators (oscillator 1's values). No solid LED: the target stays.
    link.ledModes = {{0x0B, 2}, {0x13, 2}, {0x03, 2}, {0x07, 1}, {0x06, 1}, {0x15, 1}, {0x11, 2}, {0x0C, 2}, {0x0E, 1}};
    link.leds += 1; c.tick(5200);
    v = c.view();
    CHECK(v.sound == 3 && v.osc == 0 && v.oscBoth && v.section == "ENV3");
    link.ledModes = {{0x0B, 2}, {0x13, 2}, {0x15, 1}};
    link.leds += 1; c.tick(5300);
    v = c.view();
    CHECK(v.sound == 3 && v.osc == 1 && !v.oscBoth && v.section == "ENV3");
    // PREV / NEXT stay within the section: a single-page section has neither, and they do nothing.
    CHECK(v.pages == 1 && !v.prev && !v.next);
    c.command("page_next", "");
    c.command("page_prev", "");
    CHECK(c.view().section == "ENV3" && c.view().page == 1);

    // Triggers: an LED burst -> one read 50 ms after the last LED message; a panel gesture likewise.
    link.sent.clear();
    link.leds += 5; c.tick(6000);
    link.leds += 3; c.tick(6020);
    c.tick(6060);
    CHECK(link.sent.empty());
    c.tick(6071);
    CHECK(link.sent.size() == 1);
    link.gestures += 1; c.tick(6100);
    c.tick(6149);
    CHECK(link.sent.size() == 1);
    c.tick(6151);
    CHECK(link.sent.size() == 2);
    // A display change with no LED burst: one read 600 ms later; with a burst: the burst's read only.
    link.disp = {{'P', ' ', '0', '2'}}; c.tick(7000);
    c.tick(7500);
    CHECK(link.sent.size() == 2);
    c.tick(7601);
    CHECK(link.sent.size() == 3);
    link.disp = {{'P', ' ', '0', '3'}}; c.tick(8000);
    link.leds += 2; c.tick(8350);
    c.tick(8401);
    c.tick(9500);
    CHECK(link.sent.size() == 4);
    // The standing read is always the visible page's read.
    CHECK(link.standing == c.pageReadsForTest());

    // Presets: names, 8 per view; a reply per name; and the current preset's name (the edit buffer's).
    link.sent.clear();
    c.command("area", "PRESETS");
    CHECK(link.sent.size() == 9 && link.sent[8] == edproto::partialRead({edproto::kPreset, 0, 0, 0, 8, 16}));
    CHECK(link.sent[3] == edproto::partialRead({edproto::kPreset, 0x43, 0, 0, 8, 16}));   // stored preset 3 (0x40 + n)
    Bytes preset(24, ' ');
    const char* nm = "Big Pad 4       ";
    std::memcpy(preset.data() + 8, nm, 16);
    link.answer(link.sent[3], preset);
    c.tick(9600);
    v = c.view();
    CHECK(v.pages == 8 && v.screen.rowText(1).find("04  Big Pad 4") != std::string::npos);

    // Closed: nothing more is sent, whatever happens.
    c.setOpen(false);
    CHECK(!link.open);
    link.sent.clear();
    link.leds += 9; link.gestures += 9; link.disp = {{'P', ' ', '0', '9'}};
    for (int ms = 10000; ms < 12000; ms += 10) c.tick(ms);
    c.command("page_next", "");
    CHECK(link.sent.empty());
    std::printf("editor_test: controller (navigation, reads, Follow, triggers, no polling, closed = silent)\n");
}
}  // namespace

int main() {
    testProto();
    testTables();
    testPages();
    testController();
    std::printf("editor_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
