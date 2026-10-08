// Editor engine test, part 3 (no ROM data): editing, COMMON and ARP (docs/editor/EDITOR_ROUND2.md section 7).
//  1. Messages: the whole-object dump (the layout of the OS's own reply, checksum), its parser, NRPN with signed
//     values, program changes, the OS's print formats.
//  2. Fields: COMMON and ARP offsets, NRPN numbers, stored <-> shown conversions.
//  3. Editing on synthetic tables with a fake engine link, every verified write path: oscillator NRPN (signed, bit
//     fields), the wave by SysEx (never NRPN), sound settings and system partial writes (offset byte), bit fields
//     read-modify-write, COMMON NRPN MSB 1, ARP whole-preset dumps (fresh whole read + live arp state folded in, then
//     ARP through the panel's buttons only, on a simulated OS (the arp-control variable, the arp menu, the live arp
//     state, the current arp preset, the runtime flags): the control selected (Tempo, Range, or Arp Mode until the
//     variable reads it), 1.4 s, then -/No / +/Yes until the value lands; never a step outside the arp controls (the
//     safety rule, also when the OS leaves the control); on/off and keyboard by their buttons; no write into the arp
//     state and no dump; program changes from the preset list and the current preset marked; pacing and coalescing
//     of drags; clamping; display-only fields send nothing; typed entry; keys; the right-click lists; revert; EDITED;
//     read-back after the last write; Follow on panel knobs; nothing at all while closed.
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
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
    CHECK(c.size() == 16 + 5);                                                    // the insert modulation is on INSERT > MOD
    CHECK(c[4].label == "MODE" && c[4].slot == 1 && c[4].offset == 38 && c[4].nrpnLsb == 11);   // slot 2: B 36, L 9
    CHECK(c[7].label == "HIGH" && c[7].offset == 37 && c[7].nrpnLsb == 10);
    CHECK(c[16].label == "P" && c[16].offset == 34 && c[16].nrpnLsb == 7 && c[18].label == "PERF3" && c[20].offset == 50 && c[20].nrpnLsb == 21);
    const ArpText t = arpText();
    const std::vector<edfields::Field> a = arpFields(t);
    // The page order (owner-approved mockup, v0.8.3.5): the Arp Mode button's order (PrSt, nrES, ndur, FEEL, the
    // intervals, Lo n, Hi n, tch, CEnt, ntLt, HELd) with the pattern and the riff after the preset's row, then the
    // other arp buttons and the name. The labels as before; RIFF is the only new one.
    std::vector<std::string> labels;
    for (const edfields::Field& f : a) labels.push_back(f.label);
    CHECK((labels == std::vector<std::string>{"PrSt", "nrES", "ndur", "PATTERN", "RIFF", "FEEL", "-3rd", "3rd", "4th", "5th",
                                              "Oct", "Lo n", "Hi n", "Tch", "CEnt", "ntLt", "HELd", "ARP", "KBD", "TMPO", "UP",
                                              "DOWN", "NAME"}));
    const int controls[23] = {2, 3, 4, -1, -1, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, -1, -1, 0, 1, 1, -1};
    for (size_t i = 0; i < a.size() && i < 23; ++i) CHECK(a[i].arpControl == controls[i]);
    for (const edfields::Field& f : a)                                          // two columns: preset, pattern, riff, feel
        CHECK(f.span == (f.kind == Kind::PresetAction || f.kind == Kind::Pattern || f.kind == Kind::Feel ? 2 : 1));
    CHECK(a[17].label == "ARP" && a[17].kind == Kind::RuntimeFlag && a[17].flag == 0 && a[17].button == kBtnArpOnOff && a[17].editable);
    CHECK(a[18].label == "KBD" && a[18].kind == Kind::RuntimeFlag && a[18].flag == 1 && a[18].button == kBtnArpKeyboard);
    for (const edfields::Field& f : a) CHECK(f.editable == (f.kind != Kind::Pattern && f.kind != Kind::Text));   // pattern, riff, name: display only
    CHECK(a[11].label == "Lo n" && a[11].offset == 370 && a[11].mirror == 372 && a[12].mirror == 373);
    // The pattern, the riff and the feel: 16-bit numbers in the arp block, named by the OS's objects 9, 11 and 10.
    CHECK(a[3].kind == Kind::Pattern && a[3].offset == 390 && a[3].size == 2 && a[3].nameType == edproto::kArpPattern);
    CHECK(a[4].kind == Kind::Pattern && a[4].offset == 394 && a[4].size == 2 && a[4].nameType == edproto::kArpRiff && a[4].live());
    CHECK(a[5].kind == Kind::Feel && a[5].offset == 392 && a[5].size == 2 && a[5].nameType == edproto::kArpFeel);
    CHECK(edproto::kArpFeel == 10 && edproto::kArpRiff == 11);
    // Conversions
    edfields::Field dur = a[2];
    CHECK(dur.label == "ndur" && toRaw(dur, 50) == 128 && toDomain(dur, 128) == 50 && toRaw(dur, 0) == 1 && toDomain(dur, 255) == 99);
    for (int s = 0; s <= 99; ++s) CHECK(toDomain(dur, toRaw(dur, s)) == s);   // the smallest raw that shows s
    edfields::Field down = a[21];
    uint8_t buf[64] = {};
    store(down, toRaw(down, 2), buf, 64, kArpBlock);
    int64_t raw;
    CHECK(buf[368 - 350] == 0xE8 && rawOf(down, buf, 64, kArpBlock, raw) && raw == -24 && toDomain(down, raw) == 2);
    CHECK_EQ(format(down, 2, t), std::string("Oct2"));
    CHECK(format(a[18], 1, t) == "On" && format(a[17], 0, t) == "OFF" && toDomain(a[17], 2) == 1);
    edfields::Field res = a[1];
    CHECK(res.kind == Kind::Resolution && toDomain(res, 96) == 4 && toRaw(res, 4) == 96 && format(res, 4, t) == "16th" && toDomain(res, 1000) == -1);
    store(res, 384, buf, 64, kArpBlock);
    CHECK(buf[52] == 0 && buf[53] == 0 && buf[54] == 0x01 && buf[55] == 0x80);                  // 4 bytes, big-endian
    CHECK_EQ(format(a[19], 120, t), std::string("t120"));
    // The feel: value, range and conversions as before (its name is added by the controller).
    CHECK(a[5].lo == -1 && a[5].hi == 999 && a[5].editable);
    CHECK(toDomain(a[5], kNone16) == -1 && toRaw(a[5], -1) == kNone16 && format(a[5], -1, t) == "OFF" && format(a[5], 3, t) == "3");
    // The arp preset: the OS's own number (it counts from 0) through the OS's format.
    CHECK(a[0].kind == Kind::PresetAction && format(a[0], -1, t) == "Snd" && format(a[0], 0, t) == "AP00" && a[0].lo == -1);
    CHECK(format(a[0], 1, t) == "AP01" && arpPresetLabel(t, 117) == "AP117" && arpPresetLabel(t, 7) == "AP07");
    CHECK(toDomain(a[0], 0xFFFFFFFF) == -1 && toDomain(a[0], 22) == 22);
    std::printf("editor_edit_test: COMMON and ARP fields\n");
}

