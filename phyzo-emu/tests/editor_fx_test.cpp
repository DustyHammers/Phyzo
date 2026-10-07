// Editor INSERT and REVERB pages (no ROM data; docs/editor/EFFECTS_EDITOR_SPEC.md, docs/editor/ROUND3_ANSWERS.md):
//  1. Tables: a synthetic image of our own (made-up names and values, the OS's layouts): the walker tables, family
//     descriptors with sectioned records (every display sub-block, implicit and explicit PIDs), algorithm headers,
//     parameter maps, an edit-order map, a hidden placeholder, variation names, the panel codes and variation formats,
//     the suffix list, the tempo-sync names and the insert modulation's descriptor with its records in RAM.
//  2. Values as the OS shows them: lists, decimals with suffixes, formatters 80, 81 and 82.
//  3. The effect parameter message (values 0, 127, 128, negative, the 14-bit extremes).
//  4. Controller with a fake OS: the pages; an effect value is a raw-memory write of the word into the edit buffer,
//     then the effect parameter message, clamped; the algorithm is NRPN LSB 30, 31, then 32 = 0; variations and their
//     PSEt rules; under a variation the slot's values are those that play, display only (not underlined, a hint); the
//     insert modulation is one 10-byte partial write per gesture (a new destination resets its range); nothing is
//     sent while closed.
//  5. The rebuild hold: after the modulation write or NRPN LSB 30-33 no effect message for 350 ms, then the latest
//     queued value; modulation writes at least 350 ms apart.
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "editor/editor_controller.h"
#include "editor/editor_proto.h"
#include "editor/editor_tables.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_EQ(a, b) do { const auto va = (a); const auto vb = (b); if (!(va == vb)) { std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); ++failures; } } while (0)
using Bytes = std::vector<uint8_t>;
namespace A = edtables::fxaddr;

// ---- a synthetic image: fixed addresses as in the OS, everything else allocated above them
struct Image {
    Bytes img = Bytes(0xE0000, 0);
    uint32_t next = 0xD0000;
    void put8(uint32_t a, uint8_t v) { img[a - 0x4000] = v; }
    void put16(uint32_t a, uint16_t v) { put8(a, uint8_t(v >> 8)); put8(a + 1, uint8_t(v)); }
    void put32(uint32_t a, uint32_t v) { put16(a, uint16_t(v >> 16)); put16(a + 2, uint16_t(v)); }
    void putBytes(uint32_t a, const Bytes& b) { for (size_t i = 0; i < b.size(); ++i) put8(a + uint32_t(i), b[i]); }
    void putStr(uint32_t a, const std::string& s) { putBytes(a, Bytes(s.begin(), s.end())); put8(a + uint32_t(s.size()), 0); }
    uint32_t alloc(size_t n) { const uint32_t a = next; next += uint32_t((n + 3) & ~size_t(3)); return a; }
    uint32_t bytes(const Bytes& b) { const uint32_t a = alloc(b.size()); putBytes(a, b); return a; }
    uint32_t cstr(const std::string& s) { Bytes b(s.begin(), s.end()); b.push_back(0); return bytes(b); }
    uint32_t array(const std::vector<uint32_t>& v, uint32_t at = 0) {   // count, then the longs
        const uint32_t a = at ? at : alloc(4 + 4 * v.size());
        put32(a, uint32_t(v.size()));
        for (size_t i = 0; i < v.size(); ++i) put32(a + 4 + 4 * uint32_t(i), v[i]);
        return a;
    }
    uint32_t names(const std::vector<std::string>& n, uint32_t at = 0) {
        std::vector<uint32_t> p;
        for (const auto& s : n) p.push_back(cstr(s));
        return array(p, at);
    }
};

void be16(Bytes& b, int v) { b.push_back(uint8_t(uint16_t(v) >> 8)); b.push_back(uint8_t(v)); }
void be32(Bytes& b, uint32_t v) { be16(b, int(v >> 16)); be16(b, int(v & 0xFFFF)); }

// A sectioned field record: header, section 0 (display: name, list, formatter, prefix/suffix, width/decimals as
// present in `sub`), section 1 (handler), section 7 (the PID) unless `pid` < 0.
struct Disp { uint8_t sub = 0x01; uint32_t name = 0, list = 0, fmt = 0; uint8_t pre = 0, suf = 0, width = 0, dec = 0; };
Bytes record(int min, int max, const Disp& d, int pid) {
    Bytes s0 = {0, d.sub, 0, 0};
    if (d.sub & 0x01) be32(s0, d.name);
    if (d.sub & 0x02) be32(s0, d.list);
    if (d.sub & 0x04) be32(s0, d.fmt);
    if (d.sub & 0x08) { s0.push_back(d.pre); s0.push_back(d.suf); }
    if (d.sub & 0x10) { s0.push_back(d.width); s0.push_back(d.dec); }
    s0[0] = uint8_t(s0.size());
    Bytes r = {0, uint8_t(pid >= 0 ? 0x83 : 0x03), 0, 0};
    be16(r, min); be16(r, max);
    r.insert(r.end(), s0.begin(), s0.end());
    const Bytes s1 = {6, 0x01, 0, 0, 0, 9};
    r.insert(r.end(), s1.begin(), s1.end());
    if (pid >= 0) be16(r, pid);
    r[0] = uint8_t(r.size());
    return r;
}

