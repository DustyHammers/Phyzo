#include "editor_controller.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace editor {

using edpages::Item;
using edpages::Page;
using edpages::Row;
using edfields::Kind;

namespace {
const char* const kOscSections[] = {"WAVE", "PITCH", "FILTER", "AMP", "NOISE", "LFO", "ENV1", "ENV2", "ENV3", "GEN"};
const char* const kSoundSections[] = {"TRACK", "RANGE", "RECEIVE"};
const char* const kCommonSections[] = {"SLOTS", "KNOBS", "MOD"};
constexpr int kWaveField = 51;                       // the oscillator's wave (4-byte wave ID)

// The widest text a field's value can take (cells reserved for it on the page).
int valueWidth(const edtables::Field& f, const edtables::Table& t, size_t waveNameMax) {
    using namespace edtables;
    if (f.type == kLong) return int(2 + (waveNameMax ? 1 + waveNameMax : 0));
    if (f.display == Display::List && f.listIndex >= 0 && size_t(f.listIndex) < t.lists.size()) {
        size_t w = 0;
        for (const std::string& e : t.lists[size_t(f.listIndex)].entries) w = std::max(w, edpages::trimmed(e).size());
        w = std::max(w, std::to_string(f.max).size());   // values past the list's end show as numbers
        return int(w);
    }
    if (f.format == kFmtOffOn) return 3;
    if (f.format == kFmtNote) return 4;
    const bool sign = f.display == Display::Formatter || (f.format & kFmtSigned) || f.min < 0 || f.type == kOffsetByte;
    const int lo = f.type == kBitField ? 0 : f.min, hi = f.type == kBitField ? 255 : std::max<int>(f.max, 0);
    return int(std::max(std::to_string(lo).size(), std::to_string(hi + 1).size()) + (sign ? 1 : 0));
}

int presetValueWidth(const edfields::Field& f, const edtables::ArpText& t) {
    switch (f.kind) {
    case Kind::Text: case Kind::Pattern: return 16;
    case Kind::Feel: case Kind::Signed16: return 6;
    case Kind::PresetAction: return int(edfields::format(f, 0, t).size());
    default: break;
    }
    size_t w = 0;
    for (int d = f.lo; d <= f.hi && d - f.lo < 400; ++d) w = std::max(w, edfields::format(f, d, t).size());
    return int(w);
}

std::vector<Page> twoColumnPages(const std::vector<Item>& items, bool stages) {
    std::vector<Row> rows;
    if (stages) {
        std::vector<Item> rest;
        rows = edpages::stageRows(items, rest);
        const std::vector<Row> more = edpages::twoColumnRows(rest);
        rows.insert(rows.end(), more.begin(), more.end());
    } else {
        rows = edpages::twoColumnRows(items);
    }
    return rows.empty() ? std::vector<Page>() : edpages::paginate(rows);
}

std::string lowerStr(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool parseInt(const std::string& s, int64_t& v) {
    const std::string t = edpages::trimmed(s);
    if (t.empty()) return false;
    size_t i = 0;
    if (t[0] == '+' || t[0] == '-') i = 1;
    if (i >= t.size()) return false;
    for (size_t j = i; j < t.size(); ++j) if (!std::isdigit(static_cast<unsigned char>(t[j]))) return false;
    try { v = std::stoll(t); } catch (...) { return false; }
    return true;
}

// A note name (C4 = 60, sharps with #, flats with b) or a number.
bool parseNote(const std::string& s, int64_t& v) {
    if (parseInt(s, v)) return true;
    const std::string t = lowerStr(edpages::trimmed(s));
    static const int kBase[] = {9, 11, 0, 2, 4, 5, 7};   // a b c d e f g
    if (t.empty() || t[0] < 'a' || t[0] > 'g') return false;
    int n = kBase[t[0] - 'a'];
    size_t i = 1;
    if (i < t.size() && t[i] == '#') { ++n; ++i; } else if (i < t.size() && t[i] == 'b') { --n; ++i; }
    int64_t oct;
    if (!parseInt(t.substr(i), oct)) return false;
    v = (oct + 1) * 12 + n;
    return true;
}

uint8_t bitWidth(const edtables::Field& f) { return uint8_t((uint16_t(f.min) >> 8) & 0xFF); }
uint8_t bitPos(const edtables::Field& f) { return uint8_t(uint16_t(f.min) & 0xFF); }
bool bitFieldOk(const edtables::Field& f) {
    const int w = bitWidth(f), b = bitPos(f);
    return w >= 1 && w <= 8 && b + w <= 8;
}
}  // namespace

std::vector<std::string> parseWaveNames(const std::string& text) {
    std::vector<std::string> names(static_cast<size_t>(edtables::kWaves));
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        int n = 0;
        try { n = std::stoi(line.substr(0, tab)); } catch (...) { continue; }
        if (n >= 1 && n <= edtables::kWaves) names[size_t(n - 1)] = line.substr(tab + 1);
    }
    return names;
}

Controller::Controller(Link& link) : link_(link) {}

void Controller::setTables(const edtables::OsTables& tables) {
    tables_ = tables;
    objs_.clear();
    liveValid_ = flagsValid_ = arpPresetValid_ = false;
    presetNameRead_ = {};
    presetNamesRequested_ = 0;
    currentPreset_ = -1;
    patternNames_.clear();
    patternRequested_.clear();
    arpPresetNames_ = {};
    arpNamesRequested_ = 0;
    build();
    render();
}

void Controller::setWaveNames(const std::vector<std::string>& names) {
    waveNames_ = names;
    waves_.names = names;
    build();
    render();
}

void Controller::setFollowLeds(const std::array<int, 4>& sound, const std::array<int, 2>& osc, const std::array<int, 3>& env) {
    soundLeds_ = sound; oscLeds_ = osc; envLeds_ = env;
}

void Controller::setFollowExtraLeds(int noise, int lfo, int filterKbd) {
    noiseLed_ = noise; lfoLed_ = lfo; filterKbdLed_ = filterKbd;
}

void Controller::setKnobLabels(const std::array<std::string, 5>& labels) {
    knobLabels_ = labels;
    build();
    render();
}

void Controller::setOpen(bool open) {
    if (open == open_) return;
    open_ = open;
    link_.setOpen(open);
    if (open) {
        wavesRequested_ = false;
        lastLeds_ = link_.ledStateMessages();
        lastGestures_ = link_.panelGestures();
        lastTriggers_ = link_.presetTriggers();
        lastMoves_ = link_.panelControlMoves();
        lastDisplay_ = link_.display();
        followLeds();
        readDue_ = now_;                             // read the visible page on the next tick
    } else {
        outstanding_ = 0;
        readDue_ = displayChangedAt_ = readBackAt_ = -1;
        pending_.clear(); rmw_.clear(); pendingArp_.clear();   // the engine drops its queue too
        if (jobActive_) arpFinish(false, true);      // lets go of a button the editor holds; presses nothing more
        dragRef_ = -1;
        typing_ = false;
    }
    render();
}

// ------------------------------------------------------------------ structure

