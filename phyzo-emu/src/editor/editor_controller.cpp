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
const char* const kCommonSections[] = {"SLOTS", "KNOBS"};
constexpr int kWaveField = 51;                       // the oscillator's wave (4-byte wave ID)
// Effects (docs/editor/EFFECTS_EDITOR_SPEC.md)
constexpr uint32_t kFxFrom = 60, kFxTo = 334;        // the preset's effect bytes read with the page
constexpr uint32_t kRunningFrom = 0x2CC, kRunningLength = 0x10E;   // the running performance's effect block (preset 60-329)
constexpr int kInsertMixSlot = 13, kReverbMixSlot = 27;
constexpr uint32_t kInsertMixPreset = 136, kInsertMixVariation = 292;
const int kInsertSends[] = {6, 7, 8, 9, 10, 0, 17, 3};           // routing-shell slots (docs/editor/ROUND3_ANSWERS.md 1)
const int kInsertAlgoRouting[] = {14, 11, 12, 15, 16};
const int kReverbSends[] = {25, 26, 28, 29, 30, 31, 2};
const uint32_t kModOrder[] = {0, 1, 2, 4, 6, 8};      // source, destination, source min / max, destination min / max
// Copying a variation to PSEt (write path 4b): preset bytes [from, to) of the running performance (or of the edit
// buffer) raw-written into the edit buffer at `into`, in this order.
struct CopyRange { uint32_t from, to; bool running; uint32_t into; };
const CopyRange kInsertCopy[] = {{174, 258, true, 174},   // the insert area
                                 {64, 74, true, 64},      // the modulation block
                                 {118, 120, true, 118},   // the two routing words a variation sets (slots 4 and 6)
                                 {122, 124, true, 122},
                                 {292, 294, false, 136}}; // the insert mix: the edit buffer's word 292 into word 136
const CopyRange kReverbCopy[] = {{294, 330, true, 294}};  // the reverb area
const char* const kCopyFailed = "The variation could not be copied to PSEt (the OS did not store the copy). It stays "
                                "selected; nothing more was sent.";

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
    if (f.format == kFmtThreeDigits && f.display == Display::Number && f.type != kBitField)
        return int(std::max<size_t>(3, std::to_string(std::max<int>(f.max, 0)).size()));
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