// A family descriptor: records (with a padding byte between two of them), names, a shared offsets array, the lists.
uint32_t family(Image& im, const std::vector<Bytes>& recs, const std::vector<std::string>& names, uint32_t lists, uint32_t at = 0,
                uint32_t recsAt = 0, uint32_t namesAt = 0) {
    Bytes all;
    for (size_t i = 0; i < recs.size(); ++i) {
        all.insert(all.end(), recs[i].begin(), recs[i].end());
        if (i == 0) all.push_back(0);                // padding
    }
    const uint32_t ra = recsAt ? (im.putBytes(recsAt, all), recsAt) : im.bytes(all);
    const uint32_t na = im.names(names, namesAt);
    const uint32_t d = at ? at : im.alloc(32);
    im.put32(d, ra);
    im.put32(d + 4, na);
    im.put32(d + 8, im.array(std::vector<uint32_t>(32, 0)));
    im.put32(d + 12, lists);
    im.put16(d + 0x14, uint16_t(recs.size()));
    im.put8(d + 0x16, 2);
    return d;
}

// An algorithm header: name, family / sub, the parameter map (PIDs by slot), the variations' names.
uint32_t algo(Image& im, const std::string& name, int fam, int sub, const std::vector<int>& pids, const std::vector<std::string>& vars) {
    Bytes map;
    for (int pid : pids) {
        be16(map, pid >> 8); be16(map, pid & 0xFF); be16(map, 0); be32(map, 0); be16(map, 0); be32(map, 0);
    }
    const uint32_t ma = im.bytes(map);
    Bytes vt;
    for (const auto& v : vars) {
        const uint32_t n = im.bytes(Bytes(v.begin(), v.end()));   // a 16-character field, no terminator
        be32(vt, n); be32(vt, 0); be32(vt, 0); be32(vt, 0);
    }
    const uint32_t va = im.bytes(vt);
    const uint32_t h = im.alloc(0x20);
    im.put32(h, name.empty() ? 0 : im.bytes(Bytes(name.begin(), name.end())));
    im.put16(h + 6, uint16_t(fam));
    im.put16(h + 8, uint16_t(sub));
    im.put16(h + 0x0E, uint16_t(pids.size()));
    im.put32(h + 0x10, ma);
    im.put16(h + 0x18, uint16_t(vars.size()));
    im.put32(h + 0x1A, va);
    return h;
}

constexpr uint32_t kModRam = 0x0BE10000;             // where our modulation records "live" in RAM
uint32_t ramImage(uint32_t ram) { return A::kDataImage + (ram - A::kDataRam); }