void Controller::build() {
    const std::string area = areas_.empty() ? std::string() : areas_[size_t(area_)].name;
    const std::string section = areas_.empty() || areas_[size_t(area_)].sections.empty() ? std::string()
                                : areas_[size_t(area_)].sections[size_t(section_)].name;
    const int page = page_;
    areas_.clear();
    refs_.clear();
    focus_ = -1;
    typing_ = false;
    dragRef_ = -1;
    pending_.clear(); rmw_.clear(); pendingArp_.clear(); before_.clear();
    if (jobActive_) arpFinish(false, true);
    size_t waveNameMax = 0;
    for (const auto& n : waveNames_) waveNameMax = std::max(waveNameMax, n.size());
    auto item = [&](const edtables::Table& t, int field, char kind) {
        const edtables::Field& f = t.fields[size_t(field)];
        refs_.push_back({&t, field, kind});
        // System codes are the OS's 4-character codes (trimmed); other names split into words.
        const std::string name = kind == 's' ? edpages::trimmed(f.name) : edpages::displayName(f.name);
        return Item{name, "", int(refs_.size()) - 1, valueWidth(f, t, waveNameMax)};
    };
    common_ = edfields::commonFields(knobLabels_);
    arp_ = tables_.ok && tables_.arp.ok ? edfields::arpFields(tables_.arp) : std::vector<edfields::Field>();
    auto pitem = [&](char kind, int index) {
        const edfields::Field& f = (kind == 'm' ? common_ : arp_)[size_t(index)];
        refs_.push_back({nullptr, index, kind});
        return Item{f.label, "", int(refs_.size()) - 1, presetValueWidth(f, tables_.arp)};
    };
    if (!tables_.ok) return;

    {   // SYSTEM: the OS's 4-character codes, four per row
        std::vector<Item> items;
        for (size_t i = 0; i < tables_.system.fields.size(); ++i)
            if (!tables_.system.fields[i].hidden) items.push_back(item(tables_.system, int(i), 's'));
        for (Item& x : items) x.width = std::min(x.width, 7);
        Area a{"SYSTEM", {{"", edpages::paginate(edpages::gridRows(items))}}};
        areas_.push_back(a);
    }
    {   // PRESETS: names, 8 per view
        std::vector<Item> items;
        for (int n = 0; n < kPresets; ++n) {
            refs_.push_back({nullptr, n, 'n'});
            items.push_back({(n < 9 ? "0" : "") + std::to_string(n + 1), "", int(refs_.size()) - 1, 16});
        }
        areas_.push_back({"PRESETS", {{"", edpages::paginate(edpages::nameRows(items))}}});
    }
    {   // COMMON: the sound slots (a table, one row per sound), the performance knobs, the insert modulation
        Area a{"COMMON", {}};
        for (const char* sec : kCommonSections) {
            std::vector<Row> rows;
            if (std::string(sec) == "SLOTS") {
                static const int kCols[] = {4, 16, 29, 38}, kWidths[] = {6, 8, 4, 4};   // MODE, OSC, LOW, HIGH
                for (int n = 0; n < 4; ++n) {
                    Row r;
                    r.kind = edpages::RowKind::Fixed;
                    r.items.push_back({"S" + std::to_string(n + 1), "", -1, 0, 0});
                    int k = 0;
                    for (size_t i = 0; i < common_.size(); ++i) {
                        if (common_[i].slot != n) continue;
                        Item x = pitem('m', int(i));
                        x.col = kCols[k];
                        x.width = kWidths[k];
                        r.items.push_back(x);
                        ++k;
                    }
                    rows.push_back(r);
                }
            } else {
                std::vector<Item> items;
                for (size_t i = 0; i < common_.size(); ++i) if (common_[i].section == sec) items.push_back(pitem('m', int(i)));
                rows = edpages::twoColumnRows(items);
            }
            a.sections.push_back({sec, edpages::paginate(rows)});
        }
        areas_.push_back(a);
    }
    {   // SOUND: the sound's settings in the preset and its common fields
        Area a{"SOUND", {}};
        for (const char* sec : kSoundSections) {
            std::vector<Item> items;
            for (size_t i = 0; i < tables_.soundSettings.fields.size(); ++i) {
                const auto& f = tables_.soundSettings.fields[i];
                if (!f.hidden && edpages::soundSection(f.name) == sec) items.push_back(item(tables_.soundSettings, int(i), 'p'));
            }
            for (size_t i = 0; i < tables_.soundCommon.fields.size(); ++i) {
                const auto& f = tables_.soundCommon.fields[i];
                if (!f.hidden && edpages::soundSection(f.name) == sec) items.push_back(item(tables_.soundCommon, int(i), 'c'));
            }
            std::vector<Page> pages = twoColumnPages(items, false);
            if (!pages.empty()) a.sections.push_back({sec, pages});
        }
        if (!a.sections.empty()) areas_.push_back(a);
    }
    {   // OSC: the oscillator's fields by section; envelopes as stage tables
        Area a{"OSC", {}};
        for (const char* sec : kOscSections) {
            std::vector<Item> items;
            for (size_t i = 0; i < tables_.oscillator.fields.size(); ++i) {
                const char* in = edpages::oscillatorSection(int(i));   // by field index; outside the known table: GEN
                if (!tables_.oscillator.fields[i].hidden && std::string(in ? in : "GEN") == sec)
                    items.push_back(item(tables_.oscillator, int(i), 'o'));
            }
            std::vector<Page> pages = twoColumnPages(items, std::string(sec).rfind("ENV", 0) == 0);
            if (!pages.empty()) a.sections.push_back({sec, pages});
        }
        if (!a.sections.empty()) areas_.push_back(a);
    }
    if (!arp_.empty()) {   // ARP: page 1 the OS's codes four per row; page 2 resolution, tempo, feel, arp preset, pattern, name
        std::vector<Item> first, second;
        Row pattern, name;
        pattern.kind = name.kind = edpages::RowKind::Fixed;
        for (size_t i = 0; i < arp_.size(); ++i) {
            Item x = pitem('a', int(i));
            if (arp_[i].kind == Kind::Pattern) { x.col = 0; pattern.items.push_back(x); }
            else if (arp_[i].kind == Kind::Text) { x.col = 0; name.items.push_back(x); }
            else (arp_[i].page == 0 ? first : second).push_back(x);
        }
        for (Item& x : first) x.width = std::min(x.width, 7);
        for (Item& x : second) x.width = std::min(x.width, 7);
        Page p1 = edpages::gridRows(first), p2 = edpages::gridRows(second);
        p2.push_back(pattern);
        p2.push_back(name);
        std::vector<Page> pages;
        for (size_t i = 0; i < p1.size(); i += size_t(edpages::kRows))
            pages.emplace_back(p1.begin() + long(i), p1.begin() + long(std::min(p1.size(), i + size_t(edpages::kRows))));
        pages.push_back(p2);
        areas_.push_back({"ARP", {{"", pages}}});
    }
    // Keep the position if the same area, section and page still exist.
    area_ = section_ = page_ = 0;
    for (size_t i = 0; i < areas_.size(); ++i)
        if (areas_[i].name == area) {
            area_ = int(i);
            for (size_t j = 0; j < areas_[i].sections.size(); ++j)
                if (areas_[i].sections[j].name == section) {
                    section_ = int(j);
                    if (page < int(areas_[i].sections[j].pages.size())) page_ = page;
                }
        }
}

Controller::Area* Controller::currentArea() { return areas_.empty() ? nullptr : &areas_[size_t(area_)]; }
const Controller::Area* Controller::currentArea() const { return areas_.empty() ? nullptr : &areas_[size_t(area_)]; }

Controller::ObjKey Controller::keyFor(char kind) const {
    switch (kind) {
    case 's': return {edproto::kSystem, 0, 0, 0};
    case 'p': return {edproto::kPerformance, edproto::kEditBuffer, 1, uint16_t(sound_)};
    case 'c': return {edproto::kSound, uint16_t(sound_), 0, 0};
    case 'o': return {edproto::kSound, uint16_t(sound_), 2, uint16_t(osc_)};
    case 'P': return {edproto::kPreset, 0, 0, 0};   // the preset edit buffer
    default: return {0xFF, 0, 0, 0};
    }
}

int Controller::refOf(char kind, int field) const {
    for (size_t i = 0; i < refs_.size(); ++i) if (refs_[i].kind == kind && refs_[i].field == field) return int(i);
    return -1;
}

// ------------------------------------------------------------------ reading

std::vector<std::vector<uint8_t>> Controller::pageReads() const {
    std::vector<std::vector<uint8_t>> reads;
    const Area* a = currentArea();
    if (!a || a->sections.empty()) return reads;
    const Section& s = a->sections[size_t(section_)];
    if (s.pages.empty()) return reads;
    std::map<char, std::pair<uint32_t, uint32_t>> ranges;   // kind -> [from, to)
    bool live = false, arpPreset = false, flags = false, names = false;
    auto range = [&](char kind, uint32_t from, uint32_t to) {
        auto it = ranges.find(kind);
        if (it == ranges.end()) ranges[kind] = {from, to};
        else it->second = {std::min(it->second.first, from), std::max(it->second.second, to)};
    };
    for (const Row& r : s.pages[size_t(page_)])
        for (const Item& x : r.items) {
            if (x.ref < 0) continue;
            const Ref& ref = refs_[size_t(x.ref)];
            if (ref.kind == 'n') {                   // a preset name: 16 characters at offset 8 of the stored preset
                reads.push_back(edproto::partialRead({edproto::kPreset, uint16_t(edproto::kStoredPreset + ref.field), 0, 0, 8, 16}));
                names = true;
                continue;
            }
            if (const edfields::Field* f = presetField(ref)) {
                if (f->kind == Kind::PresetAction) { arpPreset = true; continue; }
                if (f->kind == Kind::RuntimeFlag) { flags = true; continue; }
                if (f->live()) live = true;
                else range('P', f->offset, f->offset + uint32_t(f->size));
                continue;
            }
            const edtables::Field& f = ref.table->fields[size_t(ref.field)];
            range(ref.kind, f.offset, f.offset + uint32_t(f.size()));
        }
    for (const auto& kv : ranges) {
        const ObjKey k = keyFor(kv.first);
        reads.push_back(edproto::partialRead({k.type, k.number, k.subtype, k.subnumber, kv.second.first, kv.second.second - kv.second.first}));
    }
    if (names) reads.push_back(edproto::partialRead({edproto::kPreset, 0, 0, 0, 8, 16}));   // the current preset's name
    if (live) reads.push_back(edproto::partialRead({edproto::kRawMemory, 0, 0, 0, edfields::kLiveArp, edfields::kArpBlockSize}));
    if (flags) reads.push_back(edproto::partialRead({edproto::kRawMemory, 0, 0, 0, edfields::kArpFlags, 2}));
    // The OS's current arp preset on every ARP page (4 bytes), first: known before page 2 (PrSt) is opened.
    if (arpPreset || a->name == "ARP")
        reads.insert(reads.begin(), edproto::partialRead({edproto::kRawMemory, 0, 0, 0, edfields::kCurrentArpPreset, 4}));
    return reads;
}