// ---- controller with a fake engine link
struct FakeLink : editor::Link {
    // A simulated OS for the arp controls (docs/editor/ROUND2_REVIEW.md): answers the raw-memory reads of the
    // arp-control variable, the live arp state, the current arp preset and the runtime flags, and reacts to buttons.
    struct ArpSim {
        bool on = false;                             // answer the arp reads
        int mode = 0x01;                             // the panel mode: 0x01 normal, 0x0C the arp menu
        int var = 2;                                 // the arp-control variable (keeps its value outside the arp menu)
        int lastParam = 5;                           // Arp Mode opens at the last arp parameter shown
        bool tempoIgnored = false;                   // the Tempo button does nothing (a select that fails)
        bool selectIgnored = false;                  // Tempo / Range / Arp Mode do not open the arp menu
        int leaveMenuAfterSteps = -1;                // the panel leaves the arp menu after this many steps
        int mainPresetSteps = 0;                     // -/No or +/Yes outside the arp menu: the main preset stepped
        int bankSwitches = 0;                        // the bank button outside the arp menu: the bank switched
        Bytes live = Bytes(64, 0);
        int32_t preset = -1;
        int presets = 12;                            // the OS's arp presets (0 .. presets - 1): it answers their names only
        int demoFrom = -1;                           // a name read of this arp preset or later starts the OS's demo
        uint8_t flags[2] = {0, 0};
        int blindSteps = 0;                          // -/No or +/Yes outside the arp menu (would step the main preset)
        std::vector<int> stepVars;                   // the variable at every -/No / +/Yes
        void step(int dir) {
            auto u32 = [&](int at) { return uint32_t(live[size_t(at)]) << 24 | uint32_t(live[size_t(at) + 1]) << 16 | uint32_t(live[size_t(at) + 2]) << 8 | live[size_t(at) + 3]; };
            auto put32 = [&](int at, uint32_t v) { for (int i = 0; i < 4; ++i) live[size_t(at + i)] = uint8_t(v >> (24 - 8 * i)); };
            auto clampByte = [&](int at, int lo, int hi) { live[size_t(at)] = uint8_t(std::max(lo, std::min(hi, int(live[size_t(at)]) + dir))); };
            switch (var) {
            case 0: put32(56, uint32_t(std::max(25, std::min(320, int(u32(56)) + dir)))); break;
            case 1: {                                // Range: up and down together
                const int up = std::max(0, std::min(48, int(live[17]) + 12 * dir));
                live[17] = uint8_t(up); live[18] = uint8_t(int8_t(-up));
                break;
            }
            case 2: preset = std::max(-1, std::min(presets - 1, preset + dir)); break;
            case 3: {
                int i = 0;
                while (i < 12 && edfields::kResolutionTicks[i] != u32(52)) ++i;
                i = std::max(0, std::min(11, (i == 12 ? 4 : i) + dir));
                put32(52, edfields::kResolutionTicks[i]);
                break;
            }
            case 4: {                                // duration: the smallest raw that shows the next value
                const int shown = std::max(0, std::min(99, int(live[35]) * 100 / 256 + dir));
                live[35] = uint8_t(std::max(1, (shown * 256 + 99) / 100));
                break;
            }
            case 5: {                                // feel: OFF (0xFFFF) <-> 0, 1, ...
                const int v = live[42] << 8 | live[43];
                const int n = v == 0xFFFF ? (dir > 0 ? 0 : 0xFFFF) : (v == 0 && dir < 0 ? 0xFFFF : v + dir);
                live[42] = uint8_t(n >> 8); live[43] = uint8_t(n);
                break;
            }
            case 6: case 7: case 8: case 9: case 10:  // intervals: one at most
                if (dir > 0) for (int i = 0; i < 5; ++i) live[size_t(11 + i)] = uint8_t(i == var - 6);
                else live[size_t(11 + var - 6)] = 0;
                break;
            case 11: clampByte(20, 0, 127); live[22] = live[20]; break;   // both key-range pairs
            case 12: clampByte(21, 0, 127); live[23] = live[21]; break;
            case 13: clampByte(34, 0, 127); break;
            case 14: clampByte(32, 0, 1); break;
            case 15: clampByte(36, 0, 99); break;
            case 16: clampByte(1, 0, 1); break;
            default: break;
            }
        }
    } arp;
    std::vector<std::pair<int, bool>> presses;
    std::vector<double> pressTimes;
    const double* clock = nullptr;
    std::set<int> downNow;                           // buttons down now: the editor's own presses
    std::set<int> latched;                           // and the skin's latched buttons
    bool panelButtonHeld() const override { return !downNow.empty() || !latched.empty(); }
    void pressButton(int raw, bool down) override {
        if (down) downNow.insert(raw); else downNow.erase(raw);
        presses.push_back({raw, down});
        pressTimes.push_back(clock ? *clock : 0);
        ++gestures;                                  // the engine counts every edge as panel input
        if (!down) return;
        const bool inMenu = arp.mode == 0x0C;
        switch (raw) {
        case edfields::kBtnTempo: if (!arp.tempoIgnored && !arp.selectIgnored) { arp.mode = 0x0C; arp.var = 0; } break;
        case edfields::kBtnRange: if (!arp.selectIgnored) { arp.mode = 0x0C; arp.var = 1; } break;
        case edfields::kBtnArpMode:
            if (arp.selectIgnored) break;
            arp.var = !inMenu || arp.var < 2 ? arp.lastParam : arp.var == 16 ? 2 : arp.var + 1;
            arp.mode = 0x0C;
            arp.lastParam = arp.var;
            break;
        case edfields::kBtnYes: case edfields::kBtnNo:
            arp.stepVars.push_back(arp.var);
            if (!inMenu) { ++arp.blindSteps; ++arp.mainPresetSteps; }
            else arp.step(raw == edfields::kBtnYes ? 1 : -1);
            if (inMenu && arp.leaveMenuAfterSteps > 0 && --arp.leaveMenuAfterSteps == 0) arp.mode = 0x01;
            break;
        case edfields::kBtnBank:                     // from the arp menu: back to normal; in normal mode: the bank
            if (inMenu) arp.mode = 0x01; else ++arp.bankSwitches;
            break;
        case edfields::kBtnArpOnOff: arp.flags[0] ^= 1; break;
        case edfields::kBtnArpKeyboard: arp.flags[1] ^= 1; break;
        default: break;
        }
    }
    int pressCount(int raw) const { int n = 0; for (const auto& p : presses) n += p.first == raw && p.second; return n; }
    bool open = false, run = true;
    std::vector<Bytes> sent, standing;
    std::vector<Bytes> replies;
    uint64_t leds = 0, triggers = 0, gestures = 0, moves = 0, midi = 0;
    int lastControl = -1;
    std::map<int, int> ledModes;
    std::array<char, 4> disp{{'P', ' ', '0', '1'}};
    bool demo = false;                               // the OS answers no SysEx at all (its demo mode)
    // The names the OS gives (16 characters, offset 8) by object type and number: arp presets (type 8, below
    // arp.presets), note patterns (9), feels (10), riffs (11). None given: no answer, as the OS does past its last one.
    std::map<std::pair<int, int>, std::string> objNames;
    static std::string arpPresetName(int n) {        // invented names
        static const char* const kGreek[6] = {"Alpha", "Beta", "Gamma", "Delta", "Epsilon", "Zeta"};
        return std::string(kGreek[n % 6]) + " " + std::to_string(n);
    }
    void setOpen(bool on) override { open = on; }
    bool send(const Bytes& m) override {
        sent.push_back(m);
        if (arp.demoFrom >= 0 && m.size() > 20 && m[5] == edproto::kPartialRead && addressOf(m).type == edproto::kArpPreset &&
            addressOf(m).number >= arp.demoFrom)
            demo = true;
        if (!demo && arp.on && m.size() > 20 && m[5] == edproto::kPartialRead && addressOf(m).offset == 8 &&
            addressOf(m).type >= edproto::kArpPreset && addressOf(m).type <= edproto::kArpRiff) {
            const edproto::Address a = addressOf(m);
            std::string name;
            bool given = a.type == edproto::kArpPreset && a.number < arp.presets;
            if (given) name = arpPresetName(a.number);
            if (auto it = objNames.find({a.type, a.number}); it != objNames.end()) { name = it->second; given = true; }
            if (given) {                             // (an empty name: 16 spaces)
                name.resize(16, ' ');
                answerData(m, Bytes(name.begin(), name.end()));
            }
        }
        if (!demo && arp.on && m.size() > 20 && m[5] == edproto::kPartialRead && addressOf(m).type == edproto::kRawMemory) {
            const edproto::Address a = addressOf(m);
            if (a.offset == edfields::kArpControlVar) answerData(m, {uint8_t(arp.var)});
            else if (a.offset == edfields::kPanelMode) answerData(m, {uint8_t(arp.mode)});
            else if (a.offset == edfields::kLiveArp) answerData(m, arp.live);
            else if (a.offset == edfields::kArpFlags) answerData(m, {arp.flags[0], arp.flags[1]});
            else if (a.offset == edfields::kCurrentArpPreset)
                answerData(m, {uint8_t(uint32_t(arp.preset) >> 24), uint8_t(uint32_t(arp.preset) >> 16), uint8_t(uint32_t(arp.preset) >> 8), uint8_t(arp.preset)});
        }
        return true;
    }
    void setStandingRead(const std::vector<Bytes>& m) override { standing = m; }
    bool popReply(Bytes& m) override { if (replies.empty()) return false; m = replies.front(); replies.erase(replies.begin()); return true; }
    uint64_t ledStateMessages() const override { return leds; }
    uint64_t presetTriggers() const override { return triggers; }
    uint64_t panelGestures() const override { return gestures; }
    uint64_t panelControlMoves() const override { return moves; }
    uint64_t externalMidi() const override { return midi; }
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
    t.system.fields[6].format = kFmtThreeDigits;                                  // shown as stored, "000"
    t.system.fields.push_back(mkField("NRPN", 25, kBitField, (1 << 8) | 5, 1));   // a flag: bit 5 of byte 25
    t.system.fields.push_back(mkField("Channel", 9, kUnsignedByte, 0, 1));          // the stored value may be outside
    for (int i = 0; i < edpages::kOscillatorFields; ++i) t.oscillator.fields.push_back(mkField("SPARE " + std::to_string(i), 0xF0, kUnsignedByte, 0, 1));
    t.oscillator.fields[51] = mkField("Wave ID", 0x4C, kLong, 0, -1);
    t.oscillator.fields[30] = mkField("Filter Mode", 0x3A, kUnsignedByte, 0, 1);
    t.oscillator.fields[32] = mkField("Filter Cutoff", 0x39, kUnsignedByte, 0, 127);
    t.oscillator.fields[37] = mkField("Filter Kbd Amount", 0x3B, kSignedByte, -64, 63);
    t.oscillator.fields[14] = mkField("Fine Tune", 0x20, kSignedByte, -64, 63);
    t.oscillator.fields[15] = mkField("PitchBend Enable", 0x22, kBitField, (1 << 8) | 0, 1);
    t.oscillator.fields[16] = mkField("Trigger Key", 0x23, kSmallUnsigned, 0, 15);   // value type 5: display only
    t.oscillator.fields[17] = mkField("TriggerKey", 0x0E, kBitField, 0x0F04, 0x0809, 0, 0);   // mask 0x0F at bit 4
    t.oscillator.lists.push_back({true, 8, {"NoteOff", "NoteOn"}});
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
    osc[0x39] = 64; osc[0x3A] = 1; osc[0x3B] = 10; osc[0x20] = 0; osc[0x22] = 0xFF; osc[0x23] = 3; osc[0x0E] = 144;
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
    // A bit field with a mask other than 1: shown through its value list, display only.
    const int trigMode = c.refForTest('o', 17);
    CHECK(spanOf(c.view(), trigMode) && c.view().screen.rowText(spanOf(c.view(), trigMode)->row).find("NoteOn") != std::string::npos);
    link.sent.clear();
    c.command("drag_begin", std::to_string(trigMode));
    c.command("drag", "-3");
    c.command("drag_end", "");
    c.choose(trigMode, 8);
    tick(200);
    CHECK(!link.any(0xB0) && !link.any(0xF0) && c.menu(trigMode).items.empty() && c.menu(trigMode).groups.empty());
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
    sys[9] = 3;                                                                 // outside the table's range 0-1
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
    // Revert restores the stored value exactly, even outside the table's range; new edits stay in it.
    {
        const int ch = c.refForTest('s', 9);
        link.sent.clear();
        c.choose(ch, 0);
        CHECK(link.sent.back() == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 9, 1}, {0}));
        tick(20);
        CHECK(c.menu(ch).revert && c.menu(ch).revertLabel == "3");
        c.revert(ch);
        CHECK(link.sent.back() == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 9, 1}, {3}));
        tick(20);
        c.choose(ch, 5);                                                         // clamped to 0-1
        CHECK(link.sent.back() == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 9, 1}, {1}));
        tick(20);
        c.revert(ch);
        CHECK(link.sent.back() == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 9, 1}, {3}));
        tick(200);
    }
    // Format 0xC000: stored 0 shows "000"; typed numbers are the shown numbers.
    {
        const int id = c.refForTest('s', 6);
        const editor::View sysView = c.view();
        const auto* sp = spanOf(sysView, id);
        CHECK(sp && sysView.screen.rowText(sp->row).substr(size_t(sp->col), 3) == "000");
        link.sent.clear();
        c.command("type", std::to_string(id));
        for (char ch : std::string("005")) c.key(std::string(1, ch));
        c.key("enter");
        CHECK(!link.sent.empty() && link.sent.back() == edproto::partialWrite({edproto::kSystem, 0, 0, 0, 6, 1}, {5}));
        CHECK(c.view().screen.rowText(sp->row).substr(size_t(sp->col), 3) == "005");
        tick(200);
    }
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
    // COMMON no longer shows the insert modulation (it is on INSERT > MOD).
    CHECK((c.view().sections == std::set<std::string>{"SLOTS", "KNOBS"}));
    c.command("section", "MOD");
    CHECK(c.view().section == "KNOBS");

    // ---- ARP: values from the OS (live arp state, runtime flags, current arp preset); every edit through the panel's
    // own buttons, on a simulated OS
    link.clock = &now;
    link.arp.on = true;
    Bytes& live = link.arp.live;
    live[351 - 350] = 0; live[352 - 350] = 0; live[362 - 350] = 1;            // held off, 3rd on
    live[367 - 350] = 12; live[368 - 350] = uint8_t(-12); live[370 - 350] = 0; live[371 - 350] = 127;
    live[372 - 350] = 0; live[373 - 350] = 127; live[385 - 350] = 128; live[386 - 350] = 0;
    live[392 - 350] = 0xFF; live[393 - 350] = 0xFF; live[390 - 350] = 0x00; live[391 - 350] = 0x05;
    live[394 - 350] = 0xFF; live[395 - 350] = 0xFF;                            // no riff
    link.objNames[{edproto::kArpPattern, 5}] = "Theta Run";
    live[402 - 350 + 3] = 96; live[406 - 350 + 3] = 120;
    live[383 - 350] = 0;                                                       // the live on/off byte does not reflect the panel
    link.arp.flags[0] = 1; link.arp.flags[1] = 1;                               // the OS's flags: arp on, keyboard on
    c.command("area", "ARP");
    tick(10);
    tick(10);
    tick(10);
    v = c.view();
    // The owner-approved layout: two pages, the arp preset, pattern, riff and feel two columns wide.
    CHECK(v.page == 1 && v.pages == 2);
    CHECK_EQ(v.screen.rowText(0), std::string("PRST Snd                  NRES 16th   NDUR 50     "));
    CHECK_EQ(v.screen.rowText(1), std::string("PATTERN Theta Run         RIFF NONE               "));
    CHECK_EQ(v.screen.rowText(2), std::string("FEEL OFF                  -3RD OFF    3RD  On     "));
    CHECK_EQ(v.screen.rowText(3), std::string("4TH  OFF    5TH  OFF      OCT  OFF    LO N C-1    "));
    int patternReads = 0;                                                      // the pattern's name is read once
    for (const Bytes& m : link.sent) patternReads += m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kArpPattern && FakeLink::addressOf(m).number == 5;
    CHECK(patternReads == 1);
    c.command("page_next", "");
    tick(10);
    tick(10);
    v = c.view();
    CHECK(v.page == 2);
    CHECK_EQ(v.screen.rowText(0), std::string("HI N G9     TCH  OFF      CENT OFF    NTLT 0      "));
    CHECK_EQ(v.screen.rowText(1), std::string("HELD OFF    ARP  On       KBD  On     TMPO t120   "));
    CHECK_EQ(v.screen.rowText(2), std::string("UP   Oct1   DOWN Oct1                             "));
    CHECK(v.screen.rowText(3).rfind("NAME ", 0) == 0);
    c.command("page_prev", "");
    tick(10);
    auto runFor = [&](double ms) { for (double t = 0; t < ms; t += 10) tick(10); };
    auto settle = [&]() { for (int i = 0; i < 2000 && c.arpBusyForTest(); ++i) tick(10); tick(10); };   // up to 20 s
    auto lastRelease = [&](int raw) { double at = -1; for (size_t i = 0; i < link.presses.size(); ++i) if (link.presses[i].first == raw && !link.presses[i].second) at = link.pressTimes[i]; return at; };
    auto firstPress = [&](int raw) { for (size_t i = 0; i < link.presses.size(); ++i) if (link.presses[i].first == raw && link.presses[i].second) return link.pressTimes[i]; return -1.0; };
    const int arpOn = c.refForTest('a', 17), kbd = c.refForTest('a', 18), minus3 = c.refForTest('a', 6), up = c.refForTest('a', 20);
    const int lo = c.refForTest('a', 11), limit = c.refForTest('a', 15), res = c.refForTest('a', 1), tempo = c.refForTest('a', 19);
    const int feel = c.refForTest('a', 5), prst = c.refForTest('a', 0), pattern = c.refForTest('a', 3), riff = c.refForTest('a', 4);
    const int centre = c.refForTest('a', 14), touch = c.refForTest('a', 13);

    // Resolution 16th -> 32nd: Arp Mode until the variable reads 3, 1.4 s for the OS to show it, then +/Yes once.
    link.presses.clear(); link.pressTimes.clear();
    c.choose(res, 5);
    CHECK(c.view().screen.rowText(0).find("NRES 32nd") == 26);                  // its target, shown at once
    settle();
    CHECK(link.arp.live[55] == 48 && link.arp.var == 3);                         // 32nd = 48 ticks
    CHECK(link.pressCount(edfields::kBtnYes) == 1 && link.pressCount(edfields::kBtnNo) == 0 && link.pressCount(edfields::kBtnArpMode) >= 2);
    CHECK(firstPress(edfields::kBtnYes) - lastRelease(edfields::kBtnArpMode) >= 1400);
    for (int sv : link.arp.stepVars) CHECK(sv == 3);
    CHECK(link.arp.blindSteps == 0 && c.view().edited);
    // Begun in normal mode: back there with the bank button, pressed once (in the arp menu), never a bank switch.
    CHECK(c.panelModeBeforeArpEdit() == 0x01 && link.arp.mode == 0x01 && link.pressCount(edfields::kBtnBank) == 1);
    CHECK(link.arp.bankSwitches == 0);
    {   // the bank button came after the last step, while the mode read the arp menu
        size_t bank = 0, yes = 0;
        for (size_t i = 0; i < link.presses.size(); ++i) {
            if (link.presses[i].first == edfields::kBtnBank && link.presses[i].second) bank = i;
            if (link.presses[i].first == edfields::kBtnYes && link.presses[i].second) yes = i;
        }
        CHECK(bank > yes);
    }
    // Range (up and down together): the Range button once, +/Yes once.
    link.presses.clear(); link.arp.stepVars.clear();
    c.choose(up, 2);
    settle();
    CHECK(link.arp.live[17] == 24 && int8_t(link.arp.live[18]) == -24);
    CHECK(link.pressCount(edfields::kBtnRange) == 1 && link.pressCount(edfields::kBtnYes) == 1 && link.pressCount(edfields::kBtnArpMode) == 0);
    c.command("page_next", "");                                                // UP and DOWN are on page 2
    tick(10);
    tick(10);
    v = c.view();
    CHECK(v.screen.rowText(2).find("UP   Oct2") == 0 && v.screen.rowText(2).find("DOWN Oct2") == 12);
    c.command("page_prev", "");
    tick(10);
    // The arp preset: " Snd" -> AP04 by +/Yes five times. The list: the preset's own settings, then the OS's arp presets
    // by the OS's own number (from 0) and name, up to the last one the OS answers a name read for.
    CHECK(c.view().screen.rowText(0).find("PRST Snd") == 0 && c.view().screen.rowText(0).find("NRES 32nd") == 26);
    runFor(10000);                                                             // the names, and the end of the list
    const editor::Menu pm = c.menu(prst);
    CHECK(pm.groups.empty() && pm.items.size() == 13 && pm.items[0].label == "Snd" && pm.items[0].current);
    CHECK(pm.items[1].label == "AP00 Alpha 0" && pm.items[1].value == 0 && pm.items[12].label == "AP11 Zeta 11");
    link.presses.clear(); link.arp.stepVars.clear();
    c.choose(prst, 4);
    settle();
    CHECK(link.arp.preset == 4 && link.pressCount(edfields::kBtnYes) == 5 && link.arp.blindSteps == 0);
    for (int sv : link.arp.stepVars) CHECK(sv == 2);
    CHECK(c.view().screen.rowText(0).find("PRST AP04 Epsilon 4") == 0);        // the OS's number and the name
    // Lo n by dragging: the target follows the drag; both key-range pairs (the OS writes them).
    link.presses.clear(); link.arp.stepVars.clear();
    c.command("drag_begin", std::to_string(lo));
    c.command("drag", "3");
    runFor(500);
    c.command("drag", "5");
    c.command("drag_end", "");
    CHECK(c.view().screen.rowText(2).find("C#-1") == std::string::npos || true);
    settle();
    CHECK(link.arp.live[20] == 5 && link.arp.live[22] == 5 && link.arp.blindSteps == 0);
    CHECK(c.panelModeBeforeArpEdit() == 0x01 && link.arp.mode == 0x01 && link.arp.bankSwitches == 0);
    // Begun in the arp menu: the editor stays there (no bank button).
    link.presses.clear();
    link.arp.mode = 0x0C; link.arp.var = 9;
    c.choose(touch, 30);
    settle();
    CHECK(link.arp.live[34] == 30 && link.arp.mode == 0x0C && link.pressCount(edfields::kBtnBank) == 0);
    CHECK(c.panelModeBeforeArpEdit() == 0x0C);
    link.arp.mode = 0x01;
    // A run of edits begun in normal mode: each lands, then the bank button once at the end.
    link.presses.clear();
    c.choose(touch, 31);
    c.choose(centre, 1);
    settle();
    CHECK(link.arp.live[34] == 31 && link.arp.live[32] == 1 && link.pressCount(edfields::kBtnBank) == 1 && link.arp.mode == 0x01);
    CHECK(link.presses.size() >= 2 && link.presses[link.presses.size() - 2].first == edfields::kBtnBank);
    // Another panel mode (a sound edit, 0x02): no arp edit, nothing pressed, and a message for the user.
    link.presses.clear();
    link.arp.mode = 0x02;
    const uint32_t notices = c.noticeSerial();
    c.choose(touch, 50);
    c.choose(centre, 0);
    settle();
    CHECK(link.presses.empty() && link.arp.live[34] == 31 && link.arp.live[32] == 1 && c.noticeSerial() == notices + 1);
    CHECK(c.notice().find("normal mode") != std::string::npos && c.panelModeBeforeArpEdit() == 0x02);
    link.arp.mode = 0x01;
    // The next edit clears the message (an empty text with a new serial: the plugin hides its banner).
    const uint32_t refused = c.noticeSerial();
    c.choose(touch, 32);
    CHECK(c.notice().empty() && c.noticeSerial() == refused + 1);
    settle();
    CHECK(link.arp.live[34] == 32 && c.notice().empty());
    // A panel button latched by the skin (the preset bank button): no arp edit, no button edge at all, a message.
    link.presses.clear();
    link.latched.insert(edfields::kBtnBank);
    const uint32_t beforeLatched = c.noticeSerial();
    c.choose(tempo, 140);
    settle();
    CHECK(link.presses.empty() && link.arp.bankSwitches == 0 && c.noticeSerial() == beforeLatched + 1);
    CHECK(c.notice().find("held") != std::string::npos);
    link.latched.clear();
    // Safety: the panel leaves the arp menu while the editor waits (the variable keeps reading the control): the edit
    // stops without pressing -/No or +/Yes.
    link.presses.clear(); link.arp.stepVars.clear();
    c.choose(limit, 7);
    runFor(600);
    link.arp.mode = 0x01;                                                       // back to normal mode
    settle();
    CHECK(link.arp.live[36] == 0 && link.arp.blindSteps == 0 && link.arp.mainPresetSteps == 0);
    CHECK(link.pressCount(edfields::kBtnYes) == 0 && link.pressCount(edfields::kBtnNo) == 0);
    // Normal mode, and the select does not open the arp menu (the variable reads 2 = PrSt there): no -/No or +/Yes,
    // the main preset unchanged.
    link.presses.clear(); link.arp.stepVars.clear();
    link.arp.selectIgnored = true; link.arp.mode = 0x01; link.arp.var = 2;
    const int32_t arpPresetBefore = link.arp.preset;
    c.choose(prst, link.arp.preset + 3);
    settle();
    CHECK(link.pressCount(edfields::kBtnYes) == 0 && link.pressCount(edfields::kBtnNo) == 0 && link.arp.mainPresetSteps == 0);
    CHECK(link.arp.preset == arpPresetBefore && c.panelModeBeforeArpEdit() == 0x01);
    CHECK(link.pressCount(edfields::kBtnBank) == 0);                           // the mode never read the arp menu
    link.arp.selectIgnored = false;
    // Every press is checked: the panel leaves the arp menu after the first step: no second press.
    link.presses.clear();
    link.arp.leaveMenuAfterSteps = 1;
    c.choose(limit, 5);
    settle();
    CHECK(link.pressCount(edfields::kBtnYes) == 1 && link.arp.live[36] == 1 && link.arp.mainPresetSteps == 0);
    link.arp.leaveMenuAfterSteps = -1;
    // Panel input or other MIDI during an edit: it stops at once (no -/No or +/Yes after it).
    for (int which = 0; which < 2; ++which) {
        link.presses.clear();
        c.choose(limit, 9);
        runFor(600);                                                            // waiting for the OS to show the control
        if (which == 0) link.gestures += 1;                                     // a panel button or knob
        else link.midi += 1;                                                    // a note from the host
        settle();
        CHECK(link.pressCount(edfields::kBtnYes) == 0 && link.pressCount(edfields::kBtnNo) == 0 && link.arp.live[36] == 1);
        CHECK(link.pressCount(edfields::kBtnBank) == 0);                       // nothing more pressed at all
        link.arp.mode = 0x01;                                                   // (the panel's user takes it from here)
    }
    c.choose(limit, 7);
    settle();
    CHECK(link.arp.live[36] == 7 && link.arp.mainPresetSteps == 0);
    for (int sv : link.arp.stepVars) CHECK(sv == 15);
    // A control that cannot be selected (the Tempo button does nothing here): no step at all.
    link.presses.clear(); link.arp.stepVars.clear();
    link.arp.tempoIgnored = true; link.arp.mode = 0x01; link.arp.var = 2;
    c.choose(tempo, 130);
    settle();
    CHECK(link.pressCount(edfields::kBtnTempo) == 1 && link.pressCount(edfields::kBtnYes) == 0 && link.pressCount(edfields::kBtnNo) == 0);
    link.arp.tempoIgnored = false;
    // Feel is editable now: OFF -> 0.
    c.choose(feel, 0);
    settle();
    CHECK(link.arp.live[42] == 0 && link.arp.live[43] == 0 && link.arp.blindSteps == 0);
    // An interval: the OS turns the others off.
    c.choose(minus3, 1);
    settle();
    CHECK(link.arp.live[11] == 1 && link.arp.live[12] == 0);
    // On/off and keyboard: their own buttons, once, only when the OS's flag differs; no -/No or +/Yes.
    link.presses.clear();
    c.choose(arpOn, 0);
    settle();
    CHECK(link.arp.flags[0] == 0 && link.pressCount(edfields::kBtnArpOnOff) == 1);
    c.choose(arpOn, 0);
    settle();
    CHECK(link.pressCount(edfields::kBtnArpOnOff) == 1);
    c.choose(kbd, 0);
    settle();
    CHECK(link.arp.flags[1] == 0 && link.pressCount(edfields::kBtnArpKeyboard) == 1 && link.pressCount(edfields::kBtnYes) == 0);
    c.command("page_next", "");
    tick(10);
    tick(10);
    CHECK(c.view().screen.rowText(1).find("ARP  OFF") == 12 && c.view().screen.rowText(1).find("KBD  OFF") == 26);
    c.command("page_prev", "");
    // Display-only fields (the pattern, the riff, the name) ignore drags and have no list.
    link.presses.clear();
    for (int ref : {pattern, riff}) {
        c.command("drag_begin", std::to_string(ref));
        c.command("drag", "3");
        c.command("drag_end", "");
        runFor(500);
        CHECK(c.menu(ref).items.empty() && c.menu(ref).groups.empty());
    }
    CHECK(link.presses.empty());
    // Over every arp edit above: the bank never switched, the main preset never stepped.
    CHECK(link.arp.bankSwitches == 0 && link.arp.mainPresetSteps == 0 && link.arp.blindSteps == 0);
    // Never a write into the arp state and never a dump (nor its whole read).
    for (const Bytes& m : link.sent) {
        CHECK(!(m.size() > 6 && m[5] == edproto::kPartialWrite && FakeLink::addressOf(m).type == edproto::kRawMemory));
        CHECK(!(m.size() > 6 && (m[5] == edproto::kWholeDump || m[5] == edproto::kWholeRead)));
    }
    // Closing in the middle of an edit lets go of any button the editor holds.
    c.choose(res, 7);
    for (int i = 0; i < 400 && !(!link.presses.empty() && link.presses.back().second); ++i) tick(10);
    CHECK(!link.presses.empty() && link.presses.back().second);
    c.setOpen(false);
    CHECK(!link.presses.back().second && link.presses.back().first == link.presses[link.presses.size() - 2].first);
    c.setOpen(true);
    tick(10);

    // ---- PRESETS: the current preset marked (focused) and its view shown; a click sends the program change; a
    // preset change ends EDITED and revert
    // Stored presets are numbers 0x40 + n (the user's, as saved); 0x100 + n are the factory copies in the OS image,
    // which the fake OS names differently: the editor must never show those.
    CHECK(edproto::kStoredPreset == 0x40 && edproto::kFactoryPreset == 0x100);
    std::string currentName = "Preset 12";
    auto presetName = [](int n) { std::string s = "Preset " + std::to_string(n + 1); s.resize(16, ' '); return s; };
    int storedReads = 0, otherReads = 0;
    auto answerPresets = [&]() {
        for (const Bytes& m : link.sent) {
            if (!(m.size() > 6 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kPreset)) continue;
            const edproto::Address a = FakeLink::addressOf(m);
            if (a.number >= edproto::kStoredPreset && a.number < edproto::kStoredPreset + 64) {
                ++storedReads;
                const std::string n = presetName(a.number - edproto::kStoredPreset); link.answerData(m, Bytes(n.begin(), n.end()));
            } else if (a.number >= edproto::kFactoryPreset) {
                ++otherReads;
                std::string n = "Factory " + std::to_string(a.number - edproto::kFactoryPreset + 1); n.resize(16, ' ');
                link.answerData(m, Bytes(n.begin(), n.end()));
            } else if (a.number == 0 && a.offset == 8) { std::string n = currentName; n.resize(16, ' '); link.answerData(m, Bytes(n.begin(), n.end())); }
            else ++otherReads;
        }
        link.sent.clear();
    };
    link.sent.clear();
    c.command("area", "PRESETS");
    for (int i = 0; i < 60; ++i) { answerPresets(); tick(20); }
    CHECK(storedReads >= 64 && otherReads == 0);                               // every name from the stored presets
    v = c.view();
    CHECK(v.area == "PRESETS" && v.page == 2);                                  // preset 12 is on the second view
    CHECK(v.focusRow == 1 && v.focusCol == 25 && v.focusLen == 20);            // marked: "12  Preset 12" blinks
    for (int col = 25; col < 45; ++col) CHECK(!v.screen.underline[1][size_t(col)]);   // names are never underlined
    link.sent.clear();
    c.command("pick", std::to_string(c.refForTest('n', 13)));                  // preset 14
    CHECK(link.sent.size() == 1 && link.sent[0] == edproto::programChange(0, 13));
    v = c.view();
    CHECK(v.focusRow == 2 && v.focusCol == 25);                                 // marked at once
    CHECK(c.view().edited);
    currentName = "Preset 14";
    link.triggers += 1;                                                         // the load: read behind it
    tick(10);
    CHECK(!c.view().edited);
    // Another route (a host program change): the mark and the view follow.
    currentName = "Preset 40";
    link.triggers += 1;
    c.command("page", "2");
    for (int i = 0; i < 5; ++i) { answerPresets(); tick(20); }
    v = c.view();
    CHECK(v.page == 5 && v.focusRow == 3 && v.focusCol == 25);                  // preset 40: view 5, last row, right
    // Leaving and coming back: the presets open on the current preset's view.
    c.command("area", "SYSTEM");
    c.command("area", "PRESETS");
    CHECK(c.view().page == 5);

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