Image buildImage() {
    Image im;
    im.putBytes(A::kRecordSizes, {0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x02});
    im.putBytes(A::kDisplaySizes, {0x04, 0x04, 0x04, 0x04, 0x02, 0x02});
    std::vector<std::string> sync;
    for (int i = 0; i < 11; ++i) sync.push_back("S" + std::to_string(i));
    for (int i = 0; i < 11; ++i) im.put32(A::kSyncNames + 4 * uint32_t(i), im.cstr(sync[size_t(i)]));
    im.putStr(A::kMsFormat, "%ldms");
    im.put32(A::kPrefixList, im.bytes({0x80, 0x00}));
    im.put32(A::kSuffixList, im.bytes({0x80, 'd', 'B', 0x0D, '%', 0x0D, 'H', 'z', 0x00}));
    // Shared value lists: 0 off/on, 1 the sync names then two more, 2 the modulation sources.
    Bytes syncList = {0x80};
    for (const auto& s : sync) { syncList.insert(syncList.end(), s.begin(), s.end()); syncList.push_back(0x0D); }
    for (char c : std::string("Free\rHeld")) syncList.push_back(uint8_t(c));
    syncList.push_back(0);
    const uint32_t lists = im.array({im.bytes({0x80, 'O', 'f', 'f', 0x0D, 'O', 'n', 0x00}), im.bytes(syncList),
                                     im.bytes({0x80, 'N', 'o', 'n', 'e', 0x0D, 'W', 'h', 'e', 'e', 'l', 0x0D, 'P', 'r', 'e', 's', 's', 0x00})});
    // Family 0: the placeholder (PID 0x0007). Family 1: insert parameters (one record without section 7: its id is
    // the previous + 1). Family 2: the routing shell's 32. Family 3: the reverb's 6.
    std::vector<Bytes> f0;
    for (int i = 0; i < 8; ++i) { Disp d; d.name = 0; f0.push_back(record(0, 0, d, i)); }
    Disp onoff; onoff.sub = 0x03; onoff.name = 0; onoff.list = 0;
    Disp gain; gain.sub = 0x19; gain.name = 1; gain.suf = 1; gain.dec = 1;
    Disp delay; delay.sub = 0x05; delay.name = 2; delay.fmt = 80;
    Disp ms; ms.sub = 0x05; ms.name = 3; ms.fmt = 81;
    Disp depth; depth.sub = 0x09; depth.name = 4; depth.suf = 2;
    Disp rate; rate.sub = 0x11; rate.name = 5; rate.dec = 2;
    std::vector<Bytes> f1 = {record(0, 1, onoff, 0x0100), record(-120, 60, gain, 0x0101), record(0, 2011, delay, 0x0102),
                             record(0, 500, ms, -1) /* 0x0103 */, record(-50, 50, depth, 0x0104), record(1, 999, rate, 0x0105)};
    std::vector<Bytes> f2;
    std::vector<std::string> routeNames;
    for (int k = 0; k < 32; ++k) {
        Disp d; d.name = uint32_t(k);
        if (k == 5) { d.sub = 0x03; d.list = 1; }    // a list starting with the tempo-sync names
        f2.push_back(record(k == 5 ? 0 : -64, k == 5 ? 12 : 63, d, 0x0200 + k));
        routeNames.push_back("Route " + std::to_string(k));
    }
    std::vector<Bytes> f3;
    for (int k = 0; k < 6; ++k) { Disp d; d.name = uint32_t(k); f3.push_back(record(0, 100, d, 0x0300 + k)); }
    im.put32(A::kFamilies + 0, family(im, f0, {"????"}, lists));
    im.put32(A::kFamilies + 4, family(im, f1, {"Switch", "Gain", "DelayTime", "Spread", "Depth", "LfoRate"}, lists));
    im.put32(A::kFamilies + 8, family(im, f2, routeNames, lists));
    im.put32(A::kFamilies + 12, family(im, f3, {"Size", "Decay", "Damping", "PreDelay", "Diffusion", "Tail"}, lists));
    // Algorithms: A (2/0) with a hidden slot and an edit-order map, B (2/1) in natural order, 39 more, the last
    // without a stored name; the routing shell (9/0) and the reverb (1/0).
    std::vector<uint32_t> ins = {algo(im, "      Alpha Algo", 2, 0, {0x0100, 0x0101, 0x0007, 0x0102}, {"       First~Var", "      Second~Var"}),
                                 algo(im, "       Beta Algo", 2, 1, {0x0103, 0x0104, 0x0105}, {"            Only"})};
    for (int i = 2; i < 41; ++i) ins.push_back(algo(im, i == 40 ? "" : "Algo " + std::to_string(i), 3, i, {0x0104}, {"               V"}));
    for (int i = 0; i < 41; ++i) im.put32(A::kInsertAlgos + 4 * uint32_t(i), ins[size_t(i)]);
    std::vector<int> routing;
    for (int k = 0; k < 32; ++k) routing.push_back(0x0200 + k);
    im.put32(A::kRoutingAlgo, algo(im, "", 9, 0, routing, {}));
    im.put32(A::kReverbAlgo, algo(im, "", 1, 0, {0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0305}, {"            Hall", "            Room", "           Plate"}));
    im.put32(A::kEditOrder, im.bytes({2, 0, 3, 3, 0, 1, 0}));   // A: slots 3, 0, 1 (2 is the placeholder)
    im.put32(A::kEditOrder + 4, 0);
    for (int i = 0; i < 41; ++i) im.putStr(A::kCodes + 5 * uint32_t(i), "c" + std::string(i < 10 ? "0" : "") + std::to_string(i) + "x");
    im.putStr(A::kPreset, "PSEt");
    im.putStr(A::kInsertVariationFmt, " u%02d");
    im.putStr(A::kReverbVariationFmt, " r%02d");
    // The insert modulation: the descriptor in the image, its records, names and offsets in RAM (initial copy).
    std::vector<Bytes> mod;
    Disp src; src.sub = 0x03; src.name = 0; src.list = 2;
    Disp num; num.sub = 0x01;
    Disp dst; dst.sub = 0x05; dst.name = 3; dst.fmt = 82;
    num.name = 1; mod.push_back(record(0, 2, src, -1));
    mod.push_back(record(0, 127, num, -1));
    num.name = 2; mod.push_back(record(0, 127, num, -1));
    mod.push_back(record(0, 0, dst, -1));
    num.name = 4; mod.push_back(record(-32768, 32767, num, -1));
    num.name = 5; mod.push_back(record(-32768, 32767, num, -1));
    mod[1] = record(0, 127, [] { Disp d; d.name = 1; return d; }(), -1);
    const uint32_t d = A::kModDescriptor;
    family(im, mod, {"ModSource", "SourceMin", "SourceMax", "ModDest", "DestMin", "DestMax"}, lists, d, ramImage(kModRam), ramImage(kModRam + 0x400));
    im.put32(d, kModRam);
    im.put32(d + 4, kModRam + 0x400);
    im.array({0, 2, 4, 1, 6, 8}, ramImage(kModRam + 0x600));
    im.put32(d + 8, kModRam + 0x600);
    im.put8(d + 0x16, 0);
    return im;
}