void Controller::readPage() {
    if (!open_ || !link_.running()) return;
    std::vector<std::vector<uint8_t>> reads = pageReads();
    link_.setStandingRead(reads);
    if (!wavesRequested_ && currentArea() && currentArea()->name == "OSC") {
        reads.push_back(edproto::partialRead({edproto::kRawMemory, 0, 0, 0, edtables::kWaveList + 4, uint32_t(4 * edtables::kWaves)}));
        wavesRequested_ = true;
    }
    for (const auto& m : reads)
        if (link_.send(m)) { ++outstanding_; lastSendAt_ = now_; }
    render();
}

void Controller::handleReply(const std::vector<uint8_t>& m) {
    if (outstanding_ > 0) --outstanding_;
    unanswered_ = false;
    edproto::Message r;
    if (!edproto::parse(m.data(), m.size(), r) || r.command != edproto::kPartialReply) return;
    const edproto::Address& a = r.address;
    if (a.type == edproto::kRawMemory && a.offset == edtables::kWaveList + 4) {
        waves_.ids.assign(size_t(edtables::kWaves), 0);
        for (size_t i = 0; i < size_t(edtables::kWaves) && 4 * i + 3 < r.data.size(); ++i)
            waves_.ids[i] = uint32_t(r.data[4 * i]) << 24 | uint32_t(r.data[4 * i + 1]) << 16 | uint32_t(r.data[4 * i + 2]) << 8 | r.data[4 * i + 3];
        return;
    }
    if (a.type == edproto::kRawMemory && a.offset == edfields::kLiveArp && r.data.size() >= edfields::kArpBlockSize) {
        std::copy(r.data.begin(), r.data.begin() + edfields::kArpBlockSize, live_.begin());
        liveValid_ = liveFresh_ = true;
        return;
    }
    if (a.type == edproto::kRawMemory && a.offset == edfields::kArpControlVar && !r.data.empty()) {
        arpControl_ = r.data[0];
        arpControlFresh_ = true;
        return;
    }
    if (a.type == edproto::kRawMemory && a.offset == edfields::kPanelMode && !r.data.empty()) {
        panelMode_ = r.data[0];
        panelModeFresh_ = true;
        return;
    }
    if (a.type == edproto::kRawMemory && a.offset == edfields::kCurrentArpPreset && r.data.size() >= 4) {
        currentArpPreset_ = int32_t(uint32_t(r.data[0]) << 24 | uint32_t(r.data[1]) << 16 | uint32_t(r.data[2]) << 8 | r.data[3]);
        if (currentArpPreset_ < 0) currentArpPreset_ = -1;
        arpPresetValid_ = arpPresetFresh_ = true;
        return;
    }
    if (a.type == edproto::kRawMemory && a.offset == edfields::kArpFlags && r.data.size() >= 2) {
        arpFlags_ = {r.data[0], r.data[1]};
        flagsValid_ = flagsFresh_ = true;
        return;
    }
    auto text = [&]() {
        std::string name(r.data.begin(), r.data.end());
        for (char& c : name) if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = ' ';
        return name;
    };
    if (a.type == edproto::kArpPreset && a.number < edfields::kArpPresets) {
        if (a.offset == 8) arpPresetNames_[a.number] = text();
        return;
    }
    if (a.type == edproto::kArpPattern && a.offset == 8) { patternNames_[a.number] = text(); return; }
    if (a.type == edproto::kPreset && a.number >= edproto::kStoredPreset && a.offset == 8) {
        const int n = a.number - edproto::kStoredPreset;
        if (n >= 0 && n < kPresets) { presetNames_[size_t(n)] = text(); presetNameRead_[size_t(n)] = true; }
        updateCurrentPreset();
        return;
    }
    const ObjKey key{a.type, a.number, a.subtype, a.subnumber};
    Obj& o = objs_[key];
    const size_t end = size_t(a.offset) + r.data.size();
    if (end > 1 << 16) return;
    if (o.bytes.size() < end) { o.bytes.resize(end, 0); o.valid.resize(end, false); }
    std::copy(r.data.begin(), r.data.end(), o.bytes.begin() + long(a.offset));
    std::fill(o.valid.begin() + long(a.offset), o.valid.begin() + long(end), true);
    // Bit fields written by SysEx: the fresh byte, the field's bits changed, written back.
    std::map<uint32_t, uint8_t> composed;
    for (auto it = rmw_.begin(); it != rmw_.end();) {
        if (!(it->key == key) || it->offset < a.offset || it->offset >= end) { ++it; continue; }
        uint8_t& b = composed.count(it->offset) ? composed[it->offset] : (composed[it->offset] = o.bytes[it->offset]);
        const int mask = (1 << it->width) - 1;
        b = uint8_t((b & ~(mask << it->bit)) | ((it->value & mask) << it->bit));
        it = rmw_.erase(it);
    }
    for (const auto& kv : composed) {
        o.bytes[kv.first] = kv.second;
        if (link_.send(edproto::partialWrite({key.type, key.number, key.subtype, key.subnumber, kv.first, 1}, {kv.second}))) {
            lastSendAt_ = now_;
            afterWrite();
        }
    }
    // Writes not sent yet (and the value being dragged) stay shown.
    for (const auto& kv : pending_) apply(kv.first, kv.second);
    if (dragRef_ >= 0) apply(dragRef_, dragValue_);
    if (key == keyFor('P') && a.offset <= 8 && end >= 24) updateCurrentPreset();   // the edit buffer's name
}

// The current preset: the stored preset whose name the edit buffer holds (the last one picked here if several match).
void Controller::updateCurrentPreset() {
    const Obj* o;
    if (!bytesOf('P', o) || o->bytes.size() < 24 || !o->valid[8] || !o->valid[23]) return;
    std::string name(o->bytes.begin() + 8, o->bytes.begin() + 24);
    for (char& c : name) if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = ' ';
    name = edpages::trimmed(name);
    int found = -1;
    for (int n = 0; n < kPresets && !name.empty(); ++n) {
        if (!presetNameRead_[size_t(n)] || edpages::trimmed(presetNames_[size_t(n)]) != name) continue;
        if (found < 0 || n == lastPicked_ || (n == currentPreset_ && found != lastPicked_)) found = n;
    }
    if (found < 0 || found == currentPreset_) return;
    currentPreset_ = found;
    if (const Area* a = currentArea(); a && a->name == "PRESETS" && page_ != found / kPresetsPerPage) {
        page_ = found / kPresetsPerPage;             // the page follows the current preset
        readPage();
    }
}

// ------------------------------------------------------------------ values

const edfields::Field* Controller::presetField(const Ref& r) const {
    if (r.kind == 'm' && r.field >= 0 && size_t(r.field) < common_.size()) return &common_[size_t(r.field)];
    if (r.kind == 'a' && r.field >= 0 && size_t(r.field) < arp_.size()) return &arp_[size_t(r.field)];
    return nullptr;
}

bool Controller::bytesOf(char kind, const Obj*& o) const {
    auto it = objs_.find(keyFor(kind));
    if (it == objs_.end()) return false;
    o = &it->second;
    return true;
}

bool Controller::valueOf(int ref, int64_t& d) const {
    if (ref < 0 || size_t(ref) >= refs_.size()) return false;
    const Ref& r = refs_[size_t(ref)];
    if (r.kind == 'n') return false;
    if (const edfields::Field* f = presetField(r)) {
        if (f->kind == Kind::PresetAction) { if (!arpPresetValid_) return false; d = currentArpPreset_; return true; }
        if (f->kind == Kind::RuntimeFlag) {
            if (!flagsValid_ || f->flag < 0 || f->flag > 1) return false;
            d = arpFlags_[size_t(f->flag)] ? 1 : 0;
            return true;
        }
        if (f->kind == Kind::Text || f->kind == Kind::Pattern) return false;
        int64_t raw;
        if (f->live()) {
            if (!liveValid_ || !edfields::rawOf(*f, live_.data(), live_.size(), edfields::kArpBlock, raw)) return false;
        } else {
            const Obj* o;
            if (!bytesOf('P', o) || f->offset + size_t(f->size) > o->bytes.size()) return false;
            for (int i = 0; i < f->size; ++i) if (!o->valid[f->offset + size_t(i)]) return false;
            if (!edfields::rawOf(*f, o->bytes.data(), o->bytes.size(), 0, raw)) return false;
        }
        d = edfields::toDomain(*f, raw);
        return true;
    }
    if (!r.table) return false;
    const edtables::Field& f = r.table->fields[size_t(r.field)];
    const Obj* o;
    if (!bytesOf(r.kind, o) || f.offset + size_t(f.size()) > o->bytes.size()) return false;
    if (!std::all_of(o->valid.begin() + long(f.offset), o->valid.begin() + long(f.offset + size_t(f.size())), [](bool b) { return b; }))
        return false;
    int64_t raw;
    if (!edpages::rawValue(f, o->bytes.data(), o->bytes.size(), 0, raw)) return false;
    if (f.type == edtables::kLong) { d = waves_.numberOf(uint32_t(raw)); return d > 0; }
    d = raw;
    return true;
}