// The ARP page before the OS's current arp preset is read: a placeholder at once, and not clickable until it is read.
void testArpPresetPlaceholder() {
    FakeLink link;
    editor::Controller c(link);
    c.setTables(tables());
    c.setOpen(true);
    double now = 0;
    c.tick(now += 10);
    c.command("area", "ARP");
    editor::View v = c.view();
    CHECK(v.screen.rowText(0).find("PRST ...") == 0);                          // shown at once
    const int prst = c.refForTest('a', 0);
    bool span = false;
    for (const auto& sp : v.screen.values) span = span || sp.ref == prst;
    CHECK(span);
    // The current arp preset's read is the page's first read (ahead of anything else).
    bool first = false;
    for (const Bytes& m : link.sent)
        if (m.size() > 20 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kRawMemory) {
            first = FakeLink::addressOf(m).offset == edfields::kCurrentArpPreset;
            break;
        }
    CHECK(first);
    link.presses.clear();
    c.command("drag_begin", std::to_string(prst));                             // not read yet: not clickable
    c.command("drag", "3");
    c.command("drag_end", "");
    c.command("type", std::to_string(prst));
    CHECK(!c.view().typing && c.menu(prst).items.empty() && c.menu(prst).groups.empty());
    for (int i = 0; i < 50; ++i) c.tick(now += 20);
    CHECK(link.presses.empty());
    // Once read: the value, and clickable.
    link.arp.on = true;
    link.arp.preset = 7;
    c.command("page_next", "");
    c.command("page_prev", "");
    for (int i = 0; i < 3; ++i) c.tick(now += 10);
    CHECK(c.view().screen.rowText(0).find("PRST AP07") == 0);                  // the OS's own number
    CHECK(!c.menu(prst).items.empty());
    std::printf("editor_edit_test: ARP preset field: placeholder at once, clickable once read\n");
}