void testTables(const edtables::FxTables& t) {
    using namespace edtables;
    CHECK(t.ok);
    if (!t.ok) { std::printf("editor_fx_test: %s\n", t.error.c_str()); return; }
    CHECK(t.inserts.size() == 41 && t.codes.size() == 41 && t.codes[7] == "c07x" && t.inserts[40].name.empty());
    CHECK(t.preset == "PSEt" && t.insertVariationFmt == " u%02d" && t.reverbVariationFmt == " r%02d");
    const FxAlgo& a = t.inserts[0];
    CHECK(a.family == 2 && a.sub == 0 && a.params.size() == 4 && a.name == "      Alpha Algo");
    CHECK((a.order == std::vector<int>{3, 0, 1}));                     // the edit-order map
    CHECK((a.variations == std::vector<std::string>{"       First Var", "      Second Var"}));   // '~' shown as a space
    CHECK(a.params[0].name == "Switch" && a.params[0].hasList && a.params[0].list.size() == 2);
    CHECK(a.params[1].name == "Gain" && a.params[1].min == -120 && a.params[1].max == 60 && a.params[1].decimals == 1 && a.params[1].suffix == 1);
    CHECK(a.params[2].hidden() && a.params[3].formatter == 80 && a.params[3].tempoSync);
    const FxAlgo& b = t.inserts[1];
    CHECK((b.order == std::vector<int>{0, 1, 2}));                    // no map: the natural order
    CHECK(b.params[0].found && b.params[0].name == "Spread" && b.params[0].formatter == 81);   // the implicit PID 0x0103
    CHECK(b.params[1].suffix == 2 && b.params[2].decimals == 2 && !b.params[2].tempoSync);
    CHECK(t.routing.params.size() == 32 && t.routing.params[13].name == "Route 13" && t.routing.params[5].tempoSync);
    CHECK(t.reverb.params.size() == 6 && t.reverb.variations.size() == 3 && t.reverb.family == 1);
    CHECK(t.mod.size() == 6 && (t.modOffsets == std::vector<uint32_t>{0, 2, 4, 1, 6, 8}));
    CHECK(t.mod[0].name == "ModSource" && t.mod[0].hasList && t.mod[0].list.size() == 3 && t.mod[3].formatter == 82);
    CHECK((t.suffixes == std::vector<std::string>{"dB", "%", "Hz"}) && t.syncNames.size() == 11 && t.msFormat == "%ldms");
    std::printf("editor_fx_test: effect tables read back from a synthetic image\n");

    // Values as the OS shows them.
    CHECK_EQ(formatEffect(a.params[0], 1, t, &a), std::string("On"));
    CHECK_EQ(formatEffect(a.params[1], 125, t, &a), std::string("12.5dB"));
    CHECK_EQ(formatEffect(a.params[1], -5, t, &a), std::string("-0.5dB"));
    CHECK_EQ(formatEffect(a.params[3], 3, t, &a), std::string("S3"));
    CHECK_EQ(formatEffect(a.params[3], 11, t, &a), std::string("0ms"));
    CHECK_EQ(formatEffect(a.params[3], 111, t, &a), std::string("100ms"));
    CHECK_EQ(formatEffect(b.params[0], 250, t, &a), std::string("250ms"));
    CHECK_EQ(formatEffect(b.params[1], -7, t, &a), std::string("-7%"));
    CHECK_EQ(formatEffect(b.params[2], 5, t, &a), std::string("0.05"));
    CHECK_EQ(formatEffect(t.mod[3], 1, t, &a), std::string("Gain"));   // formatter 82: the insert slot's name
    std::printf("editor_fx_test: lists, decimals, suffixes and formatters 80-82\n");
}

void testMessage() {
    using edproto::fxParameter;
    auto msg = [](int slot, int param, uint8_t lo, uint8_t hi) {
        return Bytes{0xF0, 0x0F, 0x7F, 0x7F, 0, 0, 0, 0, uint8_t(slot), 0, uint8_t(param), lo, hi, 0xF7};
    };
    CHECK(fxParameter(1, 3, 0) == msg(1, 3, 0x00, 0x00));
    CHECK(fxParameter(0, 13, 127) == msg(0, 13, 0x7F, 0x00));
    CHECK(fxParameter(3, 5, 128) == msg(3, 5, 0x00, 0x01));
    CHECK(fxParameter(1, 0, -1) == msg(1, 0, 0x7F, 0x7F));
    CHECK(fxParameter(1, 0, -100) == msg(1, 0, 0x1C, 0x7F));
    CHECK(fxParameter(1, 31, 8191) == msg(1, 31, 0x7F, 0x3F));
    CHECK(fxParameter(1, 31, -8192) == msg(1, 31, 0x00, 0x40));
    CHECK(fxParameter(0, 0, 0).size() == 14);
    std::printf("editor_fx_test: the effect parameter message\n");
}