bool Controller::rangeOf(int ref, int64_t& lo, int64_t& hi) const {
    if (ref < 0 || size_t(ref) >= refs_.size()) return false;
    const Ref& r = refs_[size_t(ref)];
    if (const edfields::Field* f = presetField(r)) { lo = f->lo; hi = f->hi; return true; }
    if (!r.table) return false;
    using namespace edtables;
    const Field& f = r.table->fields[size_t(r.field)];
    if (f.type == kLong) {
        if (waves_.ids.empty()) return false;
        lo = 1; hi = kWaves;
        return true;
    }
    if (f.type == kBitField) {
        if (!bitFieldOk(f)) return false;
        lo = 0; hi = (1 << bitWidth(f)) - 1;
        return true;
    }
    if (f.type == kOffsetByte) {                     // shown = stored - 64; the record's range may be either form
        lo = f.min < 0 ? f.min : f.min - 64;
        hi = f.min < 0 ? f.max : f.max - 64;
        lo = std::max<int64_t>(lo, -64); hi = std::min<int64_t>(hi, 63);
        return lo <= hi;
    }
    if (f.max < f.min) return false;
    lo = f.min; hi = f.max;
    if (f.display == Display::List && f.listIndex >= 0 && size_t(f.listIndex) < r.table->lists.size()) {
        const ValueList& l = r.table->lists[size_t(f.listIndex)];
        const int first = l.hasFirst ? l.first : (f.type == kSignedListMinusOne ? -1 : std::min<int>(0, f.min));
        const int64_t last = first + int64_t(l.entries.size()) - 1;
        if (std::max<int64_t>(lo, first) <= std::min(hi, last)) { lo = std::max<int64_t>(lo, first); hi = std::min(hi, last); }
    }
    return true;
}

bool Controller::editable(int ref) const {
    if (ref < 0 || size_t(ref) >= refs_.size()) return false;
    const Ref& r = refs_[size_t(ref)];
    if (const edfields::Field* f = presetField(r)) {
        if (f->kind == Kind::PresetAction && !arpPresetValid_) return false;   // clickable once the OS's value is read
        return f->editable && (r.kind == 'm' || f->arpControl >= 0 || f->button >= 0);   // ARP: through the panel's buttons
    }
    if (!r.table) return false;
    using namespace edtables;
    const Field& f = r.table->fields[size_t(r.field)];
    int64_t lo, hi;
    if (!rangeOf(ref, lo, hi)) return false;
    switch (r.kind) {
    case 'o':                                        // NRPN for single-byte types 0, 1, 3; the wave by SysEx
        if (f.type == kLong) return r.field == kWaveField;
        return f.type == kUnsignedByte || f.type == kSignedByte || f.type == kBitField;
    case 'p': case 's':                              // SysEx partial write of the byte
        return f.type != kLong;
    default:                                         // sound common: display only this round
        return false;
    }
}

std::string Controller::textOf(int ref, int64_t d) const {
    const Ref& r = refs_[size_t(ref)];
    if (const edfields::Field* f = presetField(r)) return edfields::format(*f, d, tables_.arp);
    const edtables::Field& f = r.table->fields[size_t(r.field)];
    if (f.type == edtables::kLong) {
        if (d < 1 || size_t(d) > waves_.ids.size()) return std::to_string(d);
        return edpages::formatValue(f, *r.table, waves_.ids[size_t(d - 1)], &waves_);
    }
    return edpages::formatValue(f, *r.table, d, &waves_);
}

std::vector<Choice> Controller::listOf(int ref) const {
    std::vector<Choice> out;
    if (ref < 0 || size_t(ref) >= refs_.size()) return out;
    const Ref& r = refs_[size_t(ref)];
    int64_t cur = 0;
    const bool known = valueOf(ref, cur);
    if (const edfields::Field* f = presetField(r)) {
        if (f->kind == Kind::PresetAction) {         // the preset's own settings, then the arp presets by number and name
            if (!arpPresetValid_) return out;        // not read yet
            out.push_back({edfields::format(*f, -1, tables_.arp), -1, known && cur == -1});
            for (int n = 0; n < edfields::kArpPresets; ++n) {
                const std::string name = edpages::trimmed(arpPresetNames_[size_t(n)]);
                out.push_back({edfields::arpPresetLabel(tables_.arp, n) + (name.empty() ? "" : " " + name), n, known && cur == n});
            }
            return out;
        }
        switch (f->kind) {
        case Kind::List: case Kind::Note: case Kind::OnOff: case Kind::Interval: case Kind::Inverted: case Kind::RuntimeFlag:
        case Kind::OctaveUp: case Kind::OctaveDown: case Kind::Resolution:
            if (!f->editable) return out;
            for (int d = f->lo; d <= f->hi; ++d) out.push_back({edfields::format(*f, d, tables_.arp), d, known && cur == d});
            return out;
        default: return out;
        }
    }
    if (!r.table || !editable(ref)) return out;
    using namespace edtables;
    const Field& f = r.table->fields[size_t(r.field)];
    const bool list = f.type == kLong || f.display == Display::List || f.format == kFmtOffOn || f.format == kFmtNote ||
                      (f.type == kBitField && bitWidth(f) == 1);
    int64_t lo, hi;
    if (!list || !rangeOf(ref, lo, hi) || hi - lo > 255) return out;
    for (int64_t d = lo; d <= hi; ++d) out.push_back({textOf(ref, d), d, known && cur == d});
    return out;
}

Menu Controller::menu(int ref) const {
    Menu m;
    m.ref = ref;
    if (ref < 0 || size_t(ref) >= refs_.size()) return m;
    std::vector<Choice> all = listOf(ref);
    const edfields::Field* pf = presetField(refs_[size_t(ref)]);
    const bool notes = (pf && pf->kind == Kind::Note) ||
                       (refs_[size_t(ref)].table && refs_[size_t(ref)].table->fields[size_t(refs_[size_t(ref)].field)].format == edtables::kFmtNote);
    const size_t chunk = notes ? 12 : pf && pf->kind == Kind::PresetAction ? 20 : 10;
    if (all.size() <= 24) {
        m.items = all;
    } else {                                         // long lists in groups (notes by octave)
        size_t i = 0;
        if (notes) while (i < all.size() && all[i].value % 12 != 0) ++i;
        if (i > 0) m.groups.push_back({all.front().label + " ... " + all[i - 1].label, {all.begin(), all.begin() + long(i)}});
        for (; i < all.size(); i += chunk) {
            const size_t e = std::min(all.size(), i + chunk);
            m.groups.push_back({all[i].label + " ... " + all[e - 1].label, {all.begin() + long(i), all.begin() + long(e)}});
        }
    }
    int64_t cur;
    auto it = before_.find(ref);
    if (editable(ref) && it != before_.end() && valueOf(ref, cur) && cur != it->second) {
        m.revert = true;
        m.revertLabel = textOf(ref, it->second);
    }
    return m;
}

bool Controller::parseTyped(int ref, const std::string& s, int64_t& d) const {
    const Ref& r = refs_[size_t(ref)];
    const edfields::Field* pf = presetField(r);
    const bool note = (pf && pf->kind == Kind::Note) || (r.table && r.table->fields[size_t(r.field)].format == edtables::kFmtNote);
    if (note) return parseNote(s, d);
    // A value list: the entry's text (or the start of it), else a number.
    const std::string want = lowerStr(edpages::trimmed(s));
    const std::vector<Choice> list = listOf(ref);
    for (const Choice& c : list) if (lowerStr(edpages::trimmed(c.label)) == want) { d = c.value; return true; }
    for (const Choice& c : list)
        if (!want.empty() && lowerStr(edpages::trimmed(c.label)).rfind(want, 0) == 0) { d = c.value; return true; }
    if (pf && pf->kind == Kind::Tempo) {             // the tempo may be typed with the format's letters
        std::string digits;
        for (char c : s) if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
        return parseInt(digits, d);
    }
    return parseInt(s, d);
}

// ------------------------------------------------------------------ writing

void Controller::write(int ref, int64_t d) {
    if (!open_ || !link_.running() || !editable(ref)) return;
    if (!notice_.empty()) setNotice("");             // a new edit: an earlier message no longer applies
    int64_t lo, hi;
    if (!rangeOf(ref, lo, hi)) return;
    d = std::clamp(d, lo, hi);
    int64_t cur;
    if (!before_.count(ref) && valueOf(ref, cur)) before_[ref] = cur;
    apply(ref, d);
    if (refs_[size_t(ref)].kind == 'a') {            // through the panel's buttons, one edit at a time
        if (jobActive_ && job_.ref == ref && job_.stage < ArpJob::Stage::Leave) job_.target = d;
        else pendingArp_[ref] = d;
    } else {
        pending_[ref] = d;
    }
    edited_ = true;
    lastWriteAt_ = now_;
    pump();
    render();
}

