#include "editor_fields.h"
#include <algorithm>
#include "editor_pages.h"
#include "editor_proto.h"

namespace edfields {

namespace {
// A code from the OS (trimmed, up to its first format character), or the neutral label if the OS has none.
std::string code(const std::string& os, const char* fallback) {
    std::string s = edpages::trimmed(os.substr(0, os.find('%')));
    return s.empty() ? std::string(fallback) : s;
}
std::string onText(const edtables::ArpText& t) { const std::string s = edpages::trimmed(t.on); return s.empty() ? "ON" : s; }
std::string offText(const edtables::ArpText& t) { const std::string s = edpages::trimmed(t.off); return s.empty() ? "OFF" : s; }
}  // namespace

std::vector<Field> commonFields(const std::array<std::string, 5>& knobLabels) {
    std::vector<Field> out;
    for (int n = 0; n < 4; ++n) {
        const uint32_t b = 28 + 8 * uint32_t(n);
        const int l = 2 + 7 * n;
        auto slot = [&](const char* label, uint32_t off, Kind kind, int hi, std::vector<std::string> list, int lsb) {
            Field f;
            f.label = label; f.section = "SLOTS"; f.slot = n; f.offset = off; f.kind = kind; f.hi = hi;
            f.list = std::move(list); f.nrpnLsb = lsb; f.editable = true;
            out.push_back(f);
        };
        slot("MODE", b + 2, Kind::List, 3, {"OFF", "LAYER", "SPLIT", "USER"}, l + 2);
        slot("OSC", b + 3, Kind::List, 3, {"NONE", "1", "2", "1+2"}, l + 3);
        slot("LOW", b + 0, Kind::Note, 127, {}, l);
        slot("HIGH", b + 1, Kind::Note, 127, {}, l + 1);
    }
    const uint32_t knobOffsets[5] = {34, 35, 42, 43, 50};
    const int knobLsbs[5] = {7, 8, 14, 15, 21};
    for (size_t k = 0; k < 5; ++k) {
        Field f;
        f.label = knobLabels[k].empty() ? "PERF" + std::to_string(k + 1) : knobLabels[k];
        f.section = "KNOBS"; f.offset = knobOffsets[k]; f.nrpnLsb = knobLsbs[k]; f.editable = true;
        out.push_back(f);
    }
    return out;                                      // (the insert modulation is on INSERT > MOD)
}

std::vector<Field> arpFields(const edtables::ArpText& t) {
    std::vector<Field> out;
    auto add = [&](const std::string& label, uint32_t off, int size, Kind kind, int lo, int hi, bool editable) -> Field& {
        Field f;
        f.label = label; f.area = 'a'; f.offset = off; f.size = size; f.kind = kind; f.lo = lo; f.hi = hi;
        f.editable = editable;
        out.push_back(f);
        return out.back();
    };
    // The arp-control numbers (the OS's arp-control variable): 0 Tempo, 1 Range, 2 PrSt, 3 nrES, 4 ndur, 5 FEEL,
    // 6-10 the intervals, 11 Lo n, 12 Hi n, 13 tch, 14 CEnt, 15 ntLt, 16 HELd.
    // The arp preset's upper end is the controller's: the last arp preset the OS answers a name read for.
    Field& prst = add(code(t.presetCode, "PRST"), 0, 4, Kind::PresetAction, -1, -1, true);
    prst.arpControl = 2; prst.span = 2;
    add(code(t.resolutionCode, "RES"), 402, 4, Kind::Resolution, 0, 11, true).arpControl = 3;
    add(code(t.duration, "DUR"), 385, 1, Kind::Duration, 0, 99, true).arpControl = 4;
    Field& pattern = add("PATTERN", 390, 2, Kind::Pattern, 0, 0, false);
    pattern.nameType = edproto::kArpPattern; pattern.span = 2;
    Field& riff = add("RIFF", 394, 2, Kind::Pattern, 0, 0, false);
    riff.nameType = edproto::kArpRiff; riff.span = 2;
    Field& feel = add(code(t.feel, "FEEL"), 392, 2, Kind::Feel, -1, 999, true);
    feel.arpControl = 5; feel.nameType = edproto::kArpFeel; feel.span = 2;
    static const char* const kIntervals[5] = {"-3RD", "3RD", "4TH", "5TH", "OCT"};
    for (uint32_t i = 0; i < 5; ++i)
        add(code(t.intervals[i], kIntervals[i]), 361 + i, 1, Kind::Interval, 0, 1, true).arpControl = 6 + int(i);
    Field& lo = add(code(t.low, "LO"), 370, 1, Kind::Note, 0, 127, true);   // input filter; output bounds at 372
    lo.mirror = 372; lo.arpControl = 11;
    Field& hi = add(code(t.high, "HI"), 371, 1, Kind::Note, 0, 127, true);
    hi.mirror = 373; hi.arpControl = 12;
    add(code(t.touch, "TCH"), 384, 1, Kind::Touch, 0, 127, true).arpControl = 13;
    add(code(t.centre, "CENT"), 382, 1, Kind::OnOff, 0, 1, true).arpControl = 14;
    add(code(t.limit, "LIM"), 386, 1, Kind::Number, 0, 99, true).arpControl = 15;
    add(code(t.hold, "HOLD"), 351, 1, Kind::OnOff, 0, 1, true).arpControl = 16;
    Field& on = add("ARP", 0, 1, Kind::RuntimeFlag, 0, 1, true);
    on.flag = 0; on.button = kBtnArpOnOff;
    Field& kbd = add("KBD", 0, 1, Kind::RuntimeFlag, 0, 1, true);
    kbd.flag = 1; kbd.button = kBtnArpKeyboard;
    add("TMPO", 406, 4, Kind::Tempo, 25, 320, true).arpControl = 0;
    add("UP", 367, 1, Kind::OctaveUp, 0, 4, true).arpControl = 1;      // the Range control sets up and down together
    add("DOWN", 368, 1, Kind::OctaveDown, 0, 4, true).arpControl = 1;
    add("NAME", 334, 16, Kind::Text, 0, 0, false);
    return out;
}

bool rawOf(const Field& f, const uint8_t* bytes, size_t size, uint32_t base, int64_t& raw) {
    if (f.size <= 0 || f.size > 4 || f.offset < base || size_t(f.offset - base) + size_t(f.size) > size) return false;
    const uint8_t* p = bytes + (f.offset - base);
    uint32_t v = 0;
    for (int i = 0; i < f.size; ++i) v = (v << 8) | p[i];
    if (f.kind == Kind::OctaveDown) raw = int8_t(uint8_t(v));
    else if (f.kind == Kind::Signed16) raw = int16_t(uint16_t(v));
    else raw = v;
    return true;
}

void store(const Field& f, int64_t raw, uint8_t* bytes, size_t size, uint32_t base) {
    if (f.size <= 0 || f.size > 4 || f.offset < base || size_t(f.offset - base) + size_t(f.size) > size) return;
    uint8_t* p = bytes + (f.offset - base);
    for (int i = 0; i < f.size; ++i) p[i] = uint8_t(uint64_t(raw) >> (8 * (f.size - 1 - i)));
}

int64_t toDomain(const Field& f, int64_t raw) {
    switch (f.kind) {
    case Kind::OnOff: case Kind::Interval: return raw != 0 ? 1 : 0;
    case Kind::Inverted: return raw == 0 ? 1 : 0;
    case Kind::OctaveUp: return raw / 12;
    case Kind::OctaveDown: return -raw / 12;
    case Kind::Duration: return raw * 100 / 256;
    case Kind::Feel: return raw == kNone16 ? -1 : raw;
    case Kind::RuntimeFlag: return raw != 0 ? 1 : 0;
    case Kind::PresetAction: return int32_t(uint32_t(raw)) < 0 ? -1 : raw;
    case Kind::Resolution:
        for (int i = 0; i < 12; ++i) if (kResolutionTicks[i] == raw) return i;
        return -1;
    default: return raw;
    }
}

int64_t toRaw(const Field& f, int64_t d) {
    switch (f.kind) {
    case Kind::OnOff: case Kind::Interval: return d ? f.onRaw : 0;
    case Kind::Inverted: return d ? 0 : 1;
    case Kind::OctaveUp: return 12 * d;
    case Kind::OctaveDown: return -12 * d;
    case Kind::Duration: return std::max<int64_t>(1, (d * 256 + 99) / 100);   // the smallest raw that shows d
    case Kind::Resolution: return d >= 0 && d < 12 ? kResolutionTicks[d] : kResolutionTicks[4];
    case Kind::Feel: return d < 0 ? kNone16 : d;
    default: return d;
    }
}

std::string format(const Field& f, int64_t d, const edtables::ArpText& t) {
    switch (f.kind) {
    case Kind::List: return d >= 0 && size_t(d) < f.list.size() ? f.list[size_t(d)] : std::to_string(d);
    case Kind::Note: return edpages::noteName(int(d));
    case Kind::OnOff: case Kind::Interval: case Kind::Inverted: return d ? onText(t) : offText(t);
    case Kind::OctaveUp: case Kind::OctaveDown:
        return edtables::formatNumber(t.rangeFmt.empty() ? std::string("Oct%d") : t.rangeFmt, int(d));
    case Kind::Touch: return d == 0 ? offText(t) : std::to_string(d);
    case Kind::Resolution: {
        const std::string n = d >= 0 && d < 12 ? edpages::trimmed(t.resolution[size_t(d)]) : std::string();
        return n.empty() ? std::to_string(d) : n;
    }
    case Kind::Tempo: return edtables::formatNumber(t.tempoFmt.empty() ? std::string("t%3d") : t.tempoFmt, int(d));
    case Kind::Feel: return d < 0 ? offText(t) : std::to_string(d);
    case Kind::RuntimeFlag: return d ? onText(t) : offText(t);
    case Kind::PresetAction:
        if (d >= 0) return arpPresetLabel(t, int(d));
        { const std::string s = edpages::trimmed(t.ownSettings); return s.empty() ? "SND" : s; }
    default: return std::to_string(d);
    }
}

std::string arpPresetLabel(const edtables::ArpText& t, int n) {
    return edtables::formatNumber(t.arpPresetFmt.empty() ? std::string("AP%02d") : t.arpPresetFmt, n);
}

}  // namespace edfields