// ---- controller with a fake OS: the preset edit buffer and the running performance's effect block
struct FakeLink : editor::Link {
    Bytes preset = Bytes(414, 0), playing = Bytes(0x2CC + 0x10E, 0);
    std::vector<Bytes> sent, replies;
    std::vector<double> sentAt;                      // when each message was sent (with `clock`)
    const double* clock = nullptr;
    bool open = false;
    void setOpen(bool on) override { open = on; }
    bool send(const Bytes& m) override {
        sent.push_back(m);
        sentAt.push_back(clock ? *clock : 0);
        if (m.size() > 20 && m[5] == edproto::kPartialRead) {
            edproto::Address a;
            a.type = m[6]; a.number = uint16_t(edproto::get14(&m[7])); a.subtype = m[9];
            a.subnumber = uint16_t(edproto::get14(&m[10])); a.offset = edproto::get32(&m[12]); a.length = edproto::get32(&m[17]);
            const Bytes* obj = a.type == edproto::kPreset && a.number == 0 ? &preset
                               : a.type == edproto::kPerformance && a.number == edproto::kEditBuffer && a.subtype == 0 ? &playing : nullptr;
            if (obj && a.offset + a.length <= obj->size()) {
                Bytes r = edproto::partialWrite(a, Bytes(obj->begin() + long(a.offset), obj->begin() + long(a.offset + a.length)));
                r[4] = 0; r[5] = edproto::kPartialReply;
                replies.push_back(r);
            }
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
    std::array<char, 4> display() const override { return {{'P', ' ', '0', '1'}}; }
    bool running() const override { return true; }
    void word(Bytes& b, uint32_t off, int v) { b[off] = uint8_t(uint16_t(v) >> 8); b[off + 1] = uint8_t(v); }
    bool sentAny(uint8_t status) const { for (const Bytes& m : sent) if (!m.empty() && (m[0] & 0xF0) == status) return true; return false; }
    int writes() const { int n = 0; for (const Bytes& m : sent) n += m.size() > 6 && m[0] == 0xF0 && m[5] != edproto::kPartialRead; return n; }
};

const edpages::Screen::Span* spanOf(const editor::View& v, int ref) {
    for (const auto& s : v.screen.values) if (s.ref == ref) return &s;
    return nullptr;
}

int refOfLabel(editor::Controller& c, const std::string& text) {   // the value reference after a label on screen
    const editor::View v = c.view();
    for (int r = 0; r < edpages::kRows; ++r) {
        const size_t at = v.screen.rowText(r).find(text);
        if (at == std::string::npos) continue;
        int best = -1, dist = 1 << 30;
        for (const auto& s : v.screen.values)
            if (s.row == r && s.col > int(at) && s.col - int(at) < dist) { dist = s.col - int(at); best = s.ref; }
        return best;
    }
    return -1;
}

void testController(const edtables::FxTables& fx) {
    FakeLink link;
    editor::Controller c(link);
    edtables::OsTables t;
    t.ok = true;
    t.fx = fx;
    c.setTables(t);
    // The edit buffer: algorithm A selected and stored, PSEt; reverb variation r01 over a stored family 1 / sub 0.
    Bytes& p = link.preset;
    p[330] = 2; p[331] = 0; p[332] = 0xFF; p[333] = 0;
    link.word(p, 174, 2); link.word(p, 176, 0); link.word(p, 294, 1); link.word(p, 296, 0);
    link.word(p, 194 + 2, -30);                      // A slot 1 (Gain): -3.0dB
    link.word(p, 194 + 6, 20);                       // A slot 3 (DelayTime): S9 ... 20 = 9ms
    link.word(p, 314, 40);                           // reverb slot 0
    link.word(p, 110 + 2 * 11, 7); link.word(p, 136, 50); link.word(p, 292, 77);
    p[64] = 1; p[65] = 1; link.word(p, 66, 0); link.word(p, 68, 127); link.word(p, 70, -120); link.word(p, 72, 60);
    // What plays (the running performance): the reverb variation's own values.
    link.word(link.playing, 0x2CC + (314 - 60), 88);
    double now = 0;
    auto tick = [&](int n = 1) { for (int i = 0; i < n; ++i) c.tick(now += 20); };

    // Closed: nothing.
    c.command("area", "INSERT");
    tick(5);
    CHECK(link.sent.empty());
    c.setOpen(true);
    tick(3);
    editor::View v = c.view();
    CHECK(v.areas.count("INSERT") && v.areas.count("REVERB") && v.area == "INSERT");
    CHECK((v.sections == std::set<std::string>{"ALGO", "MOD", "SENDS"}) && v.pages == 2);   // page 1 + A's 3 parameters
    CHECK(v.screen.rowText(0).find("ALGO Alpha Algo") == 0 && v.screen.rowText(0).find("VARIATION PSEt") != std::string::npos);
    CHECK(v.screen.rowText(1).find("ROUTE 13") == 0 && v.screen.rowText(1).find("50") != std::string::npos);
    c.command("page", "2");
    tick();
    v = c.view();
    CHECK(v.screen.rowText(0).find("DELAY TIME") == 0 && v.screen.rowText(0).find("9ms") != std::string::npos);   // edit order
    CHECK(v.screen.rowText(0).find("SWITCH") == 25);
    CHECK(v.screen.rowText(1).find("GAIN") == 0 && v.screen.rowText(1).find("-3.0dB") != std::string::npos);

    // An insert value: the word into the edit buffer (raw memory), then the effect parameter message; clamped.
    const int gain = refOfLabel(c, "GAIN");
    link.sent.clear();
    c.command("type", std::to_string(gain));
    for (char ch : std::string("1.5")) c.key(std::string(1, ch));
    c.key("enter");
    CHECK(link.sent.size() == 2);
    if (link.sent.size() == 2) {
        CHECK(link.sent[0] == edproto::partialWrite({edproto::kRawMemory, 0, 0, 0, edproto::kEditBufferRam + 196, 2}, {0x00, 15}));
        CHECK(link.sent[1] == edproto::fxParameter(1, 1, 15));
    }
    tick(5);
    link.sent.clear();
    c.choose(gain, 5000);
    CHECK(link.sent.size() == 2 && link.sent.size() == 2 && link.sent[1] == edproto::fxParameter(1, 1, 60));   // max 60
    CHECK(!link.sentAny(0xB0));                      // never NRPN for effect values
    for (const Bytes& m : link.sent) CHECK(!(m.size() > 6 && m[5] == edproto::kPartialWrite && m[6] == edproto::kPreset));
    tick(5);

    // The algorithm: NRPN MSB 1 LSB 30 = family, 31 = sub, then 32 = 0, in this order.
    c.command("page", "1");
    tick();
    const int algoRef = refOfLabel(c, "ALGO");
    CHECK(c.menu(algoRef).groups.size() + c.menu(algoRef).items.size() > 0);
    link.sent.clear();
    c.choose(algoRef, 1);
    Bytes want;
    std::vector<Bytes> wantMsgs;
    for (auto& m : edproto::nrpn(0, 1, 30, 2)) wantMsgs.push_back(m);
    for (auto& m : edproto::nrpn(0, 1, 31, 1)) wantMsgs.push_back(m);
    for (auto& m : edproto::nrpn(0, 1, 32, 0)) wantMsgs.push_back(m);
    CHECK(link.sent == wantMsgs);
    // The OS now runs B with its first variation: its parameters, display only; B's pages.
    p[330] = 2; p[331] = 1; p[332] = 0;
    link.word(link.playing, 0x2CC + (194 - 60), 300);   // what plays: B slot 0 = 300ms
    tick(10);
    v = c.view();
    CHECK(v.screen.rowText(0).find("ALGO Beta Algo") == 0 && v.screen.rowText(0).find("VARIATION  u01") != std::string::npos);
    CHECK(v.pages == 2);
    // PSEt is not offered: the selected algorithm (B) is not the stored one (A).
    const int var = refOfLabel(c, "VARIATION");
    editor::Menu vm = c.menu(var);
    CHECK(!vm.items.empty() && vm.items[0].value == 0 && vm.items[0].label == " u01 Only");
    c.command("page", "2");
    tick();
    v = c.view();
    const int spread = refOfLabel(c, "SPREAD");
    const auto* sp = spanOf(v, spread);
    CHECK(sp && v.screen.rowText(sp->row).find("300ms") != std::string::npos);   // the value that plays
    CHECK(sp && !v.screen.underline[size_t(sp->row)][size_t(sp->col)]);          // display only: not underlined
    const uint32_t notices = c.noticeSerial();
    link.sent.clear();
    c.command("drag_begin", std::to_string(spread));
    c.command("drag", "5");
    c.command("drag_end", "");
    c.choose(spread, 10);
    tick(5);
    CHECK(link.writes() == 0 && !link.sentAny(0xB0));
    CHECK(c.noticeSerial() > notices && c.notice() == "select PSEt to edit");

    // Back to A with PSEt (selected = stored): editable again; PSEt offered.
    p[330] = 2; p[331] = 0; p[332] = 0xFF;
    c.command("page", "1");
    tick(10);
    vm = c.menu(refOfLabel(c, "VARIATION"));
    CHECK(!vm.items.empty() && vm.items[0].value == -1 && vm.items[0].label == "PSEt" && vm.items.size() == 3);
    link.sent.clear();
    c.choose(refOfLabel(c, "VARIATION"), 1);
    std::vector<Bytes> varMsgs = edproto::nrpn(0, 1, 32, 1);
    CHECK(link.sent == varMsgs);
    p[332] = 1;                                      // the OS selected u02
    tick(20);                                        // (past the rebuild hold)
    // The insert mix with a variation: word 292, then the effect message slot 0, param 13.
    const int mix = refOfLabel(c, "ROUTE 13");
    link.sent.clear();
    c.choose(mix, 30);
    CHECK(link.sent.size() == 2 && link.sent[0] == edproto::partialWrite({edproto::kRawMemory, 0, 0, 0, edproto::kEditBufferRam + 292, 2}, {0, 30}) &&
          link.sent[1] == edproto::fxParameter(0, 13, 30));
    tick(5);
    p[332] = 0xFF;
    tick(10);

    // SENDS: routing slots; the FX-bus 1 reverb send (slot 6) plays the insert variation's value.
    c.command("section", "SENDS");
    tick(3);
    v = c.view();
    CHECK(v.screen.rowText(0).find("ROUTE 6") == 0 && v.screen.rowText(0).find("ROUTE 7") == 25);
    CHECK(v.screen.rowText(3).find("ROUTE 17") == 0 && v.screen.rowText(3).find("ROUTE 3") == 25);
    link.sent.clear();
    c.choose(refOfLabel(c, "ROUTE 7"), 12);
    CHECK(link.sent.size() == 2 && link.sent[1] == edproto::fxParameter(0, 7, 12));
    tick(5);

    // MOD: one 10-byte partial write of the edit buffer per gesture; a new destination resets dest min / max.
    c.command("section", "MOD");
    tick(3);
    v = c.view();
    CHECK(v.screen.rowText(0).find("MOD SOURCE") == 0 && v.screen.rowText(0).find("Wheel") != std::string::npos);
    CHECK(v.screen.rowText(0).find("MOD DEST") == 25 && v.screen.rowText(0).find("Gain") != std::string::npos);
    CHECK(v.screen.rowText(2).find("-12.0dB") != std::string::npos);            // dest min in the destination's units
    const int dest = refOfLabel(c, "MOD DEST");
    link.sent.clear();
    c.choose(dest, 3);                                                           // DelayTime: tempo-syncable
    Bytes blk = {1, 3, 0, 0, 0, 127, 0, 11, uint8_t(2011 >> 8), uint8_t(2011 & 0xFF)};
    CHECK(link.sent.size() == 1 && link.sent[0] == edproto::partialWrite({edproto::kPreset, 0, 0, 0, 64, 10}, blk));
    for (size_t i = 0; i < blk.size(); ++i) p[64 + i] = blk[i];                 // the OS holds it
    tick(20);                                                                    // (past the rebuild hold)
    // A drag writes once, on release; a write within 350 ms of the last waits (the rebuild hold).
    const int srcMin = refOfLabel(c, "SOURCE MIN");
    link.sent.clear();
    c.command("drag_begin", std::to_string(srcMin));
    for (int i = 1; i <= 5; ++i) { c.command("drag", std::to_string(i)); tick(); }
    CHECK(link.writes() == 0);
    c.command("drag_end", "");
    CHECK(link.writes() == 1 && link.sent.back() == edproto::partialWrite({edproto::kPreset, 0, 0, 0, 64, 10}, {1, 3, 0, 5, 0, 127, 0, 11, uint8_t(2011 >> 8), uint8_t(2011 & 0xFF)}));
    c.key("plus");
    CHECK(link.writes() == 1);                       // within the hold: waits
    for (int i = 0; i < 10; ++i) c.tick(now += 20);
    CHECK(link.writes() == 1);                       // 200 ms: still waiting
    for (int i = 0; i < 8; ++i) c.tick(now += 20);
    CHECK(link.writes() == 2);                       // 360 ms
    // Under an insert variation: display only.
    p[332] = 0;
    tick(20);
    link.sent.clear();
    c.choose(dest, 0);
    tick(10);
    CHECK(link.writes() == 0);
    p[332] = 0xFF;
    tick(10);

    // REVERB: the variation (r01) plays: its values, display only; PSEt offered (stored family 1 / sub 0).
    c.command("area", "REVERB");
    tick(3);
    v = c.view();
    CHECK((v.sections == std::set<std::string>{"ALGO", "SENDS"}));
    CHECK(v.screen.rowText(0).find("VARIATION") == 0 && v.screen.rowText(0).find("r01") != std::string::npos);
    CHECK(v.screen.rowText(1).find("SIZE") == 0 && v.screen.rowText(1).find("88") != std::string::npos);
    const int size = refOfLabel(c, "SIZE");
    link.sent.clear();
    c.choose(size, 10);
    CHECK(link.writes() == 0);
    vm = c.menu(refOfLabel(c, "VARIATION"));
    CHECK(!vm.items.empty() && vm.items[0].label == "PSEt" && vm.items[1].label == " r01 Hall" && vm.items.size() == 4);
    link.sent.clear();
    c.choose(refOfLabel(c, "VARIATION"), -1);
    CHECK(link.sent == edproto::nrpn(0, 1, 33, -1));
    p[333] = 0xFF;                                   // the OS selected PSEt
    tick(20);                                        // (past the rebuild hold)
    v = c.view();
    CHECK(v.screen.rowText(1).find("40") != std::string::npos);                 // PSEt: the stored value, editable
    link.sent.clear();
    c.choose(size, 10);
    CHECK(link.sent.size() == 2 && link.sent[0] == edproto::partialWrite({edproto::kRawMemory, 0, 0, 0, edproto::kEditBufferRam + 314, 2}, {0, 10}) &&
          link.sent[1] == edproto::fxParameter(3, 0, 10));
    tick(5);
    // The reverb's PSEt only over a stored family 1 / sub 0.
    p[333] = 2; link.word(p, 294, 2);
    c.command("section", "SENDS");                   // (re-read on opening the page)
    c.command("section", "ALGO");
    tick(10);
    vm = c.menu(refOfLabel(c, "VARIATION"));
    CHECK(!vm.items.empty() && vm.items[0].value == 0);
    c.command("section", "SENDS");
    tick(3);
    v = c.view();
    CHECK(v.screen.rowText(0).find("ROUTE 25") == 0 && v.screen.rowText(3).find("ROUTE 2") == 0);
    // Closing: nothing more.
    c.setOpen(false);
    link.sent.clear();
    c.choose(size, 20);
    tick(10);
    CHECK(link.sent.empty());
    std::printf("editor_fx_test: INSERT and REVERB pages, write paths, variations, the modulation block\n");
}
// The rebuild hold (docs/editor/ROUND3_ANSWERS.md 4, spec write path 5a).
void testRebuildHold(const edtables::FxTables& fx) {
    FakeLink link;
    editor::Controller c(link);
    edtables::OsTables t;
    t.ok = true;
    t.fx = fx;
    c.setTables(t);
    Bytes& p = link.preset;
    p[330] = 2; p[331] = 0; p[332] = 0xFF; p[333] = 0xFF;
    link.word(p, 174, 2); link.word(p, 176, 0); link.word(p, 294, 1); link.word(p, 296, 0);
    p[64] = 1; p[65] = 1; link.word(p, 66, 0); link.word(p, 68, 127); link.word(p, 70, -120); link.word(p, 72, 60);
    double now = 0;
    link.clock = &now;
    auto tick = [&](int n = 1) { for (int i = 0; i < n; ++i) c.tick(now += 10); };
    c.setOpen(true);
    c.command("area", "INSERT");
    tick(3);
    const int algoRef = refOfLabel(c, "ALGO"), varRef = refOfLabel(c, "VARIATION");
    const int probe = refOfLabel(c, "ROUTE 11");     // a routing slot: editable under any variation
    auto fxAt = [&](int value, double& at) {         // the effect message for the probe with this value, and when it went
        for (size_t i = 0; i < link.sent.size(); ++i)
            if (link.sent[i] == edproto::fxParameter(0, 11, value)) { at = link.sentAt[i]; return true; }
        return false;
    };
    auto effectMessages = [&] { int n = 0; for (const Bytes& m : link.sent) n += m.size() == 14 && m[2] == 0x7F && m[3] == 0x7F; return n; };
    // After each rebuild message (algorithm: LSB 30, 31, 32; insert variation: 32; reverb variation: 33): no effect
    // message for 350 ms, then only the latest queued value, raw write first.
    const int reverbVar = [&] { c.command("area", "REVERB"); tick(3); const int r = refOfLabel(c, "VARIATION");
                                c.command("area", "INSERT"); tick(3); return r; }();
    struct Trigger { int ref; int64_t value; };
    for (const Trigger& tr : {Trigger{algoRef, 0}, Trigger{varRef, -1}, Trigger{reverbVar, -1}}) {
        tick(40);                                    // (well clear of the previous hold)
        link.sent.clear(); link.sentAt.clear();
        c.choose(tr.ref, tr.value);
        const double t0 = now;
        CHECK(!link.sent.empty() && link.sent.back()[0] == 0xB0);   // the NRPN went out
        c.choose(probe, 10);
        tick(10);
        c.choose(probe, 20);                         // the latest value wins
        while (now < t0 + 340) tick();
        CHECK(effectMessages() == 0);
        while (now < t0 + 400) tick();
        double at = 0;
        CHECK(effectMessages() == 1 && fxAt(20, at) && at >= t0 + 350);
        CHECK(!fxAt(10, at));
        size_t raw = link.sent.size(), msg = link.sent.size();
        for (size_t i = 0; i < link.sent.size(); ++i) {
            if (link.sent[i] == edproto::partialWrite({edproto::kRawMemory, 0, 0, 0, edproto::kEditBufferRam + 132, 2}, {0, 20})) raw = i;
            if (link.sent[i] == edproto::fxParameter(0, 11, 20)) msg = i;
        }
        CHECK(raw < msg);                            // raw write, then the effect message
        // The read-back comes after the hold.
        bool earlyRead = false;
        for (size_t i = 0; i < link.sent.size(); ++i)
            earlyRead |= link.sent[i].size() > 6 && link.sent[i][5] == edproto::kPartialRead && link.sentAt[i] > t0 && link.sentAt[i] < t0 + 350;
        CHECK(!earlyRead);
    }
    // After the modulation write: no effect message for 350 ms; two modulation choices 200 ms apart: the second write
    // no earlier than 350 ms after the first.
    c.command("section", "MOD");
    tick(40);
    const int src = refOfLabel(c, "MOD SOURCE");
    link.sent.clear(); link.sentAt.clear();
    c.choose(src, 2);
    const double t0 = now;
    auto modWrites = [&](std::vector<double>& at) {
        at.clear();
        for (size_t i = 0; i < link.sent.size(); ++i)
            if (link.sent[i].size() > 20 && link.sent[i][5] == edproto::kPartialWrite && link.sent[i][6] == edproto::kPreset) at.push_back(link.sentAt[i]);
        return at.size();
    };
    std::vector<double> at;
    CHECK(modWrites(at) == 1 && at[0] == t0);
    c.command("section", "ALGO");
    c.choose(probe, 30);
    while (now < t0 + 200) tick();
    c.command("section", "MOD");
    c.choose(src, 0);                                // 200 ms after the first
    while (now < t0 + 340) tick();
    double fxTime = 0;
    CHECK(effectMessages() == 0 && modWrites(at) == 1);
    while (now < t0 + 800) tick();
    CHECK(fxAt(30, fxTime) && fxTime >= t0 + 350);
    CHECK(modWrites(at) == 2 && at[1] >= at[0] + 350);
    std::printf("editor_fx_test: the rebuild hold (350 ms after the modulation write and NRPN LSB 30-33)\n");
}
}  // namespace

int main() {
    const Image im = buildImage();
    const edtables::FxTables fx = edtables::readEffects(im.img);
    testTables(fx);
    testMessage();
    if (fx.ok) testController(fx);
    if (fx.ok) testRebuildHold(fx);
    std::printf("editor_fx_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