void Controller::apply(int ref, int64_t d) {
    const Ref& r = refs_[size_t(ref)];
    if (const edfields::Field* f = presetField(r)) {
        const int64_t raw = edfields::toRaw(*f, d);
        if (r.kind == 'a') {
            return;                                  // ARP: shown as the target until the OS holds it (see render)
        } else {
            Obj& o = objs_[keyFor('P')];
            const size_t end = f->offset + size_t(f->size);
            if (o.bytes.size() < end) { o.bytes.resize(end, 0); o.valid.resize(end, false); }
            if (!std::all_of(o.valid.begin() + long(f->offset), o.valid.begin() + long(end), [](bool b) { return b; })) return;
            edfields::store(*f, raw, o.bytes.data(), o.bytes.size(), 0);
        }
        return;
    }
    if (!r.table) return;
    using namespace edtables;
    const Field& f = r.table->fields[size_t(r.field)];
    auto it = objs_.find(keyFor(r.kind));
    if (it == objs_.end() || f.offset + size_t(f.size()) > it->second.bytes.size()) return;
    uint8_t* p = it->second.bytes.data() + f.offset;
    if (f.type == kLong) {
        if (d < 1 || size_t(d) > waves_.ids.size()) return;
        const uint32_t id = waves_.ids[size_t(d - 1)];
        p[0] = uint8_t(id >> 24); p[1] = uint8_t(id >> 16); p[2] = uint8_t(id >> 8); p[3] = uint8_t(id);
    } else if (f.type == kBitField) {
        if (!bitFieldOk(f)) return;
        const int mask = (1 << bitWidth(f)) - 1;
        p[0] = uint8_t((p[0] & ~(mask << bitPos(f))) | ((int(d) & mask) << bitPos(f)));
    } else if (f.type == kOffsetByte) {
        p[0] = uint8_t(d + 64);
    } else {
        p[0] = uint8_t(d);
    }
}

// The next write, paced: at most one per kPaceMs, or the time the last one takes on the cable if longer.
void Controller::pump() {
    if (!open_ || !link_.running()) return;
    while (now_ >= nextSendAt_ && !pending_.empty()) {
        auto it = pending_.find(dragRef_);           // the value being dragged first
        if (it == pending_.end()) it = pending_.begin();
        const int ref = it->first;
        const int64_t d = it->second;
        pending_.erase(it);
        sendWrite(ref, d);
    }
}

void Controller::sendWrite(int ref, int64_t d) {
    const Ref& r = refs_[size_t(ref)];
    std::vector<std::vector<uint8_t>> msgs;
    bool isRead = false;
    if (const edfields::Field* f = presetField(r)) {
        if (r.kind == 'm' && f->nrpnLsb >= 0) msgs = edproto::nrpn(channel_, 1, f->nrpnLsb, int(d));
    } else if (r.table) {
        using namespace edtables;
        const Field& f = r.table->fields[size_t(r.field)];
        const ObjKey k = keyFor(r.kind);
        if (r.kind == 'o' && f.type == kLong) {
            if (d < 1 || size_t(d) > waves_.ids.size()) return;
            const uint32_t id = waves_.ids[size_t(d - 1)];
            msgs.push_back(edproto::partialWrite({k.type, k.number, k.subtype, k.subnumber, f.offset, 4},
                                                 {uint8_t(id >> 24), uint8_t(id >> 16), uint8_t(id >> 8), uint8_t(id)}));
        } else if (r.kind == 'o') {
            int msb, lsb;
            edproto::oscillatorNrpn(sound_, osc_, r.field, msb, lsb);
            msgs = edproto::nrpn(channel_, msb, lsb, int(d));
        } else if (f.type == kBitField) {           // read the byte; the write follows its reply
            rmw_.push_back({k, f.offset, bitPos(f), bitWidth(f), int(d)});
            msgs.push_back(edproto::partialRead({k.type, k.number, k.subtype, k.subnumber, f.offset, 1}));
            isRead = true;
        } else {
            const uint8_t raw = f.type == kOffsetByte ? uint8_t(d + 64) : uint8_t(d);
            msgs.push_back(edproto::partialWrite({k.type, k.number, k.subtype, k.subnumber, f.offset, 1}, {raw}));
        }
    }
    size_t bytes = 0;
    for (const auto& m : msgs) {
        if (!link_.send(m)) { pending_.emplace(ref, d); nextSendAt_ = now_ + kPaceMs; return; }   // full: next time
        bytes += m.size();
    }
    if (isRead) ++outstanding_;
    lastSendAt_ = now_;
    nextSendAt_ = now_ + std::max(kPaceMs, double(bytes) * kCableMsPerByte);
    afterWrite();
}

void Controller::afterWrite() {
    edited_ = true;
    lastWriteAt_ = now_;
    readBackAt_ = now_ + kReadBackMs;
}

void Controller::presetChanged() {
    before_.clear();
    edited_ = false;
}

// ------------------------------------------------------------------ rendering

void Controller::render() {
    View v;
    {
        std::lock_guard<std::mutex> lk(viewMtx_);
        v.serial = view_.serial;
    }
    v.follow = follow_;
    v.sound = sound_; v.osc = osc_; v.oscBoth = oscBoth_;
    v.data = open_ && outstanding_ > 0;
    v.edited = edited_;
    v.typing = typing_;
    for (const Area& a : areas_) v.areas.insert(a.name);
    if (const Area* a = currentArea()) {
        v.area = a->name;
        v.soundTargets = a->name == "SOUND" || a->name == "OSC";
        v.oscTargets = a->name == "OSC";
        for (const Section& s : a->sections) if (!s.name.empty()) v.sections.insert(s.name);
        if (!a->sections.empty()) {
            const Section& s = a->sections[size_t(section_)];
            v.section = s.name;
            v.page = page_ + 1;
            v.pages = int(s.pages.size());
            v.prev = page_ > 0;                      // pages within the section; sections change by their legends
            v.next = page_ + 1 < int(s.pages.size());
            if (!s.pages.empty()) {
                Page page = s.pages[size_t(page_)];
                for (Row& r : page)
                    for (Item& x : r.items) {
                        if (x.ref < 0) continue;
                        const Ref& ref = refs_[size_t(x.ref)];
                        x.value.clear();             // not read yet
                        if (ref.kind == 'n') { x.value = presetNames_[size_t(ref.field)]; continue; }
                        if (const edfields::Field* f = presetField(ref)) {
                            int64_t raw;
                            if (f->kind == Kind::Text) {
                                const Obj* o;
                                if (bytesOf('P', o) && f->offset + size_t(f->size) <= o->bytes.size() &&
                                    o->valid[f->offset] && o->valid[f->offset + size_t(f->size) - 1]) {
                                    x.value.assign(o->bytes.begin() + long(f->offset), o->bytes.begin() + long(f->offset + size_t(f->size)));
                                    for (char& c : x.value) if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = ' ';
                                    x.value = edpages::trimmed(x.value);
                                }
                            } else if (f->kind == Kind::Pattern) {
                                if (liveValid_ && edfields::rawOf(*f, live_.data(), live_.size(), edfields::kArpBlock, raw)) {
                                    auto it = patternNames_.find(int(raw));
                                    x.value = raw == edfields::kNone16 ? std::string("NONE")
                                              : it != patternNames_.end() ? edpages::trimmed(it->second) : std::to_string(raw);
                                }
                            } else {
                                int64_t d;
                                bool known = valueOf(x.ref, d);
                                // An arp edit on its way through the panel's buttons: its target until the OS holds it.
                                if (jobActive_ && job_.ref == x.ref) { d = job_.target; known = true; }
                                else if (auto it = pendingArp_.find(x.ref); it != pendingArp_.end()) { d = it->second; known = true; }
                                if (known) x.value = edfields::format(*f, d, tables_.arp);
                                else if (f->kind == Kind::PresetAction) x.value = "...";   // shown at once; read soon
                            }
                        } else {
                            const edtables::Field& tf = ref.table->fields[size_t(ref.field)];
                            const Obj* o;
                            int64_t raw;
                            const bool known = bytesOf(ref.kind, o) && tf.offset + size_t(tf.size()) <= o->bytes.size() &&
                                               std::all_of(o->valid.begin() + long(tf.offset), o->valid.begin() + long(tf.offset + size_t(tf.size())), [](bool b) { return b; });
                            if (known && edpages::rawValue(tf, o->bytes.data(), o->bytes.size(), 0, raw))
                                x.value = edpages::formatValue(tf, *ref.table, raw, &waves_);
                        }
                        if (typing_ && x.ref == focus_) x.value = typed_ + "_";
                    }
                v.screen = edpages::render(page);
                // PRESETS: the current preset is the focused item (its name blinks, never underlined).
                const int focus = a->name == "PRESETS" ? (currentPreset_ >= 0 ? refOf('n', currentPreset_) : -1) : focus_;
                for (const auto& sp : v.screen.values)
                    if (sp.ref == focus && (!sp.pick || a->name == "PRESETS")) { v.focusRow = sp.row; v.focusCol = sp.col; v.focusLen = sp.len; break; }
            }
        }
    }
    std::lock_guard<std::mutex> lk(viewMtx_);
    auto sameSpans = [](const std::vector<edpages::Screen::Span>& a, const std::vector<edpages::Screen::Span>& b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (a[i].row != b[i].row || a[i].col != b[i].col || a[i].len != b[i].len || a[i].ref != b[i].ref || a[i].pick != b[i].pick)
                return false;
        return true;
    };
    const bool same = v.screen.chars == view_.screen.chars && v.screen.underline == view_.screen.underline &&
                      sameSpans(v.screen.values, view_.screen.values) && v.area == view_.area &&
                      v.section == view_.section && v.page == view_.page && v.pages == view_.pages && v.sound == view_.sound &&
                      v.osc == view_.osc && v.oscBoth == view_.oscBoth && v.prev == view_.prev && v.next == view_.next &&
                      v.follow == view_.follow && v.data == view_.data && v.edited == view_.edited &&
                      v.areas == view_.areas && v.sections == view_.sections && v.soundTargets == view_.soundTargets &&
                      v.oscTargets == view_.oscTargets && v.focusRow == view_.focusRow && v.focusCol == view_.focusCol &&
                      v.focusLen == view_.focusLen && v.typing == view_.typing;
    v.serial = same ? view_.serial : view_.serial + 1;
    view_ = std::move(v);
}

