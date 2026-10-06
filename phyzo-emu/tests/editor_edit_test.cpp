// Editor engine test, part 3 (no ROM data): editing, COMMON and ARP (docs/editor/EDITOR_ROUND2.md section 7).
//  1. Messages: the whole-object dump (the layout of the OS's own reply, checksum), its parser, NRPN with signed
//     values, program changes, the OS's print formats.
//  2. Fields: COMMON and ARP offsets, NRPN numbers, stored <-> shown conversions.
//  3. Editing on synthetic tables with a fake engine link, every verified write path: oscillator NRPN (signed, bit
//     fields), the wave by SysEx (never NRPN), sound settings and system partial writes (offset byte), bit fields
//     read-modify-write, COMMON NRPN MSB 1, ARP whole-preset dumps (fresh whole read + live arp state folded in, then
//     the edits; exclusive intervals; both key-range pairs; coalescing, one dump in flight), loading an arp preset
//     (keeping held, keyboard and the key ranges), program changes from the preset list; pacing and coalescing of
//     drags; clamping; display-only fields send nothing; typed entry; keys; the right-click lists; revert; EDITED;
//     read-back after the last write; Follow on panel knobs; nothing at all while closed.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "editor/editor_controller.h"
#include "editor/editor_fields.h"
#include "editor/editor_pages.h"
#include "editor/editor_proto.h"
#include "editor/editor_tables.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); ++failures; } } while (0)
using Bytes = std::vector<uint8_t>;
using namespace edtables;


void testMessages() {
    // The dump: header, length, the object as encoded words, checksum; the first words as in the OS's own reply.
    Bytes obj(edfields::kPresetSize, 0);
    obj[2] = 0x01; obj[3] = 0x9E;                                         // its own size word: 414
    obj[4] = 'P'; obj[5] = 'S'; obj[6] = 'T'; obj[7] = '1';               // its tag word
    for (size_t i = 8; i < obj.size(); ++i) obj[i] = uint8_t(i * 7);
    const Bytes d = edproto::wholeDump(edproto::kPreset, 0, obj);
    CHECK(d.size() == 537);
    const Bytes head = {0xF0, 0x0F, 0x11, 0x01, 0x7F, 0x02, 0x07, 0x00, 0x00, 0x1E, 0x03, 0x00, 0x00, 0x00,
                        0x1E, 0x03, 0x00, 0x00, 0x00, 0x31, 0x28, 0x4D, 0x02, 0x05};
    CHECK(Bytes(d.begin(), d.begin() + long(head.size())) == head);
    uint32_t sum = 0;
    for (size_t i = 14; i < d.size() - 3; ++i) sum += d[i];
    CHECK(edproto::get14(&d[d.size() - 3]) == (sum & 0x3FFF) && d.back() == 0xF7);
    uint8_t type; uint16_t number; Bytes back;
    CHECK(edproto::parseWhole(d.data(), d.size(), type, number, back) && type == 7 && number == 0 && back == obj);
    Bytes reply = d;                                                      // the OS's reply: id 00, command 41
    reply[4] = 0x00; reply[5] = edproto::kWholeReply;
    CHECK(edproto::parseWhole(reply.data(), reply.size(), type, number, back) && back == obj);
    reply[100] ^= 0x01;                                                   // a damaged byte: the checksum fails
    CHECK(!edproto::parseWhole(reply.data(), reply.size(), type, number, back));
    // The example from the OS (only the first and last bytes are given): the tail's checksum is two 7-bit bytes.
    CHECK(edproto::get14((const uint8_t*)"\x6B\x47") == 0x23EB);

    // NRPN with a signed value: -50 as 14-bit two's complement, all four controllers, then the null.
    const auto n = edproto::nrpn(0, 2, 32, -50);
    CHECK(n.size() == 6 && n[2] == Bytes({0xB0, 0x06, 0x7F}) && n[3] == Bytes({0xB0, 0x26, 0x4E}) && n[5] == Bytes({0xB0, 0x62, 0x7F}));
    CHECK(edproto::programChange(2, 17) == Bytes({0xC2, 17}));
    CHECK_EQ(formatNumber("Oct%d", 2), std::string("Oct2"));
    CHECK_EQ(formatNumber("t%3d", 95), std::string("t 95"));
    CHECK_EQ(formatNumber("AP%02d", 7), std::string("AP07"));
    CHECK_EQ(formatNumber("100%%", 1), std::string("100%"));
    std::printf("editor_edit_test: dump layout and checksum, signed NRPN, program change, formats\n");
}