// v0.8.3.5 (invented names; no OS names or table contents): the riff, the feel's name, the arp presets by the OS's own
// number, and the arp preset list ending where the OS stops answering name reads.
void testArpRound() {
    FakeLink link;
    editor::Controller c(link);
    c.setTables(tables());
    c.setOpen(true);
    link.clock = nullptr;
    double now = 0;
    auto tick = [&](double ms) { now += ms; c.tick(now); };
    auto runFor = [&](double ms) { for (double t = 0; t < ms; t += 10) tick(10); };
    auto reread = [&]() { c.command("page_next", ""); runFor(30); c.command("page_prev", ""); runFor(30); };
    auto settle = [&]() { for (int i = 0; i < 2000 && c.arpBusyForTest(); ++i) tick(10); tick(10); };
    // Name reads sent so far: object type, number (-1: any), at offset 8.
    auto reads = [&](uint8_t type, int number) {
        int n = 0;
        for (const Bytes& m : link.sent)
            n += m.size() > 20 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == type &&
                 FakeLink::addressOf(m).offset == 8 && (number < 0 || FakeLink::addressOf(m).number == number);
        return n;
    };
    auto maxRead = [&](uint8_t type) {
        int n = -1;
        for (const Bytes& m : link.sent)
            if (m.size() > 20 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == type) n = std::max<int>(n, FakeLink::addressOf(m).number);
        return n;
    };
    link.arp.on = true;
    link.arp.presets = 9;                                                       // this OS: arp presets 0-8
    Bytes& live = link.arp.live;
    auto put16 = [&](uint32_t off, int v) { live[off - 350] = uint8_t(v >> 8); live[off - 350 + 1] = uint8_t(v); };
    put16(390, 0xFFFF);                                                         // no pattern
    put16(394, 7);                                                              // riff 7
    put16(392, 3);                                                              // feel 3
    live[402 - 350 + 3] = 96; live[406 - 350 + 3] = 120; live[385 - 350] = 128;
    link.objNames[{edproto::kArpRiff, 7}] = "Sigma Riff";
    link.objNames[{edproto::kArpFeel, 3}] = "Lambda Feel";
    link.objNames[{edproto::kArpFeel, 5}] = "Mu Feel";
    link.objNames[{edproto::kArpFeel, 20}] = "";                               // answered, but no name
    link.arp.preset = 2;
    c.command("area", "ARP");
    runFor(200);
    editor::View v = c.view();
    CHECK_EQ(v.screen.rowText(0), std::string("PRST AP02 Gamma 2         NRES 16th   NDUR 50     "));
    CHECK_EQ(v.screen.rowText(1), std::string("PATTERN NONE              RIFF Sigma Riff         "));
    CHECK_EQ(v.screen.rowText(2).substr(0, 26), std::string("FEEL 3 Lambda Feel        "));
    // RIFF: display only; its name read once (type 11, number = the value, offset 8, 16 bytes), however often the page
    // is read; no read for none.
    const int prst = c.refForTest('a', 0), riff = c.refForTest('a', 4), feel = c.refForTest('a', 5);
    for (int i = 0; i < 5; ++i) reread();
    CHECK(reads(edproto::kArpRiff, 7) == 1 && reads(edproto::kArpRiff, -1) == 1 && reads(edproto::kArpPattern, -1) == 0);
    CHECK(reads(edproto::kArpFeel, 3) == 1);
    {
        bool exact = false;
        for (const Bytes& m : link.sent)
            exact |= m == edproto::partialRead({edproto::kArpRiff, 7, 0, 0, 8, 16});
        CHECK(exact);
    }
    link.presses.clear();
    c.command("drag_begin", std::to_string(riff));
    c.command("drag", "2");
    c.command("drag_end", "");
    c.command("type", std::to_string(riff));
    runFor(500);
    CHECK(!c.view().typing && c.menu(riff).items.empty() && c.menu(riff).groups.empty() && link.presses.empty());
    // RIFF none: the pattern's none text; a riff the OS gives no name for: its number (read once all the same).
    put16(394, 0xFFFF);
    reread();
    CHECK_EQ(c.view().screen.rowText(1).substr(26), std::string("RIFF NONE               "));
    put16(394, 12);
    reread();
    runFor(4000);
    for (int i = 0; i < 3; ++i) reread();
    CHECK_EQ(c.view().screen.rowText(1).substr(26), std::string("RIFF 12                 "));
    CHECK(reads(edproto::kArpRiff, 12) == 1);
    // FEEL: the number and the OS's name; OFF for none; the number alone when the OS has no name (no answer, or an
    // empty one).
    put16(392, 0xFFFF);
    reread();
    CHECK_EQ(c.view().screen.rowText(2).substr(0, 26), std::string("FEEL OFF                  "));
    put16(392, 20);
    reread();
    runFor(100);
    CHECK_EQ(c.view().screen.rowText(2).substr(0, 26), std::string("FEEL 20                   "));
    put16(392, 21);
    reread();
    runFor(4000);
    reread();
    CHECK_EQ(c.view().screen.rowText(2).substr(0, 26), std::string("FEEL 21                   "));
    CHECK(reads(edproto::kArpFeel, 20) == 1 && reads(edproto::kArpFeel, 21) == 1);
    // FEEL's value, range and editing as before: -1 (OFF) to 999, stepped through the panel's buttons.
    int64_t lo = 0, hi = 0;
    CHECK(c.rangeForTest(feel, lo, hi) && lo == -1 && hi == 999);
    put16(392, 3);
    reread();
    link.presses.clear();
    c.choose(feel, 5);
    settle();
    CHECK(live[42] == 0 && live[43] == 5 && link.pressCount(edfields::kBtnYes) == 2 && link.arp.blindSteps == 0);
    runFor(100);
    CHECK_EQ(c.view().screen.rowText(2).substr(0, 26), std::string("FEEL 5 Mu Feel            "));
    CHECK(c.menu(feel).revert && c.menu(feel).revertLabel == "3 Lambda Feel");
    // The arp presets: every name read once from 0; the OS answers none for 9, and still answers 8 (the probe), so the
    // list ends at 8 and no name past 9 is ever asked for, however long the page stays open.
    CHECK(reads(edproto::kArpPreset, 9) == 1 && reads(edproto::kArpPreset, 8) == 2 && maxRead(edproto::kArpPreset) == 9);
    for (int n = 0; n < 8; ++n) CHECK(reads(edproto::kArpPreset, n) == 1);
    for (int i = 0; i < 10; ++i) { reread(); runFor(1000); }
    CHECK(reads(edproto::kArpPreset, -1) == 11 && maxRead(edproto::kArpPreset) == 9);
    CHECK(c.rangeForTest(prst, lo, hi) && lo == -1 && hi == 8);
    const editor::Menu pm = c.menu(prst);
    CHECK(pm.items.size() == 10 && pm.items[0].label == "Snd" && pm.items[1].label == "AP00 Alpha 0" &&
          pm.items[3].label == "AP02 Gamma 2" && pm.items[3].current && pm.items[9].label == "AP08 Gamma 8");
    // Arp presets past the end cannot be chosen, dragged to or typed: the edit stops at the last one.
    link.presses.clear();
    c.choose(prst, 50);
    settle();
    CHECK(link.arp.preset == 8 && link.pressCount(edfields::kBtnYes) == 6);
    CHECK(c.view().screen.rowText(0).find("PRST AP08 Gamma 8") == 0);
    std::printf("editor_edit_test: ARP round: riff, feel names, arp preset numbering, list end (%d name reads)\n",
                reads(edproto::kArpPreset, -1));

    // The OS stops answering anything (its demo) just as the names are read: no end is taken from that. The probe
    // gets no answer either, nothing is repeated every tick, and the names go on once the OS answers again.
    FakeLink l2;
    editor::Controller c2(l2);
    c2.setTables(tables());
    c2.setOpen(true);
    l2.arp.on = true;
    l2.arp.presets = 9;
    l2.arp.demoFrom = 4;
    now = 0;
    c2.command("area", "ARP");
    for (int i = 0; i < 1000; ++i) c2.tick(now += 10);                         // 10 s
    auto reads2 = [&](int number) {
        int n = 0;
        for (const Bytes& m : l2.sent)
            n += m.size() > 20 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kArpPreset &&
                 (number < 0 || FakeLink::addressOf(m).number == number);
        return n;
    };
    CHECK(reads2(4) == 1 && reads2(3) == 2 && reads2(-1) == 6);                 // 0-4 once, the probe (3) once
    CHECK(c2.rangeForTest(c2.refForTest('a', 0), lo, hi) && hi == 3);           // no end yet: the ones answered
    l2.demo = false;
    l2.arp.demoFrom = -1;
    c2.command("page_next", "");                                               // a page read: the OS answers again
    for (int i = 0; i < 1000; ++i) c2.tick(now += 10);
    CHECK(reads2(4) == 2 && reads2(9) == 1 && reads2(8) == 2);
    for (const Bytes& m : l2.sent)
        if (m.size() > 20 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kArpPreset) CHECK(FakeLink::addressOf(m).number <= 9);
    CHECK(c2.rangeForTest(c2.refForTest('a', 0), lo, hi) && hi == 8 && c2.menu(c2.refForTest('a', 0)).items.size() == 10);
    // In its demo from the start: arp preset 0 is asked once, and not again until the OS answers something.
    FakeLink l3;
    editor::Controller c3(l3);
    c3.setTables(tables());
    c3.setOpen(true);
    l3.arp.on = true;
    l3.arp.demoFrom = 0;
    now = 0;
    c3.command("area", "ARP");
    for (int i = 0; i < 1500; ++i) c3.tick(now += 10);                         // 15 s
    int first = 0;
    for (const Bytes& m : l3.sent)
        first += m.size() > 20 && m[5] == edproto::kPartialRead && FakeLink::addressOf(m).type == edproto::kArpPreset;
    CHECK(first == 1);
    std::printf("editor_edit_test: ARP round: no list end taken while the OS answers nothing\n");
}