View Controller::view() const {
    std::lock_guard<std::mutex> lk(viewMtx_);
    return view_;
}

// ------------------------------------------------------------------ navigation and editing

void Controller::go(int area, int section, int page) {
    if (area != area_ && size_t(area) < areas_.size() && areas_[size_t(area)].name == "PRESETS" && currentPreset_ >= 0)
        page = currentPreset_ / kPresetsPerPage;    // the presets open on the current preset's view
    area_ = area; section_ = section; page_ = page;
    focus_ = -1;
    typing_ = false;
    dragRef_ = -1;
    render();
    readPage();
}

bool Controller::goToRef(int ref) {
    for (size_t ai = 0; ai < areas_.size(); ++ai)
        for (size_t si = 0; si < areas_[ai].sections.size(); ++si)
            for (size_t pi = 0; pi < areas_[ai].sections[si].pages.size(); ++pi)
                for (const Row& r : areas_[ai].sections[si].pages[pi])
                    for (const Item& x : r.items)
                        if (x.ref == ref) {
                            if (int(ai) != area_ || int(si) != section_ || int(pi) != page_) go(int(ai), int(si), int(pi));
                            focus_ = ref;
                            typing_ = false;
                            render();
                            return true;
                        }
    return false;
}

std::vector<edpages::Screen::Span> Controller::visibleSpans() const {
    std::vector<edpages::Screen::Span> out;
    {
        std::lock_guard<std::mutex> lk(viewMtx_);
        for (const auto& s : view_.screen.values) if (!s.pick) out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return std::tie(a.row, a.col) < std::tie(b.row, b.col); });
    return out;
}

void Controller::moveFocus(int dr, int dc) {
    const std::vector<edpages::Screen::Span> spans = visibleSpans();
    if (spans.empty()) return;
    size_t at = spans.size();
    for (size_t i = 0; i < spans.size(); ++i) if (spans[i].ref == focus_) at = i;
    if (at == spans.size()) { focus_ = spans.front().ref; render(); return; }
    if (dc != 0) {
        const long n = long(at) + dc;
        if (n >= 0 && n < long(spans.size())) focus_ = spans[size_t(n)].ref;
    } else {
        for (int row = spans[at].row + dr; row >= 0 && row < edpages::kRows; row += dr) {
            int best = -1, bestDist = 1 << 30;
            for (size_t i = 0; i < spans.size(); ++i)
                if (spans[i].row == row && std::abs(spans[i].col - spans[at].col) < bestDist) {
                    bestDist = std::abs(spans[i].col - spans[at].col);
                    best = int(i);
                }
            if (best >= 0) { focus_ = spans[size_t(best)].ref; break; }
        }
    }
    render();
}

void Controller::command(const std::string& action, const std::string& text) {
    Area* a = currentArea();
    const int ref = std::atoi(text.c_str());
    const bool validRef = !text.empty() && ref >= 0 && size_t(ref) < refs_.size();
    if (action == "area") {
        for (size_t i = 0; i < areas_.size(); ++i) if (areas_[i].name == text) go(int(i), 0, 0);
    } else if (action == "section" && a) {
        for (size_t j = 0; j < a->sections.size(); ++j) if (a->sections[j].name == text) go(area_, int(j), 0);
    } else if (action == "target" && text.size() == 2) {
        const int n = text[1] - '1';
        if (text[0] == 'S' && n >= 0 && n < 4) { sound_ = n; follow_ = false; objs_.clear(); render(); readPage(); }
        if (text[0] == 'O' && n >= 0 && n < 2) { osc_ = n; oscBoth_ = false; follow_ = false; render(); readPage(); }
    } else if (action == "page" && a && !a->sections.empty()) {
        const int n = std::atoi(text.c_str()) - 1;
        if (n >= 0 && n < int(a->sections[size_t(section_)].pages.size())) go(area_, section_, n);
    } else if (action == "page_prev" && a && !a->sections.empty()) {
        if (page_ > 0) go(area_, section_, page_ - 1);
    } else if (action == "page_next" && a && !a->sections.empty()) {
        if (page_ + 1 < int(a->sections[size_t(section_)].pages.size())) go(area_, section_, page_ + 1);
    } else if (action == "toggle_follow") {
        follow_ = !follow_;
        if (follow_) followLeds();
        render();
    } else if (action == "focus" && validRef) {
        focus_ = ref;
        typing_ = false;
        render();
    } else if (action == "pick" && validRef && refs_[size_t(ref)].kind == 'n') {
        if (open_ && link_.running() && link_.send(edproto::programChange(channel_, refs_[size_t(ref)].field))) {
            lastPicked_ = currentPreset_ = refs_[size_t(ref)].field;   // marked at once; the read after the load confirms
            render();
        }
    } else if (action == "drag_begin" && validRef) {
        focus_ = ref;
        typing_ = false;
        int64_t d;
        if (editable(ref) && valueOf(ref, d)) { dragRef_ = ref; dragStart_ = dragValue_ = d; }
        render();
    } else if (action == "drag" && dragRef_ >= 0) {
        int64_t lo, hi;
        if (!rangeOf(dragRef_, lo, hi)) return;
        const int64_t v = std::clamp(dragStart_ + int64_t(std::atoi(text.c_str())), lo, hi);
        if (v != dragValue_) { dragValue_ = v; write(dragRef_, v); }
    } else if (action == "drag_end") {
        dragRef_ = -1;
    } else if (action == "type" && validRef) {
        focus_ = ref;
        typing_ = editable(ref);
        typed_.clear();
        render();
    }
}

bool Controller::key(const std::string& k) {
    if (typing_) {
        if (k == "escape") typing_ = false;
        else if (k == "enter") {
            typing_ = false;
            int64_t d;
            if (focus_ >= 0 && parseTyped(focus_, typed_, d)) write(focus_, d);
        } else if (k == "backspace") { if (!typed_.empty()) typed_.pop_back(); }
        else if (k.size() == 1 && static_cast<unsigned char>(k[0]) >= 0x20 && static_cast<unsigned char>(k[0]) < 0x7F && typed_.size() < 12)
            typed_ += k;
        render();
        return true;                                 // typed entry takes every key
    }
    if (focus_ < 0) return false;
    if (k == "left") { moveFocus(0, -1); return true; }
    if (k == "right") { moveFocus(0, 1); return true; }
    if (k == "up") { moveFocus(-1, 0); return true; }
    if (k == "down") { moveFocus(1, 0); return true; }
    if (k == "plus" || k == "minus") {
        int64_t d;
        if (editable(focus_) && valueOf(focus_, d)) write(focus_, d + (k == "plus" ? 1 : -1));
        return true;
    }
    if (k == "enter") { command("type", std::to_string(focus_)); return true; }
    if (k == "escape") { focus_ = -1; render(); return false; }   // the panel's latched buttons are released too
    return false;
}

void Controller::choose(int ref, int64_t value) {
    if (ref < 0 || size_t(ref) >= refs_.size() || !open_ || !link_.running()) return;
    focus_ = ref;
    write(ref, value);
}

void Controller::revert(int ref) {
    auto it = before_.find(ref);
    if (it != before_.end()) write(ref, it->second);
}

// Follow: the panel's solidly lit LEDs pick the target (the sound being edited is solid, the other active sounds
// flash); the envelope LEDs the envelope section. Both OSC LEDs solid: both oscillators are edited (oscillator 1's
// values shown). No solid LED: the target stays.
void Controller::followLeds() {
    if (!follow_) return;
    const auto solid = [&](int code) { return code >= 0 && link_.ledMode(code) == 1; };
    int sound = sound_, osc = osc_;
    bool both = oscBoth_;
    for (int i = 0; i < 4; ++i) if (solid(soundLeds_[size_t(i)])) { sound = i; break; }
    const bool o1 = solid(oscLeds_[0]), o2 = solid(oscLeds_[1]);
    if (o1 || o2) { osc = o1 ? 0 : 1; both = o1 && o2; }
    bool changed = sound != sound_ || osc != osc_ || both != oscBoth_;
    if (sound != sound_) objs_.clear();
    sound_ = sound; osc_ = osc; oscBoth_ = both;
    if (Area* a = currentArea(); a && a->name == "OSC" && !a->sections.empty() && a->sections[size_t(section_)].name.rfind("ENV", 0) == 0) {
        for (int e = 0; e < 3; ++e)
            if (solid(envLeds_[size_t(e)])) {
                const std::string want = "ENV" + std::to_string(e + 1);
                for (size_t j = 0; j < a->sections.size(); ++j)
                    if (a->sections[j].name == want && int(j) != section_) { section_ = int(j); page_ = 0; changed = true; }
                break;
            }
    }
    if (changed) { render(); readPage(); }
}