ArpText arpText() {
    ArpText t;
    t.ok = true;
    t.on = " On"; t.off = "OFF"; t.hold = "HELd"; t.centre = "CEnt"; t.low = "Lo n"; t.high = "Hi n"; t.duration = "ndur";
    t.limit = "ntLt"; t.feel = "FEEL"; t.touch = " Tch"; t.resolutionCode = "nrES"; t.tempoFmt = "t%3d"; t.rangeFmt = "Oct%d";
    t.presetCode = "PrSt"; t.ownSettings = " Snd"; t.arpPresetFmt = "AP%02d";
    t.intervals = {"-3rd", "3rd", "4th", "5th", "Oct"};
    t.resolution = {"1/1", "1/2", "1/4", "1/8", "16th", "32nd", "32T", "16T", "8T", "4T", "2T", "1T"};
    return t;
}

void testFields() {
    using namespace edfields;
    const std::vector<edfields::Field> c = commonFields({"P", "H", "", "Z", "O"});
    CHECK(c.size() == 16 + 5 + 6);
    CHECK(c[4].label == "MODE" && c[4].slot == 1 && c[4].offset == 38 && c[4].nrpnLsb == 11);   // slot 2: B 36, L 9
    CHECK(c[7].label == "HIGH" && c[7].offset == 37 && c[7].nrpnLsb == 10);
    CHECK(c[16].label == "P" && c[16].offset == 34 && c[16].nrpnLsb == 7 && c[18].label == "PERF3" && c[20].offset == 50 && c[20].nrpnLsb == 21);
    CHECK(c[21].label == "MOD SOURCE" && !c[21].editable && c[23].label == "SRC MIN" && c[24].label == "SRC MAX" && c[24].offset == 68 && c[26].label == "DEST MAX");
    const ArpText t = arpText();
    const std::vector<edfields::Field> a = arpFields(t);
    CHECK(a.size() == 22);
    CHECK(a[0].label == "ARP" && a[0].offset == 383 && a[0].onRaw == 2 && a[1].label == "HELd" && a[2].kind == Kind::Inverted);
    CHECK(a[11].label == "Lo n" && a[11].offset == 370 && a[11].mirror == 372 && a[12].mirror == 373);
    // Conversions
    edfields::Field dur = a[14];
    CHECK(dur.label == "ndur" && toRaw(dur, 50) == 128 && toDomain(dur, 128) == 50 && toRaw(dur, 0) == 1 && toDomain(dur, 255) == 99);
    for (int s = 0; s <= 99; ++s) CHECK(toDomain(dur, toRaw(dur, s)) == s);   // the smallest raw that shows s
    edfields::Field down = a[10];
    uint8_t buf[64] = {};
    store(down, toRaw(down, 2), buf, 64, kArpBlock);
    int64_t raw;
    CHECK(buf[368 - 350] == 0xE8 && rawOf(down, buf, 64, kArpBlock, raw) && raw == -24 && toDomain(down, raw) == 2);
    CHECK_EQ(format(down, 2, t), std::string("Oct2"));
    CHECK(toRaw(a[2], 1) == 0 && toRaw(a[2], 0) == 1 && format(a[2], 1, t) == "On");   // keyboard: 0 = ON
    edfields::Field res = a[16];
    CHECK(res.kind == Kind::Resolution && toDomain(res, 96) == 4 && toRaw(res, 4) == 96 && format(res, 4, t) == "16th" && toDomain(res, 1000) == -1);
    store(res, 384, buf, 64, kArpBlock);
    CHECK(buf[52] == 0 && buf[53] == 0 && buf[54] == 0x01 && buf[55] == 0x80);                  // 4 bytes, big-endian
    CHECK_EQ(format(a[17], 120, t), std::string("t120"));
    CHECK_EQ(format(a[18], kNone16, t), std::string("OFF"));
    CHECK(a[19].kind == Kind::PresetAction && format(a[19], 0, t) == "Snd" && arpPresetLabel(t, 0) == "AP01");
    std::printf("editor_edit_test: COMMON and ARP fields\n");
}