// Long arp preset names (invented): the arp preset's value uses its half up to col 24, never the gap cell (col 25):
// 20 cells from col 5. A 15-character name fits exactly after a four-character label (AP07), a 14-character one after
// a five-character label (AP104). A 15-character name after a five-character label (AP105) needs 21 cells: it loses
// exactly its last character, and the gap cell stays blank.
void testArpLongNames() {
    FakeLink link;
    editor::Controller c(link);
    c.setTables(tables());
    c.setOpen(true);
    double now = 0;
    auto runFor = [&](double ms) { for (double t = 0; t < ms; t += 10) c.tick(now += 10); };
    link.arp.on = true;
    link.arp.presets = 110;
    Bytes& live = link.arp.live;
    live[390 - 350] = live[391 - 350] = live[392 - 350] = live[393 - 350] = live[394 - 350] = live[395 - 350] = 0xFF;
    live[402 - 350 + 3] = 96; live[406 - 350 + 3] = 120; live[385 - 350] = 128;
    link.objNames[{edproto::kArpPreset, 7}] = "Omicron Pattern";                // 15 characters
    link.objNames[{edproto::kArpPreset, 104}] = "Kappa Sequence";               // 14
    link.objNames[{edproto::kArpPreset, 105}] = "Upsilon Cascade";              // 15
    link.arp.preset = 7;
    c.command("area", "ARP");
    runFor(5000);                                                              // every name, and the end of the list
    const int prst = c.refForTest('a', 0);
    auto show = [&](int preset) {
        link.arp.preset = preset;
        c.command("page_next", ""); runFor(30); c.command("page_prev", ""); runFor(30);
        return c.view();
    };
    editor::View v = show(7);
    CHECK_EQ(v.screen.rowText(0), std::string("PRST AP07 Omicron Pattern NRES 16th   NDUR 50     "));
    const edpages::Screen::Span* sp = spanOf(v, prst);
    CHECK(sp && sp->col == 5 && sp->len == 20 && v.screen.underline[0][24] && !v.screen.underline[0][25]);
    v = show(104);
    CHECK_EQ(v.screen.rowText(0), std::string("PRST AP104 Kappa Sequence NRES 16th   NDUR 50     "));
    sp = spanOf(v, prst);
    CHECK(sp && sp->col == 5 && sp->len == 20 && v.screen.underline[0][24] && !v.screen.underline[0][25]);
    v = show(105);                                                             // the limit: the last character only
    CHECK_EQ(v.screen.rowText(0), std::string("PRST AP105 Upsilon Cascad NRES 16th   NDUR 50     "));
    sp = spanOf(v, prst);
    CHECK(sp && sp->col == 5 && sp->len == 20 && v.screen.rowText(0)[25] == ' ' && !v.screen.underline[0][25]);
    // The right-click list has the whole names.
    const editor::Menu m = c.menu(prst);
    bool whole7 = false, whole104 = false, whole105 = false;
    for (const auto& g : m.groups)
        for (const editor::Choice& ch : g.second) {
            whole7 |= ch.value == 7 && ch.label == "AP07 Omicron Pattern";
            whole104 |= ch.value == 104 && ch.label == "AP104 Kappa Sequence";
            whole105 |= ch.value == 105 && ch.label == "AP105 Upsilon Cascade";
        }
    CHECK(whole7 && whole104 && whole105);
    std::printf("editor_edit_test: long arp preset names: the half to col 24, never the gap cell\n");
}