// A bit field's record min: mask << 8 | bit. Only one-bit flags (mask 1) are edited; any other mask is display only.
uint8_t bitMask(const edtables::Field& f) { return uint8_t((uint16_t(f.min) >> 8) & 0xFF); }
uint8_t bitWidth(const edtables::Field& f) { return bitMask(f) == 1 ? 1 : 0; }
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
        modPending_ = modReady_ = false;
        copy_ = {};                                  // nothing more is sent
        copyFailed_ = {};
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
    modPending_ = modReady_ = false;
    copy_ = {};
    copyFailed_ = {};
    fx_.clear();
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
    {   // COMMON: the sound slots (a table, one row per sound), the performance knobs
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
    if (tables_.fx.ok) {   // INSERT (ALGO, MOD, SENDS) and REVERB (ALGO, SENDS): docs/editor/EFFECTS_EDITOR_SPEC.md
        const edtables::FxTables& t = tables_.fx;
        auto add = [&](FxRef::K k, int n) {
            fx_.push_back({k, n});
            refs_.push_back({nullptr, int(fx_.size()) - 1, 'x'});
        };
        add(FxRef::K::Algo, 0); add(FxRef::K::InsertVar, 0); add(FxRef::K::ReverbVar, 0); add(FxRef::K::InsertMix, 0);
        for (int k = 0; k < int(t.routing.params.size()); ++k) add(FxRef::K::Routing, k);
        for (int k = 0; k < 32; ++k) add(FxRef::K::Insert, k);
        for (int k = 0; k < int(t.reverb.params.size()); ++k) add(FxRef::K::Reverb, k);
        for (int i = 0; i < int(t.mod.size()); ++i) add(FxRef::K::Mod, i);
        auto item = [&](FxRef::K k, int n, const std::string& label) {
            const int ref = fxRefOf(k, n);
            return Item{label, "", ref, ref >= 0 ? fxWidth(fx_[size_t(refs_[size_t(ref)].field)]) : 0};
        };
        auto routing = [&](int k) { return item(FxRef::K::Routing, k, edpages::displayName(t.routing.params[size_t(k)].name)); };
        auto pages = [](const std::vector<Item>& items) { return edpages::paginate(edpages::twoColumnRows(items)); };
        fxAlgo_ = runningAlgo();
        Area ins{"INSERT", {}};
        ins.sections.push_back({"ALGO", insertAlgoPages()});
        std::vector<Item> mod;
        for (uint32_t off : kModOrder)
            for (size_t i = 0; i < t.modOffsets.size(); ++i)
                if (t.modOffsets[i] == off) mod.push_back(item(FxRef::K::Mod, int(i), edpages::displayName(t.mod[i].name)));
        ins.sections.push_back({"MOD", pages(mod)});
        std::vector<Item> sends;
        for (int k : kInsertSends) sends.push_back(routing(k));
        ins.sections.push_back({"SENDS", pages(sends)});
        areas_.push_back(ins);
        Area rev{"REVERB", {}};
        std::vector<Item> algo = {item(FxRef::K::ReverbVar, 0, "VARIATION"), routing(kReverbMixSlot)};
        for (int k : t.reverb.order) algo.push_back(item(FxRef::K::Reverb, k, edpages::displayName(t.reverb.params[size_t(k)].name)));
        rev.sections.push_back({"ALGO", pages(algo)});
        std::vector<Item> rsends;
        for (int k : kReverbSends) rsends.push_back(routing(k));
        rev.sections.push_back({"SENDS", pages(rsends)});
        areas_.push_back(rev);
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
    case 'R': return {edproto::kPerformance, edproto::kEditBuffer, 0, 0};   // the running performance (effects that play)
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
    bool live = false, arpPreset = false, flags = false, names = false, effects = false;
    auto range = [&](char kind, uint32_t from, uint32_t to) {
        auto it = ranges.find(kind);
        if (it == ranges.end()) ranges[kind] = {from, to};
        else it->second = {std::min(it->second.first, from), std::max(it->second.second, to)};
    };
    for (const Row& r : s.pages[size_t(page_)])
        for (const Item& x : r.items) {
            if (x.ref < 0) continue;
            const Ref& ref = refs_[size_t(x.ref)];
            if (ref.kind == 'x') { effects = true; continue; }
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
    if (effects) {                                   // the preset's effect bytes and what plays (variations)
        range('P', kFxFrom, kFxTo);
        const ObjKey k = keyFor('R');
        reads.push_back(edproto::partialRead({k.type, k.number, k.subtype, k.subnumber, kRunningFrom, kRunningLength}));
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
    if (key == keyFor('P'))                          // a copy's read-back: does the edit buffer hold the copy?
        for (VarCopy& c : copy_) {
            if (!c.active || c.readsOut <= 0) continue;
            for (const auto& rg : c.ranges) {
                if (rg.first != a.offset || rg.second.size() != r.data.size()) continue;
                if (r.data != rg.second) c.mismatch = true;
                if (--c.readsOut == 0) {
                    if (c.mismatch) c.rereadAt = now_ + kCopyRereadMs;
                    else c.matched = true;
                }
            }
        }
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
    if (key == keyFor('R')) runningAt_ = now_;
    if (key == keyFor('P'))                          // a variation being copied: the copy stays shown
        for (const VarCopy& c : copy_)
            if (c.active)
                for (const auto& rg : c.ranges)
                    for (size_t i = 0; i < rg.second.size() && rg.first + i < o.bytes.size(); ++i) {
                        o.bytes[rg.first + i] = rg.second[i];
                        o.valid[rg.first + i] = true;
                    }
    // Writes not sent yet (and the value being dragged) stay shown.
    for (const auto& kv : pending_) apply(kv.first, kv.second);
    if (dragRef_ >= 0) apply(dragRef_, dragValue_);
    if (modPending_ && key == keyFor('P') && o.bytes.size() >= 74)   // the modulation block not written yet
        for (size_t i = 0; i < modBlock_.size(); ++i) { o.bytes[64 + i] = modBlock_[i]; o.valid[64 + i] = true; }
    if (key == keyFor('P')) checkFxAlgo();
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
    if (r.kind == 'x') {
        const FxRef& e = fx_[size_t(r.field)];
        int v;
        switch (e.k) {
        case FxRef::K::Algo: d = runningAlgo(); return d >= 0;
        case FxRef::K::InsertVar: if (!insertVariation(v)) return false; d = v; return true;
        case FxRef::K::ReverbVar: if (!reverbVariation(v)) return false; d = v; return true;
        default: return fxRaw(fxOffset(e), fxSize(e), fxLocked(e), d);
        }
    }
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
    if (r.kind == 'x') {
        const FxRef& e = fx_[size_t(r.field)];
        const edtables::FxTables& t = tables_.fx;
        const int a = runningAlgo();
        int64_t w1, w2;
        int b1, b2;
        switch (e.k) {
        case FxRef::K::Algo: lo = 0; hi = int64_t(t.inserts.size()) - 1; return true;
        case FxRef::K::InsertVar:                    // PSEt only while the selected algorithm is the stored one
            if (a < 0 || !fxByte(330, b1) || !fxByte(331, b2) || !fxRaw(174, 2, false, w1) || !fxRaw(176, 2, false, w2)) return false;
            lo = b1 == w1 && b2 == w2 ? -1 : 0;
            hi = int64_t(t.inserts[size_t(a)].variations.size()) - 1;
            return lo <= hi;
        case FxRef::K::ReverbVar:                    // PSEt only while the stored reverb area is family 1 / sub 0
            if (!fxRaw(294, 2, false, w1) || !fxRaw(296, 2, false, w2)) return false;
            lo = w1 == 1 && w2 == 0 ? -1 : 0;
            hi = int64_t(t.reverb.variations.size()) - 1;
            return lo <= hi;
        case FxRef::K::Mod:
            if (fxOffset(e) == 65) {                 // the destination: a slot of the running algorithm
                if (a < 0) return false;
                lo = 0; hi = int64_t(t.inserts[size_t(a)].params.size()) - 1;
                return lo <= hi;
            }
            if (fxOffset(e) >= 70) {                 // destination min / max: the destination's range (11 if tempo-syncable)
                const edtables::FxParam* p = fxParam(e);
                if (!p) return false;
                lo = p->tempoSync ? 11 : p->min; hi = p->max;
                return lo <= hi;
            }
            [[fallthrough]];
        default: {
            const edtables::FxParam* p = fxParam(e);
            if (!p || !p->found || p->hidden() || p->max < p->min) return false;
            lo = p->min; hi = p->max;
            return true;
        }
        }
    }
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
    if (r.kind == 'x') {                             // once its bytes are read; under a variation if it can be copied
        const FxRef& e = fx_[size_t(r.field)];
        int64_t lo, hi, d;
        if (!rangeOf(ref, lo, hi) || !valueOf(ref, d)) return false;
        const bool locked = fxLocked(e);
        if (locked && !copyPossible(lockSlot(e))) return false;
        if (e.k == FxRef::K::Mod)                    // the whole block is written
            for (uint32_t i = 64; i < 74; ++i) { int64_t b; if (!fxRaw(i, 1, locked, b)) return false; }
        return true;
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
    if (r.kind == 'x') {
        const FxRef& e = fx_[size_t(r.field)];
        const edtables::FxTables& t = tables_.fx;
        const int a = runningAlgo();
        switch (e.k) {
        case FxRef::K::Algo:
            if (d < 0 || size_t(d) >= t.inserts.size()) return std::to_string(d);
            if (!edpages::trimmed(t.inserts[size_t(d)].name).empty()) return edpages::trimmed(t.inserts[size_t(d)].name);
            return size_t(d) < t.codes.size() ? edpages::trimmed(t.codes[size_t(d)]) : std::to_string(d);
        case FxRef::K::InsertVar: case FxRef::K::ReverbVar:
            if (d < 0) return t.preset;
            return edtables::formatNumber(e.k == FxRef::K::InsertVar ? t.insertVariationFmt : t.reverbVariationFmt, int(d) + 1);
        default: {
            const edtables::FxParam* p = fxParam(e);
            if (!p) return std::to_string(d);
            return edtables::formatEffect(*p, int(d), t, a >= 0 ? &t.inserts[size_t(a)] : nullptr);
        }
        }
    }
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
    if (r.kind == 'x') {
        if (!editable(ref)) return out;
        const FxRef& e = fx_[size_t(r.field)];
        const edtables::FxTables& t = tables_.fx;
        int64_t lo, hi;
        if (!rangeOf(ref, lo, hi) || hi - lo > 255) return out;
        const edtables::FxParam* p = fxParam(e);
        const bool named = e.k == FxRef::K::Algo || e.k == FxRef::K::InsertVar || e.k == FxRef::K::ReverbVar ||
                           (e.k == FxRef::K::Mod && fxOffset(e) == 65) || (p && (p->hasList || p->formatter == 80 || p->formatter == 82));
        if (!named) return out;
        const int a = runningAlgo();
        for (int64_t d = lo; d <= hi; ++d) {
            std::string label = textOf(ref, d);
            if (d >= 0 && e.k == FxRef::K::InsertVar && a >= 0 && size_t(d) < t.inserts[size_t(a)].variations.size())
                label += " " + edpages::trimmed(t.inserts[size_t(a)].variations[size_t(d)]);
            if (d >= 0 && e.k == FxRef::K::ReverbVar && size_t(d) < t.reverb.variations.size())
                label += " " + edpages::trimmed(t.reverb.variations[size_t(d)]);
            out.push_back({label, d, known && cur == d});
        }
        return out;
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
    if (r.kind == 'x') {                             // effect numbers: decimals and milliseconds as shown
        const edtables::FxParam* p = fxParam(fx_[size_t(r.field)]);
        if (p && !p->hasList && p->formatter != 82) {
            const std::string t = edpages::trimmed(s);
            size_t i = 0;
            const bool neg = i < t.size() && t[i] == '-';
            if (i < t.size() && (t[i] == '-' || t[i] == '+')) ++i;
            int64_t whole = 0, frac = 0;
            int digits = 0, fracDigits = 0;
            for (; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i, ++digits) whole = whole * 10 + (t[i] - '0');
            if (i < t.size() && t[i] == '.')
                for (++i; i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])); ++i)
                    if (fracDigits < p->decimals) { frac = frac * 10 + (t[i] - '0'); ++fracDigits; }
            if (digits == 0 && fracDigits == 0) return false;
            for (; fracDigits < p->decimals; ++fracDigits) frac *= 10;
            int64_t scale = 1;
            for (int k = 0; k < p->decimals; ++k) scale *= 10;
            d = (whole * scale + frac) * (neg ? -1 : 1);
            if (p->formatter == 80) d += int64_t(tables_.fx.syncNames.size());   // milliseconds after the sync values
            return true;
        }
    }
    if (pf && pf->kind == Kind::Tempo) {             // the tempo may be typed with the format's letters
        std::string digits;
        for (char c : s) if (std::isdigit(static_cast<unsigned char>(c))) digits.push_back(c);
        return parseInt(digits, d);
    }
    return parseInt(s, d);
}

// ------------------------------------------------------------------ writing

void Controller::write(int ref, int64_t d, bool exact) {
    if (!open_ || !link_.running() || !editable(ref)) return;
    if (!notice_.empty()) setNotice("");             // a new edit: an earlier message no longer applies
    int64_t lo, hi;
    if (!rangeOf(ref, lo, hi)) return;
    if (!exact) d = std::clamp(d, lo, hi);          // a revert restores the stored value, even outside the table's range
    int64_t cur;
    if (!before_.count(ref) && valueOf(ref, cur)) before_[ref] = cur;
    if (refs_[size_t(ref)].kind == 'x') {
        const FxRef& e = fx_[size_t(refs_[size_t(ref)].field)];
        if (e.k == FxRef::K::InsertVar) copyFailed_[0] = false;   // another variation: a copy may be tried again
        if (e.k == FxRef::K::ReverbVar) copyFailed_[1] = false;
        // A field a variation plays: the first edit copies the variation to PSEt (write path 4b); the edit is queued
        // behind the copy.
        if (fxLocked(e) && !startCopy(lockSlot(e))) return;
    }
    if (refs_[size_t(ref)].kind == 'x' && fx_[size_t(refs_[size_t(ref)].field)].k == FxRef::K::Mod) {
        // The insert modulation: the whole block from the last read-back with this field changed (a new destination
        // also resets dest min / max to its range, as the OS does), written once per gesture.
        const FxRef& e = fx_[size_t(refs_[size_t(ref)].field)];
        std::array<uint8_t, 10> blk{};
        for (uint32_t i = 0; i < 10; ++i) { int b; if (!fxByte(64 + i, b)) return; blk[i] = uint8_t(b); }
        auto put = [&](uint32_t off, int size, int64_t v) {
            if (size == 1) blk[off - 64] = uint8_t(v);
            else { blk[off - 64] = uint8_t(uint16_t(v) >> 8); blk[off - 63] = uint8_t(v); }
        };
        put(fxOffset(e), fxSize(e), d);
        if (fxOffset(e) == 65) {
            const int a = runningAlgo();
            const edtables::FxParam& p = tables_.fx.inserts[size_t(a)].params[size_t(d)];
            put(70, 2, p.tempoSync ? 11 : p.min);
            put(72, 2, p.max);
        }
        modBlock_ = blk;
        modPending_ = true;
        modReady_ = dragRef_ != ref;                 // a drag writes on release
        Obj& o = objs_[keyFor('P')];
        for (size_t i = 0; i < blk.size(); ++i) o.bytes[64 + i] = blk[i];
        edited_ = true;
        lastWriteAt_ = now_;
        pump();
        render();
        return;
    }
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
    if (r.kind == 'x') {                             // into the edit buffer's bytes as the editor holds them
        const FxRef& e = fx_[size_t(r.field)];
        if (e.k == FxRef::K::Mod) return;            // the block (write)
        Obj& o = objs_[keyFor('P')];
        auto put = [&](uint32_t off, uint8_t b) { if (off < o.bytes.size() && o.valid[off]) o.bytes[off] = b; };
        if (e.k == FxRef::K::Algo) {                 // the selected algorithm, its first variation
            if (d < 0 || size_t(d) >= tables_.fx.inserts.size()) return;
            put(330, uint8_t(tables_.fx.inserts[size_t(d)].family));
            put(331, uint8_t(tables_.fx.inserts[size_t(d)].sub));
            put(332, 0);
            checkFxAlgo();
            return;
        }
        const uint32_t off = fxOffset(e);
        if (fxSize(e) == 1) put(off, uint8_t(d < 0 ? 0xFF : d));
        else { put(off, uint8_t(uint16_t(d) >> 8)); put(off + 1, uint8_t(d)); }
        if (e.k == FxRef::K::InsertVar || e.k == FxRef::K::ReverbVar) checkFxAlgo();
        return;
    }
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
    // Effect sends wait while the OS rebuilds its effects (the rebuild hold) and while a variation is copied to PSEt;
    // other writes go on.
    auto held = [&](int ref) { return refs_[size_t(ref)].kind == 'x' && (now_ < fxHoldUntil_ || copying()); };
    while (now_ >= nextSendAt_ && !pending_.empty()) {
        auto it = pending_.find(dragRef_);           // the value being dragged first
        if (it == pending_.end() || held(it->first))
            for (it = pending_.begin(); it != pending_.end() && held(it->first); ++it) {}
        if (it == pending_.end()) break;
        const int ref = it->first;
        const int64_t d = it->second;
        pending_.erase(it);
        sendWrite(ref, d);
    }
    pumpMod();
}

void Controller::sendWrite(int ref, int64_t d) {
    const Ref& r = refs_[size_t(ref)];
    std::vector<std::vector<uint8_t>> msgs;
    bool isRead = false, rebuild = false;            // rebuild: NRPN LSB 30-33, the OS rebuilds its effects
    if (const edfields::Field* f = presetField(r)) {
        if (r.kind == 'm' && f->nrpnLsb >= 0) msgs = edproto::nrpn(channel_, 1, f->nrpnLsb, int(d));
    } else if (r.kind == 'x') {
        const FxRef& e = fx_[size_t(r.field)];
        auto nrpn = [&](int lsb, int v) { for (auto& m : edproto::nrpn(channel_, 1, lsb, v)) msgs.push_back(m); };
        switch (e.k) {
        case FxRef::K::Algo:                         // family, sub, then variation 1: the OS rebuilds the insert
            if (d < 0 || size_t(d) >= tables_.fx.inserts.size()) return;
            nrpn(30, tables_.fx.inserts[size_t(d)].family);
            nrpn(31, tables_.fx.inserts[size_t(d)].sub);
            nrpn(32, 0);
            rebuild = true;
            break;
        case FxRef::K::InsertVar: nrpn(32, int(d)); rebuild = true; break;
        case FxRef::K::ReverbVar: nrpn(33, int(d)); rebuild = true; break;
        case FxRef::K::Mod: return;                  // the block (pumpMod)
        default: {                                   // the word into the edit buffer, then applied live
            const uint32_t off = fxOffset(e);
            msgs.push_back(edproto::partialWrite({edproto::kRawMemory, 0, 0, 0, edproto::kEditBufferRam + off, 2},
                                                 {uint8_t(uint16_t(d) >> 8), uint8_t(d)}));
            const int slot = e.k == FxRef::K::Insert ? 1 : e.k == FxRef::K::Reverb ? 3 : 0;
            const int param = e.k == FxRef::K::InsertMix ? kInsertMixSlot : e.n;
            msgs.push_back(edproto::fxParameter(slot, param, int(d)));
            break;
        }
        }
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
    if (rebuild) fxHoldUntil_ = now_ + kFxHoldMs;
    afterWrite();
}

void Controller::afterWrite() {
    edited_ = true;
    lastWriteAt_ = now_;
    readBackAt_ = std::max(now_ + kReadBackMs, fxHoldUntil_);   // after the rebuild hold, if one runs
}

void Controller::presetChanged() {
    before_.clear();
    edited_ = false;
    for (int s = 0; s < 2; ++s) copyAbort(s, false);   // a copy never lands in another preset
    copyFailed_ = {};
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
                        if (ref.kind == 'x') {
                            int64_t d;
                            if (valueOf(x.ref, d)) x.value = textOf(x.ref, d);
                        } else if (const edfields::Field* f = presetField(ref)) {
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
                // Effect values a variation plays that cannot be edited now (no copy possible): not underlined.
                for (const auto& sp : v.screen.values)
                    if (sp.ref >= 0 && refs_[size_t(sp.ref)].kind == 'x' && fxLocked(fx_[size_t(refs_[size_t(sp.ref)].field)]) &&
                        !editable(sp.ref))
                        for (int c = sp.col; c < sp.col + sp.len && c < edpages::kCols; ++c) v.screen.underline[size_t(sp.row)][size_t(c)] = false;
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
        lockedHint(ref);
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
        if (modPending_) { modReady_ = true; pump(); }   // the modulation block: written on release
    } else if (action == "type" && validRef) {
        lockedHint(ref);
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
        if (lockedHint(focus_)) return true;
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
    if (lockedHint(ref)) return;
    write(ref, value);
}

void Controller::revert(int ref) {
    auto it = before_.find(ref);
    if (it != before_.end()) write(ref, it->second, true);   // the exact value it had, unclamped
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

// ------------------------------------------------------------------ effects (docs/editor/EFFECTS_EDITOR_SPEC.md)

int Controller::fxRefOf(FxRef::K k, int n) const {
    for (size_t i = 0; i < refs_.size(); ++i)
        if (refs_[i].kind == 'x' && fx_[size_t(refs_[i].field)].k == k && fx_[size_t(refs_[i].field)].n == n) return int(i);
    return -1;
}

bool Controller::fxByte(uint32_t off, int& b) const {
    const Obj* o;
    if (!bytesOf('P', o) || off >= o->bytes.size() || !o->valid[off]) return false;
    b = o->bytes[off];
    return true;
}

bool Controller::fxRaw(uint32_t off, int size, bool running, int64_t& v) const {
    const Obj* o;
    if (running && off < kFxFrom) return false;
    const uint32_t at = running ? kRunningFrom + (off - kFxFrom) : off;
    if (!bytesOf(running ? 'R' : 'P', o) || at + uint32_t(size) > o->bytes.size()) return false;
    for (int i = 0; i < size; ++i) if (!o->valid[at + uint32_t(i)]) return false;
    v = size == 1 ? int64_t(o->bytes[at]) : int64_t(int16_t(uint16_t(o->bytes[at] << 8 | o->bytes[at + 1])));
    return true;
}

int Controller::runningAlgo() const {
    int fam, sub;
    if (!tables_.fx.ok || !fxByte(330, fam) || !fxByte(331, sub)) return -1;
    for (size_t i = 0; i < tables_.fx.inserts.size(); ++i)
        if ((tables_.fx.inserts[i].family & 0xFF) == fam && (tables_.fx.inserts[i].sub & 0xFF) == sub) return int(i);
    return -1;
}

bool Controller::insertVariation(int& v) const {
    int b;
    if (!fxByte(332, b)) return false;
    v = b == 0xFF ? -1 : b;
    return true;
}

bool Controller::reverbVariation(int& v) const {
    int b;
    if (!fxByte(333, b)) return false;
    v = b == 0xFF ? -1 : b;
    return true;
}

int Controller::lockSlot(const FxRef& e) const {
    switch (e.k) {
    case FxRef::K::Insert: case FxRef::K::Mod: return 0;
    case FxRef::K::Routing: return e.n == 4 || e.n == 6 ? 0 : -1;   // the FX-bus 1 reverb send plays the insert variation's
    case FxRef::K::Reverb: return 1;
    default: return -1;
    }
}

bool Controller::variationPlays(int slot) const {
    int v = 0;
    return slot == 0 ? !insertVariation(v) || v >= 0 : !reverbVariation(v) || v >= 0;
}

// The value a variation plays (shown from the running performance); not while the slot is being copied to PSEt.
bool Controller::fxLocked(const FxRef& e) const {
    const int slot = lockSlot(e);
    if (slot < 0 || copy_[size_t(slot)].active) return false;
    return variationPlays(slot);
}

uint32_t Controller::fxOffset(const FxRef& e) const {
    int v = -1;
    switch (e.k) {
    case FxRef::K::Algo: return 330;
    case FxRef::K::InsertVar: return 332;
    case FxRef::K::ReverbVar: return 333;
    case FxRef::K::InsertMix: return insertVariation(v) && v >= 0 ? kInsertMixVariation : kInsertMixPreset;
    case FxRef::K::Routing: return 110 + 2 * uint32_t(e.n);
    case FxRef::K::Insert: return 194 + 2 * uint32_t(e.n);
    case FxRef::K::Reverb: return 314 + 2 * uint32_t(e.n);
    case FxRef::K::Mod: return 64 + (size_t(e.n) < tables_.fx.modOffsets.size() ? tables_.fx.modOffsets[size_t(e.n)] : 0);
    }
    return 0;
}

int Controller::fxSize(const FxRef& e) const {
    switch (e.k) {
    case FxRef::K::Algo: case FxRef::K::InsertVar: case FxRef::K::ReverbVar: return 1;
    case FxRef::K::Mod: return fxOffset(e) <= 65 ? 1 : 2;   // source and destination are bytes
    default: return 2;
    }
}

const edtables::FxParam* Controller::fxParam(const FxRef& e) const {
    const edtables::FxTables& t = tables_.fx;
    const int a = runningAlgo();
    switch (e.k) {
    case FxRef::K::InsertMix: return t.routing.params.size() > kInsertMixSlot ? &t.routing.params[kInsertMixSlot] : nullptr;
    case FxRef::K::Routing: return size_t(e.n) < t.routing.params.size() ? &t.routing.params[size_t(e.n)] : nullptr;
    case FxRef::K::Reverb: return size_t(e.n) < t.reverb.params.size() ? &t.reverb.params[size_t(e.n)] : nullptr;
    case FxRef::K::Insert:
        if (a < 0 || size_t(e.n) >= t.inserts[size_t(a)].params.size()) return nullptr;
        return &t.inserts[size_t(a)].params[size_t(e.n)];
    case FxRef::K::Mod: {
        if (size_t(e.n) >= t.mod.size()) return nullptr;
        const uint32_t off = fxOffset(e);
        if (off == 70 || off == 72) {                // destination min / max: in the destination parameter's terms
            int64_t dest;
            if (a < 0 || !fxRaw(65, 1, fxLocked(e), dest) || dest < 0 || size_t(dest) >= t.inserts[size_t(a)].params.size()) return nullptr;
            return &t.inserts[size_t(a)].params[size_t(dest)];
        }
        return &t.mod[size_t(e.n)];
    }
    default: return nullptr;
    }
}

int Controller::fxWidth(const FxRef& e) const {
    const edtables::FxTables& t = tables_.fx;
    if (e.k == FxRef::K::Algo) return 16;
    if (e.k == FxRef::K::InsertVar || e.k == FxRef::K::ReverbVar) {
        const std::string& fmt = e.k == FxRef::K::InsertVar ? t.insertVariationFmt : t.reverbVariationFmt;
        return int(std::max(t.preset.size(), edtables::formatNumber(fmt, 99).size()));
    }
    const edtables::FxAlgo* a = runningAlgo() >= 0 ? &t.inserts[size_t(runningAlgo())] : nullptr;
    if (e.k == FxRef::K::Mod && fxOffset(e) == 65) { // the destination: the running algorithm's parameter names
        size_t w = 4;
        if (a) for (const edtables::FxParam& q : a->params) w = std::max(w, edpages::trimmed(q.name).size());
        return int(std::min<size_t>(w, 16));
    }
    if (e.k == FxRef::K::Mod && fxOffset(e) >= 70) { // destination min / max: the widest of the destination's values
        size_t w = 4;
        if (a) for (int k = 0; k < int(a->params.size()); ++k) w = std::max(w, size_t(fxWidth({FxRef::K::Insert, k})));
        return int(std::min<size_t>(w, 16));
    }
    const edtables::FxParam* p = fxParam(e);
    if (!p) return 4;
    size_t w = std::max(edtables::formatEffect(*p, p->min, t, a).size(), edtables::formatEffect(*p, p->max, t, a).size());
    if (p->hasList) for (const std::string& x : p->list) w = std::max(w, edpages::trimmed(x).size());
    if (p->formatter == 80) for (const std::string& x : t.syncNames) w = std::max(w, edpages::trimmed(x).size());
    if (p->formatter == 82) w = 16;
    return int(std::min<size_t>(w, 16));
}

std::vector<Page> Controller::insertAlgoPages() {
    const edtables::FxTables& t = tables_.fx;
    auto item = [&](FxRef::K k, int n, const std::string& label) {
        const int ref = fxRefOf(k, n);
        return Item{label, "", ref, ref >= 0 ? fxWidth(fx_[size_t(refs_[size_t(ref)].field)]) : 0};
    };
    auto routingName = [&](int k) { return edpages::displayName(t.routing.params[size_t(k)].name); };
    std::vector<Row> rows;
    Row top;                                         // ALGO and VARIATION side by side
    top.kind = edpages::RowKind::Fixed;
    Item algo = item(FxRef::K::Algo, 0, "ALGO"), var = item(FxRef::K::InsertVar, 0, "VARIATION");
    algo.col = 0; var.col = 25;
    top.items = {algo, var};
    rows.push_back(top);
    std::vector<Item> first = {item(FxRef::K::InsertMix, 0, routingName(kInsertMixSlot))};
    for (int k : kInsertAlgoRouting) first.push_back(item(FxRef::K::Routing, k, routingName(k)));
    for (const Row& r : edpages::twoColumnRows(first)) rows.push_back(r);
    std::vector<Page> pages = edpages::paginate(rows);
    if (fxAlgo_ >= 0) {                              // the running algorithm's parameters in the OS's edit order
        const edtables::FxAlgo& a = t.inserts[size_t(fxAlgo_)];
        std::vector<Item> items;
        for (int k : a.order) items.push_back(item(FxRef::K::Insert, k, edpages::displayName(a.params[size_t(k)].name)));
        if (!items.empty()) for (const Page& pg : edpages::paginate(edpages::twoColumnRows(items))) pages.push_back(pg);
    }
    return pages;
}

void Controller::checkFxAlgo() {
    const int a = runningAlgo();
    if (a == fxAlgo_) return;
    fxAlgo_ = a;
    for (auto it = before_.begin(); it != before_.end();) {   // revert values of the previous algorithm's slots
        const Ref& r = refs_[size_t(it->first)];
        const bool slot = r.kind == 'x' && (fx_[size_t(r.field)].k == FxRef::K::Insert || fx_[size_t(r.field)].k == FxRef::K::Mod);
        it = slot ? before_.erase(it) : std::next(it);
    }
    for (size_t i = 0; i < areas_.size(); ++i) {
        if (areas_[i].name != "INSERT") continue;
        for (size_t j = 0; j < areas_[i].sections.size(); ++j) {
            Section& sec = areas_[i].sections[j];
            if (sec.name == "ALGO") sec.pages = insertAlgoPages();
            else if (sec.name == "MOD")                    // the destination's width follows the algorithm
                for (auto& pg : sec.pages) for (Row& r : pg) for (Item& x : r.items)
                    if (x.ref >= 0) x.width = fxWidth(fx_[size_t(refs_[size_t(x.ref)].field)]);
            if (int(i) == area_ && int(j) == section_ && page_ >= int(sec.pages.size())) page_ = int(sec.pages.size()) - 1;
        }
    }
    render();
}

bool Controller::lockedHint(int ref) {
    if (ref < 0 || size_t(ref) >= refs_.size() || refs_[size_t(ref)].kind != 'x') return false;
    const FxRef& e = fx_[size_t(refs_[size_t(ref)].field)];
    if (!fxLocked(e) || editable(ref)) return false;  // editable under a variation: the edit copies it to PSEt
    if (copyFailed_[size_t(lockSlot(e))]) { setNotice(kCopyFailed); render(); }
    return true;
}

// ---- copying a variation to PSEt (write path 4b)

bool Controller::copyPossible(int slot) const {
    if (slot < 0 || slot > 1 || copyFailed_[size_t(slot)] || !open_ || !link_.running()) return false;
    if (now_ < fxHoldUntil_ || runningAt_ < fxHoldUntil_) return false;   // after a rebuild: what plays, read anew
    int v;
    if (!(slot == 0 ? insertVariation(v) : reverbVariation(v)) || v < 0) return false;
    for (const CopyRange& c : slot == 0 ? std::vector<CopyRange>(std::begin(kInsertCopy), std::end(kInsertCopy))
                                        : std::vector<CopyRange>(std::begin(kReverbCopy), std::end(kReverbCopy)))
        for (uint32_t o = c.from; o < c.to; ++o) { int64_t b; if (!fxRaw(o, 1, c.running, b)) return false; }
    return true;
}

bool Controller::startCopy(int slot) {
    if (slot < 0 || slot > 1) return false;
    if (copy_[size_t(slot)].active) return true;
    if (!copyPossible(slot)) return false;
    VarCopy c;
    c.active = true;
    for (const CopyRange& r : slot == 0 ? std::vector<CopyRange>(std::begin(kInsertCopy), std::end(kInsertCopy))
                                        : std::vector<CopyRange>(std::begin(kReverbCopy), std::end(kReverbCopy))) {
        std::vector<uint8_t> bytes;
        for (uint32_t o = r.from; o < r.to; ++o) { int64_t b = 0; fxRaw(o, 1, r.running, b); bytes.push_back(uint8_t(b)); }
        c.ranges.push_back({r.into, bytes});
    }
    Obj& o = objs_[keyFor('P')];                     // the editor holds the copy at once (the slot as under PSEt)
    for (const auto& rg : c.ranges) {
        if (o.bytes.size() < rg.first + rg.second.size()) { o.bytes.resize(rg.first + rg.second.size(), 0); o.valid.resize(o.bytes.size(), false); }
        for (size_t i = 0; i < rg.second.size(); ++i) { o.bytes[rg.first + i] = rg.second[i]; o.valid[rg.first + i] = true; }
    }
    copy_[size_t(slot)] = c;
    copyWrite(slot);
    return true;
}

void Controller::copyWrite(int slot) {
    VarCopy& c = copy_[size_t(slot)];
    ++c.attempt;
    for (const auto& rg : c.ranges)
        link_.send(edproto::partialWrite({edproto::kRawMemory, 0, 0, 0, edproto::kEditBufferRam + rg.first, uint32_t(rg.second.size())}, rg.second));
    c.writtenAt = lastWriteAt_ = lastSendAt_ = now_;
    copyRead(slot);
}

void Controller::copyRead(int slot) {
    VarCopy& c = copy_[size_t(slot)];
    c.readsOut = 0;
    c.mismatch = c.matched = false;
    c.rereadAt = -1;
    for (const auto& rg : c.ranges)
        if (link_.send(edproto::partialRead({edproto::kPreset, 0, 0, 0, rg.first, uint32_t(rg.second.size())}))) {
            ++c.readsOut;
            ++outstanding_;
            lastSendAt_ = now_;
        }
}

void Controller::copyPump() {
    if (!open_ || !link_.running()) return;
    for (int s = 0; s < 2; ++s) {
        VarCopy& c = copy_[size_t(s)];
        if (!c.active) continue;
        if (c.matched) {                             // the edit buffer holds the copy: PSEt, then the rebuild hold
            for (const auto& m : edproto::nrpn(channel_, 1, s == 0 ? 32 : 33, -1)) link_.send(m);
            c.active = false;
            copyFailed_[size_t(s)] = false;
            fxHoldUntil_ = now_ + kFxHoldMs;
            Obj& o = objs_[keyFor('P')];
            const size_t at = s == 0 ? 332 : 333;
            if (at < o.bytes.size() && o.valid[at]) o.bytes[at] = 0xFF;
            lastSendAt_ = now_;
            afterWrite();                            // the read-back after the hold
            checkFxAlgo();
            render();
        } else if (now_ - c.writtenAt >= kCopyVerifyMs) {   // not stored within 1 s: write once more, then give up
            if (c.attempt < 2) copyWrite(s);
            else copyAbort(s, true);
        } else if (c.readsOut == 0 && c.mismatch && c.rereadAt >= 0 && now_ >= c.rereadAt) {
            copyRead(s);                             // not all stored yet: read again
        }
    }
}

void Controller::copyAbort(int slot, bool failed) {
    VarCopy& c = copy_[size_t(slot)];
    if (!c.active) return;
    c.active = false;
    copyFailed_[size_t(slot)] = failed;
    auto ofSlot = [&](int ref) { return refs_[size_t(ref)].kind == 'x' && lockSlot(fx_[size_t(refs_[size_t(ref)].field)]) == slot; };
    for (auto it = pending_.begin(); it != pending_.end();) it = ofSlot(it->first) ? pending_.erase(it) : std::next(it);
    if (slot == 0) modPending_ = modReady_ = false;
    if (dragRef_ >= 0 && ofSlot(dragRef_)) dragRef_ = -1;
    if (failed) setNotice(kCopyFailed);
    readDue_ = now_;                                 // show what the OS holds
    render();
}

// The insert modulation block: one partial write of the edit buffer (offset 64, 10 bytes) per gesture, not during
// the rebuild hold; it starts a new hold, and the read-back follows it.
void Controller::pumpMod() {
    if (!modPending_ || !modReady_ || now_ < fxHoldUntil_ || copying() || !open_ || !link_.running()) return;
    if (!link_.send(edproto::partialWrite({edproto::kPreset, 0, 0, 0, 64, 10}, std::vector<uint8_t>(modBlock_.begin(), modBlock_.end()))))
        return;
    modPending_ = modReady_ = false;
    lastSendAt_ = now_;
    fxHoldUntil_ = now_ + kFxHoldMs;
    afterWrite();
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

    // Edits: a variation being copied to PSEt, paced sends, the arp edit through the panel's buttons, and the read-back
    // once everything is out.
    copyPump();
    pump();
    arpPump();
    const bool quiet = pending_.empty() && pendingArp_.empty() && !jobActive_ && rmw_.empty() && !modPending_ && !copying();
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