// ---- controller with a fake engine link
struct FakeLink : editor::Link {
    bool open = false, run = true;
    std::vector<Bytes> sent, standing;
    std::vector<Bytes> replies;
    uint64_t leds = 0, triggers = 0, gestures = 0, moves = 0;
    int lastControl = -1;
    std::map<int, int> ledModes;
    std::array<char, 4> disp{{'P', ' ', '0', '1'}};
    void setOpen(bool on) override { open = on; }
    bool send(const Bytes& m) override { sent.push_back(m); return true; }
    void setStandingRead(const std::vector<Bytes>& m) override { standing = m; }
    bool popReply(Bytes& m) override { if (replies.empty()) return false; m = replies.front(); replies.erase(replies.begin()); return true; }
    uint64_t ledStateMessages() const override { return leds; }
    uint64_t presetTriggers() const override { return triggers; }
    uint64_t panelGestures() const override { return gestures; }
    uint64_t panelControlMoves() const override { return moves; }
    int lastPanelControl() const override { return lastControl; }
    int ledMode(int code) const override { auto it = ledModes.find(code); return it == ledModes.end() ? 0 : it->second; }
    std::array<char, 4> display() const override { return disp; }
    bool running() const override { return run; }
    static edproto::Address addressOf(const Bytes& request) {
        edproto::Address a;
        a.type = request[6]; a.number = uint16_t(edproto::get14(&request[7])); a.subtype = request[9];
        a.subnumber = uint16_t(edproto::get14(&request[10])); a.offset = edproto::get32(&request[12]); a.length = edproto::get32(&request[17]);
        return a;
    }
    // The OS's answer to a partial read: `data` at the requested address (or the range from `object`).
    void answerData(const Bytes& request, const Bytes& data) {
        Bytes r = edproto::partialWrite(addressOf(request), data);
        r[4] = 0x00; r[5] = edproto::kPartialReply;
        replies.push_back(r);
    }
    void answer(const Bytes& request, const Bytes& object) {
        const edproto::Address a = addressOf(request);
        Bytes data;
        for (uint32_t i = 0; i < a.length; ++i) data.push_back(a.offset + i < object.size() ? object[a.offset + i] : 0);
        answerData(request, data);
    }
    void answerWhole(const Bytes& object) {
        Bytes r = edproto::wholeDump(edproto::kPreset, 0, object);
        r[4] = 0x00; r[5] = edproto::kWholeReply;
        replies.push_back(r);
    }
    bool isRead(const Bytes& m, uint8_t type, uint32_t offset) const {
        return m.size() > 20 && m[5] == edproto::kPartialRead && addressOf(m).type == type && addressOf(m).offset == offset;
    }
    // The last whole-object dump sent (-1: none).
    int lastDump() const { for (size_t i = sent.size(); i-- > 0;) if (sent[i].size() > 6 && sent[i][5] == edproto::kWholeDump) return int(i); return -1; }
    int dumps() const { int n = 0; for (const Bytes& m : sent) n += m.size() > 6 && m[5] == edproto::kWholeDump; return n; }
    int find(const Bytes& m) const { for (size_t i = 0; i < sent.size(); ++i) if (sent[i] == m) return int(i); return -1; }
    bool any(uint8_t status) const { for (const Bytes& m : sent) if ((m[0] & 0xF0) == status) return true; return false; }
};

Field mkField(const std::string& name, uint32_t offset, uint16_t type, int min, int max, uint16_t fmt = 0, int list = -1) {
    Field f;
    f.name = name; f.offset = offset; f.type = type; f.min = int16_t(min); f.max = int16_t(max); f.format = fmt;
    if (list >= 0) { f.display = Display::List; f.listIndex = list; }
    f.hidden = isHiddenName(name);
    return f;
}