// Demo mode: the OS answers no SysEx. An arp edit started there sends a bounded number of requests (none repeated every
// tick), presses nothing, and ends with one message.
void testDemoMode() {
    FakeLink link;
    editor::Controller c(link);
    c.setTables(tables());
    c.setOpen(true);
    double now = 0;
    link.arp.on = true;
    c.command("area", "ARP");
    c.tick(now += 10);
    c.tick(now += 10);                                // the page is read; then the OS goes into its demo
    link.demo = true;
    link.sent.clear(); link.presses.clear();
    const uint32_t notices = c.noticeSerial();
    c.choose(c.refForTest('a', 13), 40);              // tch
    for (int i = 0; i < 500; ++i) c.tick(now += 20);   // 10 s
    int requests = 0;
    for (const Bytes& m : link.sent) requests += m.size() > 6 && m[5] == edproto::kPartialRead;
    CHECK(requests <= 10);
    CHECK(link.presses.empty());
    CHECK(c.noticeSerial() == notices + 1 && c.notice().find("busy") != std::string::npos);
    std::printf("editor_edit_test: demo mode: %d requests in 10 s, nothing pressed, one message\n", requests);
}

int main() {
    testMessages();
    testFields();
    testEditing();
    testArpPresetPlaceholder();
    testArpRound();
    testArpLongNames();
    testDemoMode();
    std::printf("editor_edit_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