// Follow on panel knobs (docs/editor/EDITOR_ROUND2.md section 4): the page holding the moved control's field, focused.
void Controller::followKnob(int cc) {
    const auto lit = [&](int code) { return code >= 0 && link_.ledMode(code) != 0; };
    int field = -1;
    switch (cc) {
    case 10: field = 51; break;
    case 7: field = 58; break;
    case 20: field = 14; break;
    case 18: field = 27; break;
    case 19: field = 18; break;
    case 22: field = 32; break;
    case 8: field = 33; break;
    case 4: field = lit(filterKbdLed_) ? 37 : 34; break;
    case 2: field = 12; break;
    case 1: field = 13; break;
    case 0: field = lit(lfoLed_) ? 128 : lit(noiseLed_) ? 70 : -1; break;
    case 17: case 16: case 12: case 21: {
        int env = -1;
        for (int e = 0; e < 3 && env < 0; ++e) if (envLeds_[size_t(e)] >= 0 && link_.ledMode(envLeds_[size_t(e)]) == 1) env = e;
        if (env < 0) for (int e = 0; e < 3 && env < 0; ++e) if (lit(envLeds_[size_t(e)])) env = e;
        if (env >= 0) field = (cc == 17 ? 79 : cc == 16 ? 85 : cc == 12 ? 86 : 87) + 16 * env;
        break;
    }
    case 9: case 13: case 6: case 5: case 3: {       // the performance knobs perf1-perf5: COMMON KNOBS
        const int k = cc == 9 ? 0 : cc == 13 ? 1 : cc == 6 ? 2 : cc == 5 ? 3 : 4;
        int n = 0;
        for (size_t i = 0; i < common_.size(); ++i)
            if (common_[i].section == "KNOBS" && n++ == k) {
                const int ref = refOf('m', int(i));
                if (ref >= 0) goToRef(ref);
            }
        return;
    }
    case 14:                                         // Arp Value: the ARP page
        for (size_t i = 0; i < areas_.size(); ++i) if (areas_[i].name == "ARP" && area_ != int(i)) go(int(i), 0, 0);
        return;
    default: return;                                 // volume, mix, wheels, pressure: no jump
    }
    if (field >= 0) goToRef(refOf('o', field));
}

// ------------------------------------------------------------------ arp edits through the panel's buttons

void Controller::press(int button, bool down) {
    link_.pressButton(button, down);
    if (jobActive_) ++job_.ownPresses;               // the editor's own edges in the panel's count
    heldButton_ = down ? button : -1;
    lastWriteAt_ = now_;                             // the LEDs this switches are the edit's own
}

void Controller::rawRead(uint32_t address, uint32_t length) {
    if (link_.send(edproto::partialRead({edproto::kRawMemory, 0, 0, 0, address, length}))) { ++outstanding_; lastSendAt_ = now_; }
}

void Controller::arpFinish(bool done, bool interrupted) {
    if (heldButton_ >= 0) link_.pressButton(heldButton_, false);   // never leave a button held
    heldButton_ = -1;
    if (done) afterWrite();
    if (interrupted) {                               // someone else at the panel or the MIDI input, or closing
        pendingArp_.clear();
        arpEnd();
        return;
    }
    if (pendingArp_.empty() && runMode_ == edfields::kModeNormal &&
        (job_.stage < ArpJob::Stage::Leave)) {       // the run began in normal mode: back there from the arp menu
        job_.stage = ArpJob::Stage::Leave;
        job_.at = now_;
        job_.asked = false;
        render();
        return;
    }
    if (pendingArp_.empty()) runMode_ = -1;          // (a run that began in the arp menu stays there)
    jobActive_ = false;
    render();
}

void Controller::arpEnd() {
    jobActive_ = false;
    runMode_ = -1;
    render();
}

// One arp edit at a time (docs/editor/ROUND2_REVIEW.md), as hands on the panel do it:
//  1. read the OS's panel mode (recorded: after the edit the panel stays in the arp menu; it is not returned);
//  2. the control's button: Tempo, Range, or Arp Mode until the arp-control variable reads the control, with the panel
//     mode reading the arp menu (0x0C); 1.4 s for the OS to show it;
//  3. -/No or +/Yes once per step, each press only right after the panel mode read 0x0C AND the arp-control variable
//     read the control; otherwise the edit stops without pressing (the variable keeps its value outside the arp
//     menu: in normal mode those buttons step the main preset);
//  4. the value read back each round until it lands.
// Any panel input or other MIDI during the edit stops it at once (and the edits queued behind it); none starts while a
// panel button is held (latched by the skin, or not released). On/off and keyboard:
// their own button once, if the OS's flag differs.
void Controller::arpPump() {
    if (!open_ || !link_.running()) return;
    if (jobActive_) {
        lastWriteAt_ = now_;                         // the arp LEDs the edit switches are not a preset change
        if (link_.panelGestures() != job_.gestures + uint64_t(job_.ownPresses) || link_.externalMidi() != job_.midi) {
            arpFinish(false, true);                  // someone else is at the panel or the MIDI input: stop at once
            return;
        }
    }
    if (!jobActive_) {
        if (pendingArp_.empty()) return;
        const auto it = pendingArp_.begin();
        const edfields::Field* f = presetField(refs_[size_t(it->first)]);
        job_ = ArpJob();
        job_.ref = it->first;
        job_.target = it->second;
        pendingArp_.erase(it);
        if (!f || (f->arpControl < 0 && f->button < 0)) return;
        if (f->arpControl < 0) {                     // a flag: nothing to do if the OS's flag is already so
            int64_t cur;
            if (valueOf(job_.ref, cur) && cur == job_.target) return;
        }
        if (link_.panelButtonHeld()) {               // a button latched or held on the panel: press nothing
            setNotice("The arp settings cannot be changed while a panel button is held (latched). Release it first. "
                      "Nothing was changed.");
            pendingArp_.clear();
            render();
            return;
        }
        job_.control = f->arpControl;
        job_.gestures = link_.panelGestures();
        job_.midi = link_.externalMidi();
        jobActive_ = true;
        job_.stage = ArpJob::Stage::ReadMode;
        job_.at = now_;
    }
    const edfields::Field* f = presetField(refs_[size_t(job_.ref)]);
    const double since = now_ - job_.at;
    auto next = [&](ArpJob::Stage st) { job_.stage = st; job_.at = now_; job_.asked = false; };
    const int selectButton = job_.control == 0 ? edfields::kBtnTempo : job_.control == 1 ? edfields::kBtnRange : edfields::kBtnArpMode;
    const int stepButton = job_.dir > 0 ? edfields::kBtnYes : edfields::kBtnNo;
    auto readModeAndControl = [&] {
        panelModeFresh_ = arpControlFresh_ = false;
        rawRead(edfields::kPanelMode, 1);
        rawRead(edfields::kArpControlVar, 1);
    };
    auto inControl = [&] { return panelMode_ == edfields::kModeArpMenu && arpControl_ == job_.control; };
    auto readValue = [&] {
        if (f->kind == Kind::PresetAction) { arpPresetFresh_ = false; rawRead(edfields::kCurrentArpPreset, 4); }
        else { liveFresh_ = false; rawRead(edfields::kLiveArp, edfields::kArpBlockSize); }
    };
    auto valueFresh = [&] { return f->kind == Kind::PresetAction ? arpPresetFresh_ : liveFresh_; };
    switch (job_.stage) {
    case ArpJob::Stage::ReadMode:                    // the panel mode before anything is pressed
        if (!job_.asked) { readModeAndControl(); job_.asked = true; break; }
        if (panelModeFresh_ && arpControlFresh_) {
            modeBeforeArpEdit_ = panelMode_;
            if (panelMode_ != edfields::kModeNormal && panelMode_ != edfields::kModeArpMenu) {
                // A sound, oscillator, MIDI or effects edit on the panel: no arp edit; nothing is pressed.
                setNotice("The arp settings can be changed only while the panel is in its normal mode "
                          "(not in a sound, oscillator, MIDI or effects edit). Nothing was changed.");
                pendingArp_.clear();
                arpEnd();
                break;
            }
            if (runMode_ < 0) runMode_ = panelMode_;
            if (job_.control < 0) { press(f->button, true); next(ArpJob::Stage::ToggleUp); }
            else next(ArpJob::Stage::Select);
        } else if (since > kReplyTimeoutMs) {        // no answer (the OS's demo answers no SysEx): press nothing
            setNotice("The panel is busy (e.g. demo); the arp setting was not changed.");
            arpFinish(false, true);
        }
        break;
    case ArpJob::Stage::ToggleUp:
        if (since >= 80) { press(f->button, false); next(ArpJob::Stage::ToggleRead); }
        break;
    case ArpJob::Stage::ToggleRead:
        if (!job_.asked && since >= 150) { flagsFresh_ = false; rawRead(edfields::kArpFlags, 2); job_.asked = true; }
        if (job_.asked && flagsFresh_) arpFinish(true);
        else if (since > kReplyTimeoutMs) arpFinish(false);
        break;
    case ArpJob::Stage::Select:
        press(selectButton, true);
        ++job_.selects;
        next(ArpJob::Stage::SelectUp);
        break;
    case ArpJob::Stage::SelectUp:
        if (since >= 80) { press(selectButton, false); next(ArpJob::Stage::ReadControl); }
        break;
    case ArpJob::Stage::ReadControl:                 // the arp menu, showing the control?
        if (!job_.asked && since >= 100) { readModeAndControl(); job_.asked = true; }
        if (job_.asked && panelModeFresh_ && arpControlFresh_) {
            if (inControl()) next(ArpJob::Stage::Settle);
            else if (panelMode_ == edfields::kModeArpMenu && job_.control >= 2 && job_.selects < 20) next(ArpJob::Stage::Select);
            else arpFinish(false);                   // not in the arp menu after the press: stop, press nothing more
        } else if (since > kReplyTimeoutMs) {
            arpFinish(false);
        }
        break;
    case ArpJob::Stage::Settle:
        if (since >= 1400) next(ArpJob::Stage::Check);
        break;
    case ArpJob::Stage::Check:                       // still in the control, and the value now
        if (!job_.asked) { readModeAndControl(); readValue(); job_.asked = true; break; }
        if (!(panelModeFresh_ && arpControlFresh_ && valueFresh())) { if (since > kReplyTimeoutMs) arpFinish(false); break; }
        if (!inControl()) { arpFinish(false); break; }   // the panel left the control: stop
        {
            int64_t cur;
            if (!valueOf(job_.ref, cur)) { arpFinish(false); break; }
            int64_t lo, hi;
            if (rangeOf(job_.ref, lo, hi)) job_.target = std::clamp(job_.target, lo, hi);
            if (cur == job_.target) { arpFinish(true); break; }
            if (job_.rounds > 0 && cur == job_.last) { arpFinish(true); break; }   // no further step: the end of its range
            if (++job_.rounds > 8) { arpFinish(true); break; }
            job_.last = cur;
            job_.dir = job_.target > cur ? 1 : -1;
            job_.presses = int(std::min<int64_t>(std::abs(job_.target - cur), 128));
            next(ArpJob::Stage::PreStep);
        }
        break;
    case ArpJob::Stage::PreStep:                     // before EVERY -/No or +/Yes: the arp menu and the control, read now
        if (!job_.asked) { readModeAndControl(); job_.asked = true; break; }
        if (!(panelModeFresh_ && arpControlFresh_)) { if (since > kReplyTimeoutMs) arpFinish(false); break; }
        if (!inControl()) { arpFinish(false); break; }   // stop without pressing
        next(ArpJob::Stage::Step);
        [[fallthrough]];
    case ArpJob::Stage::Step:
        press(stepButton, true);
        next(ArpJob::Stage::StepUp);
        break;
    case ArpJob::Stage::StepUp:
        if (since >= 20) {
            press(stepButton, false);
            --job_.presses;
            next(ArpJob::Stage::StepGap);
        }
        break;
    case ArpJob::Stage::StepGap:                     // the next press (checked again); after the last, settle and check
        if (job_.presses > 0 && since >= 20) next(ArpJob::Stage::PreStep);
        else if (job_.presses <= 0 && since >= 150) next(ArpJob::Stage::Check);
        break;
    case ArpJob::Stage::Leave:                       // the bank button once, only while the mode reads the arp menu
        if (!job_.asked) { panelModeFresh_ = false; rawRead(edfields::kPanelMode, 1); job_.asked = true; break; }
        if (!panelModeFresh_) { if (since > kReplyTimeoutMs) arpEnd(); break; }
        if (panelMode_ != edfields::kModeArpMenu) { arpEnd(); break; }   // not in the arp menu: press nothing
        press(edfields::kBtnBank, true);
        next(ArpJob::Stage::LeaveUp);
        break;
    case ArpJob::Stage::LeaveUp:
        if (since >= 80) { press(edfields::kBtnBank, false); next(ArpJob::Stage::LeaveCheck); }
        break;
    case ArpJob::Stage::LeaveCheck:                  // back in normal mode? (never a second press)
        if (!job_.asked && since >= 150) { panelModeFresh_ = false; rawRead(edfields::kPanelMode, 1); job_.asked = true; }
        if (job_.asked && panelModeFresh_) {
            if (panelMode_ != edfields::kModeNormal)
                setNotice("The panel did not return to its normal mode after the arp edit; please check it.");
            arpEnd();
        } else if (since > kReplyTimeoutMs) {
            arpEnd();
        }
        break;
    }
}