OsTables tables() {
    OsTables t;
    t.ok = true;
    t.arp = arpText();
    for (int i = 0; i < 8; ++i) t.system.fields.push_back(mkField("S" + std::to_string(i), uint32_t(i), kUnsignedByte, 0, 127));
    t.system.fields.push_back(mkField("NRPN", 25, kBitField, (1 << 8) | 5, 1));   // a flag: bit 5 of byte 25
    for (int i = 0; i < edpages::kOscillatorFields; ++i) t.oscillator.fields.push_back(mkField("SPARE " + std::to_string(i), 0xF0, kUnsignedByte, 0, 1));
    t.oscillator.fields[51] = mkField("Wave ID", 0x4C, kLong, 0, -1);
    t.oscillator.fields[30] = mkField("Filter Mode", 0x3A, kUnsignedByte, 0, 1);
    t.oscillator.fields[32] = mkField("Filter Cutoff", 0x39, kUnsignedByte, 0, 127);
    t.oscillator.fields[37] = mkField("Filter Kbd Amount", 0x3B, kSignedByte, -64, 63);
    t.oscillator.fields[14] = mkField("Fine Tune", 0x20, kSignedByte, -64, 63);
    t.oscillator.fields[15] = mkField("PitchBend Enable", 0x22, kBitField, (1 << 8) | 0, 1);
    t.oscillator.fields[16] = mkField("Trigger Key", 0x23, kSmallUnsigned, 0, 15);   // value type 5: display only
    t.oscillator.fields[111] = mkField("Env3 Level (1)", 0x75, kSignedByte, -99, 99);
    for (size_t i = 0; i < t.oscillator.fields.size(); ++i) t.oscillator.fields[i].index = int(i);
    t.soundSettings.fields = {mkField("Volume", 4, kUnsignedByte, 0, 127), mkField("Semitone Shift", 8, kOffsetByte, -64, 63),
                              mkField("Rcv Sustain", 7, kBitField, (1 << 8) | 3, 1)};
    t.soundCommon.fields = {mkField("Glide Mode", 24, kUnsignedByte, 0, 3)};
    return t;
}

// The value span of a reference on the current screen.
const edpages::Screen::Span* spanOf(const editor::View& v, int ref) {
    for (const auto& s : v.screen.values) if (s.ref == ref) return &s;
    return nullptr;
}

