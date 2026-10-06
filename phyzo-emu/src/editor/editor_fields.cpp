#include "editor_fields.h"
#include <algorithm>
#include "editor_pages.h"

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
    auto mod = [&](const char* label, uint32_t off, int size, Kind kind) {
        Field f;
        f.label = label; f.section = "MOD"; f.offset = off; f.size = size; f.kind = kind;   // display only
        out.push_back(f);
    };
    mod("MOD SOURCE", 64, 1, Kind::Number);
    mod("MOD DEST", 65, 1, Kind::Number);
    mod("SRC MIN", 66, 2, Kind::Signed16);
    mod("SRC MAX", 68, 2, Kind::Signed16);
    mod("DEST MIN", 70, 2, Kind::Signed16);
    mod("DEST MAX", 72, 2, Kind::Signed16);
    return out;
}

std::vector<Field> arpFields(const edtables::ArpText& t) {
    std::vector<Field> out;
    auto add = [&](int page, const std::string& label, uint32_t off, int size, Kind kind, int lo, int hi, bool editable) -> Field& {
        Field f;
        f.label = label; f.area = 'a'; f.page = page; f.offset = off; f.size = size; f.kind = kind; f.lo = lo; f.hi = hi;
        f.editable = editable;
        out.push_back(f);
        return out.back();
    };
    add(0, "ARP", 383, 1, Kind::OnOff, 0, 1, true).onRaw = 2;
    add(0, code(t.hold, "HOLD"), 351, 1, Kind::OnOff, 0, 1, true);
    add(0, "KBD", 352, 1, Kind::Inverted, 0, 1, true);
    add(0, code(t.centre, "CENT"), 382, 1, Kind::OnOff, 0, 1, true);
    static const char* const kIntervals[5] = {"-3RD", "3RD", "4TH", "5TH", "OCT"};
    for (uint32_t i = 0; i < 5; ++i) add(0, code(t.intervals[i], kIntervals[i]), 361 + i, 1, Kind::Interval, 0, 1, true);
    add(0, "UP", 367, 1, Kind::OctaveUp, 0, 4, true);
    add(0, "DOWN", 368, 1, Kind::OctaveDown, 0, 4, true);
    add(0, code(t.low, "LO"), 370, 1, Kind::Note, 0, 127, true).mirror = 372;   // input filter and output bounds
    add(0, code(t.high, "HI"), 371, 1, Kind::Note, 0, 127, true).mirror = 373;
    add(0, code(t.touch, "TCH"), 384, 1, Kind::Touch, 0, 127, true);
    add(0, code(t.duration, "DUR"), 385, 1, Kind::Duration, 0, 99, true);
    add(0, code(t.limit, "LIM"), 386, 1, Kind::Number, 0, 99, true);
    add(1, code(t.resolutionCode, "RES"), 402, 4, Kind::Resolution, 0, 11, true);
    add(1, "TMPO", 406, 4, Kind::Tempo, 25, 320, true);
    add(1, code(t.feel, "FEEL"), 392, 2, Kind::Feel, 0, 0, false);
    add(1, code(t.presetCode, "PRST"), 0, 0, Kind::PresetAction, 0, kArpPresets - 1, false);
    add(1, "PATTERN", 390, 2, Kind::Pattern, 0, 0, false);
    add(1, "NAME", 334, 16, Kind::Text, 0, 0, false);
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
    case Kind::Feel: return d == kNone16 ? offText(t) : std::to_string(d);
    case Kind::PresetAction: { const std::string s = edpages::trimmed(t.ownSettings); return s.empty() ? "SND" : s; }
    default: return std::to_string(d);
    }
}

std::string arpPresetLabel(const edtables::ArpText& t, int n) {
    return edtables::formatNumber(t.arpPresetFmt.empty() ? std::string("AP%02d") : t.arpPresetFmt, n + 1);
}

}  // namespace edfields