// ------------------------------------------------------------------ tick

void Controller::tick(double nowMs) {
    now_ = nowMs;
    std::vector<uint8_t> m;
    bool got = false;
    while (link_.popReply(m)) { handleReply(m); got = true; }
    if (!open_ || !link_.running()) { if (got) render(); return; }

    if (!wavesRequested_ && currentArea() && currentArea()->name == "OSC") readDue_ = readDue_ < 0 ? now_ : readDue_;
    const uint64_t leds = link_.ledStateMessages(), gestures = link_.panelGestures(), triggers = link_.presetTriggers();
    if (leds != lastLeds_) {                         // an LED-state burst (a preset load, a selection): debounced re-read
        lastLeds_ = leds;
        lastLedChangeAt_ = now_;
        readDue_ = now_ + kDebounceMs;
        displayChangedAt_ = -1;
        if (now_ - lastWriteAt_ > kOwnLedsMs) presetChanged();   // not the LEDs an edit of ours switched (arp on)
        followLeds();
    }
    if (gestures != lastGestures_) { lastGestures_ = gestures; readDue_ = now_ + kDebounceMs; }
    const uint64_t moves = link_.panelControlMoves();
    if (moves != lastMoves_) { lastMoves_ = moves; if (follow_) followKnob(link_.lastPanelControl()); }
    if (triggers != lastTriggers_) {                 // the engine queued the standing read behind it
        outstanding_ += int(pageReads().size()) * int(triggers - lastTriggers_);
        lastSendAt_ = now_;
        lastTriggers_ = triggers;
        presetChanged();
    }
    const std::array<char, 4> d = link_.display();
    if (d != lastDisplay_) { lastDisplay_ = d; displayChangedAt_ = now_; }
    // A new preset number with no LED burst after it (a preset whose LEDs equal the previous one's): read anyway.
    if (displayChangedAt_ >= 0 && now_ - displayChangedAt_ >= kDisplayFallbackMs) { displayChangedAt_ = -1; readDue_ = now_; }
    if (readDue_ >= 0 && now_ >= readDue_) { readDue_ = -1; readPage(); }

    // Edits: paced sends, the arp edit through the panel's buttons, and the read-back once everything is out.
    pump();
    arpPump();
    const bool quiet = pending_.empty() && pendingArp_.empty() && !jobActive_ && rmw_.empty();
    if (readBackAt_ >= 0 && now_ >= readBackAt_ && quiet && dragRef_ < 0) { readBackAt_ = -1; readPage(); }

    // Names for the ARP area: the pattern shown, and the arp presets for the right-click list (a few per tick, only
    // once the page's own reads are answered, so they never hold up a page).
    if (currentArea() && currentArea()->name == "ARP" && quiet) {
        for (const edfields::Field& f : arp_) {
            int64_t raw;
            if (f.kind != Kind::Pattern || !liveValid_ || !edfields::rawOf(f, live_.data(), live_.size(), edfields::kArpBlock, raw)) continue;
            if (raw != edfields::kNone16 && !patternRequested_.count(int(raw)) && link_.send(edproto::partialRead({edproto::kArpPattern, uint16_t(raw), 0, 0, 8, 16}))) {
                patternRequested_.insert(int(raw));
                ++outstanding_;
                lastSendAt_ = now_;
            }
        }
        // (not while the OS leaves requests unanswered, e.g. in its demo: a request is never repeated every tick)
        for (int i = 0; i < 2 && outstanding_ == 0 && !unanswered_ && arpNamesRequested_ < edfields::kArpPresets; ++i) {
            if (!link_.send(edproto::partialRead({edproto::kArpPreset, uint16_t(arpNamesRequested_), 0, 0, 8, 16}))) break;
            ++arpNamesRequested_;
            ++outstanding_;
            lastSendAt_ = now_;
        }
    }
    // PRESETS: every stored preset's name once (to find the current preset by its name), a few per tick.
    if (currentArea() && currentArea()->name == "PRESETS" && quiet && !unanswered_)
        for (int i = 0; i < 2 && presetNamesRequested_ < kPresets; ++i, ++presetNamesRequested_) {
            if (presetNameRead_[size_t(presetNamesRequested_)]) continue;
            if (!link_.send(edproto::partialRead({edproto::kPreset, uint16_t(edproto::kStoredPreset + presetNamesRequested_), 0, 0, 8, 16}))) break;
            ++outstanding_;
            lastSendAt_ = now_;
        }
    if (outstanding_ > 0 && now_ - lastSendAt_ > kReplyTimeoutMs) { outstanding_ = 0; unanswered_ = true; }
    render();
}

}  // namespace editor