void testEditing() {
    FakeLink link;
    editor::Controller c(link);
    c.setTables(tables());
    c.setKnobLabels({"P", "H", "Y", "Z", "O"});
    c.setFollowLeds({0x0B, 0x13, 0x03, 0x07}, {0x06, 0x15}, {0x11, 0x0C, 0x0E});
    c.setFollowExtraLeds(0x02, 0x0F, 0x08);
    double now = 0;
    auto tick = [&](double ms) { now += ms; c.tick(now); };
    // Closed: an edit does nothing.
    const int cutoff = c.refForTest('o', 32);
    c.command("drag_begin", std::to_string(cutoff));
    c.command("drag", "5");
    tick(20);
    CHECK(link.sent.empty());
    CHECK((c.view().areas == std::set<std::string>{"SYSTEM", "PRESETS", "COMMON", "SOUND", "OSC", "ARP"}));

    c.setOpen(true);
    tick(10);
    // ---- oscillator: NRPN, paced and coalesced while dragging, clamped, read back after the last write
    c.command("area", "OSC");
    c.command("section", "FILTER");
    Bytes osc(0x100, 0);
    osc[0x39] = 64; osc[0x3A] = 1; osc[0x3B] = 10; osc[0x20] = 0; osc[0x22] = 0xFF; osc[0x23] = 3;
    Bytes ram(232, 0);
    ram[4 * 6] = 0xAB; ram[4 * 6 + 1] = 0xCD; ram[4 * 6 + 3] = 0x07;          // wave 7
    ram[4 * 12] = 0x12; ram[4 * 12 + 3] = 0x34;                                 // wave 13
    osc[0x4C] = 0xAB; osc[0x4D] = 0xCD; osc[0x4F] = 0x07;
    for (const Bytes& m : link.sent) {
        if (link.isRead(m, edproto::kRawMemory, kWaveList + 4)) link.answerData(m, ram);   // the OS's wave list, read once
        else if (m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kSound) link.answer(m, osc);
    }
    tick(10);
    editor::View v = c.view();
    CHECK(v.section == "FILTER" && spanOf(v, cutoff) != nullptr);
    link.sent.clear();
    c.command("drag_begin", std::to_string(cutoff));
    CHECK(c.view().focusRow >= 0 && c.focusForTest() == cutoff);
    c.command("drag", "5");
    const auto n69 = edproto::nrpn(0, 2, 32, 69);
    CHECK(link.sent.size() == 6 && Bytes(link.sent[3]) == n69[3]);              // sent at once (nothing paced yet)
    c.command("drag", "6");                                                    // within 10 ms: waits, latest wins
    c.command("drag", "7");
    CHECK(link.sent.size() == 6);
    CHECK(c.view().screen.rowText(spanOf(c.view(), cutoff)->row).find("71") != std::string::npos);   // shown at once
    tick(10);
    CHECK(link.sent.size() == 12 && link.sent[9] == edproto::nrpn(0, 2, 32, 71)[3]);
    c.command("drag", "-500");                                                 // clamped to the field's range
    tick(10);
    CHECK(link.sent.size() == 18 && link.sent[15] == edproto::nrpn(0, 2, 32, 0)[3]);
    c.command("drag_end", "");
    const size_t before = link.sent.size();
    tick(20);
    CHECK(link.sent.size() == before);                                          // read back after kReadBackMs
    tick(60);
    CHECK(link.sent.size() == before + 1 && link.sent.back()[5] == edproto::kPartialRead);
    CHECK(c.view().edited);
    // Signed by typed entry; a bit field by NRPN (its own value).
    c.command("section", "PITCH");
    for (const Bytes& m : link.sent) if (m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kSound) link.answer(m, osc);
    tick(10);
    link.sent.clear();
    const int fine = c.refForTest('o', 14), bend = c.refForTest('o', 15), trig = c.refForTest('o', 16);
    c.command("type", std::to_string(fine));
    for (const char* k : {"-", "5", "0"}) c.key(k);
    CHECK(c.view().typing && c.view().screen.rowText(spanOf(c.view(), fine)->row).find("-50_") != std::string::npos);
    c.key("enter");
    CHECK(!c.view().typing && link.find(edproto::nrpn(0, 2, 14, -50)[3]) >= 0 && link.find(edproto::nrpn(0, 2, 14, -50)[2]) >= 0);
    tick(20);
    link.sent.clear();
    c.choose(bend, 0);
    CHECK(link.find(edproto::nrpn(0, 2, 15, 0)[3]) >= 0);
    tick(20);
    link.sent.clear();
    c.command("drag_begin", std::to_string(trig));                           // value type 5: display only
    c.command("drag", "3");
    c.command("drag_end", "");
    c.key("plus");
    tick(200);
    CHECK(!link.any(0xB0) && link.find(edproto::partialWrite({edproto::kSound, 0, 2, 0, 0x23, 1}, {6})) < 0);
    CHECK(c.menu(trig).items.empty() && c.menu(trig).groups.empty());
    // Typed entry: Esc cancels (nothing sent).
    link.sent.clear();
    c.command("type", std::to_string(fine));
    c.key("9");
    c.key("escape");
    tick(200);
    CHECK(!c.view().typing && !link.any(0xB0));

    // ---- the wave: SysEx partial write of the 4-byte ID from the OS's wave list (never NRPN)
    c.command("section", "WAVE");
    for (const Bytes& m : link.sent) if (m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kSound) link.answer(m, osc);
    tick(10);
    link.sent.clear();
    const int wave = c.refForTest('o', 51);
    c.choose(wave, 13);
    CHECK(link.sent.size() == 1 && link.sent[0] == edproto::partialWrite({edproto::kSound, 0, 2, 0, 0x4C, 4}, {0x12, 0x00, 0x00, 0x34}));
    CHECK(!link.any(0xB0));
    const editor::Menu wm = c.menu(wave);
    CHECK(wm.items.empty() && wm.groups.size() == 6 && wm.groups[1].second.size() == 10 && wm.groups[1].second[2].current);   // 58 waves by tens; 13 ticked (shown at once)
    tick(200);

    // ---- sound settings: partial writes (offset byte stored + 64); a bit field read-modify-write; revert
    c.command("area", "SOUND");
    Bytes track(16, 0);
    track[4] = 90; track[7] = 0x5F; track[8] = 64;
    for (const char* sec : {"TRACK", "RANGE", "RECEIVE"}) {
        c.command("section", sec);
        for (const Bytes& m : link.sent) if (m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kPerformance) link.answer(m, track);
        tick(10);
    }
    const int vol = c.refForTest('p', 0), semi = c.refForTest('p', 1), sus = c.refForTest('p', 2);
    link.sent.clear();
    c.choose(vol, 100);
    CHECK(link.sent.size() == 1 && link.sent[0] == edproto::partialWrite({edproto::kPerformance, edproto::kEditBuffer, 1, 0, 4, 1}, {100}));
    tick(20);
    c.choose(semi, -12);
    CHECK(link.sent.back() == edproto::partialWrite({edproto::kPerformance, edproto::kEditBuffer, 1, 0, 8, 1}, {52}));
    tick(20);
    c.choose(vol, 500);                                                        // clamped
    CHECK(link.sent.back() == edproto::partialWrite({edproto::kPerformance, edproto::kEditBuffer, 1, 0, 4, 1}, {127}));
    editor::Menu vm = c.menu(vol);
    CHECK(vm.revert && vm.revertLabel == "90" && vm.items.empty());
    tick(20);
    c.revert(vol);
    CHECK(link.sent.back() == edproto::partialWrite({edproto::kPerformance, edproto::kEditBuffer, 1, 0, 4, 1}, {90}));
    CHECK(!c.menu(vol).revert);
    tick(20);
    link.sent.clear();
    c.choose(sus, 0);                                                          // bit 3 of byte 7: read it first
    CHECK(link.sent.size() == 1 && link.sent[0] == edproto::partialRead({edproto::kPerformance, edproto::kEditBuffer, 1, 0, 7, 1}));
    link.answerData(link.sent[0], {0x7F});                                     // the OS holds 0x7F now (the panel changed a flag)
    tick(10);
    CHECK(link.sent.back() == edproto::partialWrite({edproto::kPerformance, edproto::kEditBuffer, 1, 0, 7, 1}, {0x77}));
    CHECK(c.menu(sus).items.size() == 2);
    tick(200);

    // ---- system: partial write; its bit field read-modify-write
    c.command("area", "SYSTEM");
    Bytes sys(32, 0);
    sys[25] = 0x5F;
    for (const Bytes& m : link.sent) if (m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kSystem) link.answer(m, sys);
    tick(10);
    link.sent.clear();
    c.choose(c.refForTest('s', 3), 49);
    CHECK(link.sent.back() == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 3, 1}, {49}));
    tick(20);
    c.choose(c.refForTest('s', 8), 1);
    link.answerData(link.sent.back(), {0x5F});
    tick(10);
    CHECK(link.sent.back() == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 25, 1}, {0x7F}));
    tick(200);
    // Keys: arrows move the focus between values, +/- step it.
    c.command("focus", std::to_string(c.refForTest('s', 0)));
    CHECK(c.key("right") && c.focusForTest() == c.refForTest('s', 1));
    CHECK(c.key("down") && c.focusForTest() == c.refForTest('s', 5));
    link.sent.clear();
    CHECK(c.key("plus"));
    CHECK(link.sent.size() == 1 && link.sent[0] == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 5, 1}, {1}));
    tick(200);

    // ---- COMMON: the slot table as approved, NRPN MSB 1
    c.command("area", "COMMON");
    Bytes preset(edfields::kPresetSize, 0);
    preset[28 + 2] = 1; preset[28 + 3] = 3; preset[28 + 0] = 0; preset[28 + 1] = 127;   // slot 1: LAYER, OSC 1+2, C-1 - G9
    preset[36 + 2] = 2; preset[36 + 3] = 1; preset[36 + 0] = 60; preset[36 + 1] = 83;   // slot 2: SPLIT, OSC 1, C4 - B5
    preset[34] = 64; preset[35] = 0; preset[42] = 127; preset[43] = 64; preset[50] = 32;
    preset[66] = 0xFF; preset[67] = 0x9C;                                                // SRC MIN -100
    const char* name = "Arp Settings 1  ";
    std::memcpy(&preset[334], name, 16);
    auto answerPreset = [&]() {
        for (const Bytes& m : link.sent) if (m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kPreset && FakeLink::addressOf(m).number == 0) link.answer(m, preset);
    };
    answerPreset();
    tick(10);
    v = c.view();
    CHECK(v.section == "SLOTS");
    CHECK_EQ(v.screen.rowText(0), std::string("S1  MODE LAYER  OSC 1+2      LOW C-1  HIGH G9     "));
    CHECK_EQ(v.screen.rowText(1), std::string("S2  MODE SPLIT  OSC 1        LOW C4   HIGH B5     "));
    link.sent.clear();
    const int mode2 = c.refForTest('m', 4);
    CHECK(c.menu(mode2).items.size() == 4 && c.menu(mode2).items[2].current);
    c.choose(mode2, 3);
    CHECK(link.sent.size() == 6 && link.sent[0] == Bytes({0xB0, 0x63, 1}) && link.sent[1] == Bytes({0xB0, 0x62, 11}) && link.sent[3] == Bytes({0xB0, 0x26, 3}));
    tick(200);
    c.command("section", "KNOBS");
    answerPreset();
    tick(10);
    v = c.view();
    CHECK_EQ(v.screen.rowText(0), std::string("P                     64 H                      0 "));
    CHECK_EQ(v.screen.rowText(2), std::string("O                     32                          "));
    c.command("section", "MOD");
    answerPreset();
    tick(10);
    v = c.view();
    CHECK(v.screen.rowText(1).find("SRC MIN") == 0 && v.screen.rowText(1).find("-100") != std::string::npos);
    link.sent.clear();
    c.command("drag_begin", std::to_string(c.refForTest('m', 21)));             // insert modulation: display only
    c.command("drag", "4");
    tick(200);
    CHECK(link.sent.empty() || !link.any(0xB0));

    // ---- ARP: values from the live arp state; edits by whole-preset dump
    link.sent.clear();
    c.command("area", "ARP");
    Bytes live(64, 0);
    live[383 - 350] = 2; live[351 - 350] = 0; live[352 - 350] = 0; live[362 - 350] = 1;  // on, held off, keyboard on, 3rd
    live[367 - 350] = 12; live[368 - 350] = 0; live[370 - 350] = 0; live[371 - 350] = 127;
    live[372 - 350] = 0; live[373 - 350] = 127; live[385 - 350] = 128; live[386 - 350] = 0;
    live[392 - 350] = 0xFF; live[393 - 350] = 0xFF; live[390 - 350] = 0x00; live[391 - 350] = 0x05;
    live[402 - 350 + 3] = 96; live[406 - 350 + 3] = 120;
    for (const Bytes& m : link.sent) {
        if (link.isRead(m, edproto::kRawMemory, edfields::kLiveArp)) link.answerData(m, live);
    }
    tick(10);
    v = c.view();
    CHECK_EQ(v.screen.rowText(0), std::string("ARP  On     HELD OFF      KBD  On     CENT OFF    "));
    CHECK_EQ(v.screen.rowText(1), std::string("-3RD OFF    3RD  On       4TH  OFF    5TH  OFF    "));
    CHECK_EQ(v.screen.rowText(2), std::string("OCT  OFF    UP   Oct1     DOWN Oct0   LO N C-1    "));
    CHECK_EQ(v.screen.rowText(3), std::string("HI N G9     TCH  OFF      NDUR 50     NTLT 0      "));
    // The pattern's name is read once.
    bool patternRead = false;
    for (const Bytes& m : link.sent) patternRead |= m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kArpPattern && FakeLink::addressOf(m).number == 5;
    CHECK(patternRead);
    // Two edits while the first dump's reads are out: one dump with both, and the -3rd interval clears the 3rd.
    link.sent.clear();
    const int minus3 = c.refForTest('a', 4), lo = c.refForTest('a', 11), hold = c.refForTest('a', 1);
    c.choose(minus3, 1);
    CHECK(link.sent.size() == 2 && link.sent[0] == edproto::wholeRead(edproto::kPreset, 0) && link.isRead(link.sent[1], edproto::kRawMemory, edfields::kLiveArp));
    c.choose(lo, 48);
    c.choose(hold, 1);
    tick(10);
    CHECK(link.sent.size() == 2);                                              // one dump in flight
    Bytes buffer = preset;                                                     // the edit buffer: a panel change meanwhile
    buffer[30] = 3;
    for (size_t i = 350; i < 414; ++i) buffer[i] = 0xEE;                      // the stored arp block (not the live state)
    link.answerWhole(buffer);
    Bytes live2 = live;
    live2[386 - 350] = 9;                                                      // the panel's arp edit in the live state
    link.answerData(link.sent[1], live2);
    tick(10);
    CHECK(link.dumps() == 1);
    const Bytes& dump1 = link.sent[size_t(std::max(0, link.lastDump()))];
    uint8_t type; uint16_t number; Bytes dumped;
    CHECK(edproto::parseWhole(dump1.data(), dump1.size(), type, number, dumped) && dump1[4] == 0x7F && type == 7 && number == 0);
    CHECK(dumped.size() == 414 && dumped[30] == 3 && dumped[34] == 64);        // the rest of the edit buffer kept
    CHECK(dumped[386] == 9 && dumped[383] == 2);                               // the live state folded in
    CHECK(dumped[361] == 1 && dumped[362] == 0 && dumped[351] == 1);           // -3rd on clears 3rd; held on
    CHECK(dumped[370] == 48 && dumped[372] == 48 && dumped[371] == 127);       // both key-range pairs
    v = c.view();
    CHECK(v.screen.rowText(1).find("-3RD On") == 0 && v.screen.rowText(3).find("NTLT 9") != std::string::npos);
    // Read back after the dump went out.
    tick(400);
    // Load an arp preset: its body, keeping held, keyboard and both key ranges.
    link.sent.clear();
    const int prst = c.refForTest('a', 19);
    const editor::Menu pm = c.menu(prst);
    CHECK(pm.groups.size() == 7 && pm.groups[0].second.size() == 20 && pm.groups[0].second[0].label == "AP01");
    c.choose(prst, 22);
    CHECK(link.sent.size() == 3 && link.sent[2] == edproto::partialRead({edproto::kArpPreset, 22, 0, 0, 24, 64}));
    link.answerWhole(dumped);
    Bytes live3(dumped.begin() + 350, dumped.end());
    link.answerData(link.sent[1], live3);
    Bytes body(64, 0x33);
    link.answerData(link.sent[2], body);
    tick(10);
    CHECK(link.dumps() == 1 && link.lastDump() >= 0);
    if (link.lastDump() >= 0) CHECK(edproto::parseWhole(link.sent[size_t(link.lastDump())].data(), link.sent[size_t(link.lastDump())].size(), type, number, dumped));
    CHECK(dumped[350] == 0x33 && dumped[383] == 0x33 && dumped[351] == 1 && dumped[352] == 0);
    CHECK(dumped[370] == 48 && dumped[371] == 127 && dumped[372] == 48 && dumped[373] == 127 && dumped[30] == 3);
    tick(400);
    // Display-only ARP fields send nothing.
    link.sent.clear();
    c.command("page_next", "");
    c.command("drag_begin", std::to_string(c.refForTest('a', 18)));            // feel
    c.command("drag", "5");
    tick(400);
    for (const Bytes& m : link.sent) CHECK(m[5] != edproto::kWholeDump && m[5] != edproto::kWholeRead);

    // ---- PRESETS: a click on a name sends its program change; a preset change ends EDITED and revert
    c.command("area", "PRESETS");
    link.sent.clear();
    int presetRef = -1;
    for (const auto& s : c.view().screen.values) if (s.pick && presetRef < 0) presetRef = s.ref;
    CHECK(presetRef >= 0);
    link.sent.clear();
    c.command("pick", std::to_string(presetRef));
    CHECK(link.sent.size() == 1 && link.sent[0] == edproto::programChange(0, 0));
    CHECK(c.view().edited);
    link.triggers += 1;
    tick(10);
    CHECK(!c.view().edited);

    // ---- Follow on panel knobs
    link.lastControl = 22; link.moves += 1;                                    // Cutoff: OSC FILTER, focused
    tick(10);
    v = c.view();
    CHECK(v.area == "OSC" && v.section == "FILTER" && c.focusForTest() == cutoff);
    link.ledModes[0x08] = 1;                                                   // Filter Keyboard lit: Filter Amount -> 37
    link.lastControl = 4; link.moves += 1;
    tick(10);
    CHECK(c.focusForTest() == c.refForTest('o', 37));
    link.ledModes[0x0E] = 1;                                                   // Amp envelope: Sustain -> 86 + 32 ... Attack 79 + 32
    link.lastControl = 17; link.moves += 1;
    tick(10);
    CHECK(c.view().section == "ENV3" && c.focusForTest() == c.refForTest('o', 111));
    link.lastControl = 13; link.moves += 1;                                    // perf2: COMMON KNOBS, H
    tick(10);
    CHECK(c.view().area == "COMMON" && c.view().section == "KNOBS" && c.focusForTest() == c.refForTest('m', 17));
    link.lastControl = 14; link.moves += 1;                                    // Arp Value: ARP
    tick(10);
    CHECK(c.view().area == "ARP");
    link.lastControl = 11; link.moves += 1;                                    // Volume: no jump
    tick(10);
    CHECK(c.view().area == "ARP");
    link.gestures += 1;                                                         // a button press is not a knob move
    link.lastControl = 22;
    tick(10);
    CHECK(c.view().area == "ARP");
    c.command("toggle_follow", "");                                            // Follow off: no jumps
    link.moves += 1;
    tick(10);
    CHECK(c.view().area == "ARP");

    // ---- closed: nothing at all
    c.setOpen(false);
    link.sent.clear();
    c.choose(vol, 10);
    c.choose(minus3, 0);
    c.key("plus");
    for (int i = 0; i < 50; ++i) tick(20);
    CHECK(link.sent.empty());
    std::printf("editor_edit_test: editing (write paths, pacing, dumps, lists, keys, revert, Follow on knobs)\n");
}
}  // namespace

int main() {
    testMessages();
    testFields();
    testEditing();
    std::printf("editor_edit_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
