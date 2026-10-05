#include "editor_controller.h"
#include <algorithm>
#include <cstdlib>
#include <sstream>

namespace editor {

using edpages::Item;
using edpages::Page;
using edpages::Row;

namespace {
const char* const kOscSections[] = {"WAVE", "PITCH", "FILTER", "AMP", "NOISE", "LFO", "ENV1", "ENV2", "ENV3", "GEN"};
const char* const kSoundSections[] = {"TRACK", "RANGE", "RECEIVE"};

// The widest text a field's value can take (cells reserved for it on the page).
int valueWidth(const edtables::Field& f, const edtables::Table& t, size_t waveNameMax) {
    using namespace edtables;
    if (f.type == kLong) return int(2 + (waveNameMax ? 1 + waveNameMax : 0));
    if (f.display == Display::List && f.listIndex >= 0 && size_t(f.listIndex) < t.lists.size()) {
        size_t w = 0;
        for (const std::string& e : t.lists[size_t(f.listIndex)].entries) {
            const size_t a = e.find_first_not_of(' '), b = e.find_last_not_of(' ');
            if (a != std::string::npos) w = std::max(w, b - a + 1);
        }
        w = std::max(w, std::to_string(f.max).size());   // values past the list's end show as numbers
        return int(w);
    }
    if (f.format == kFmtOffOn) return 3;
    if (f.format == kFmtNote) return 4;
    const bool sign = f.display == Display::Formatter || (f.format & kFmtSigned) || f.min < 0;
    const int lo = f.type == kBitField ? 0 : f.min, hi = f.type == kBitField ? 255 : std::max<int>(f.max, 0);
    return int(std::max(std::to_string(lo).size(), std::to_string(hi + 1).size()) + (sign ? 1 : 0));
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

void Controller::setOpen(bool open) {
    if (open == open_) return;
    open_ = open;
    link_.setOpen(open);
    if (open) {
        wavesRequested_ = false;
        lastLeds_ = link_.ledStateMessages();
        lastGestures_ = link_.panelGestures();
        lastTriggers_ = link_.presetTriggers();
        lastDisplay_ = link_.display();
        followLeds();
        readDue_ = now_;                             // read the visible page on the next tick
    } else {
        outstanding_ = 0;
        readDue_ = displayChangedAt_ = -1;
    }
    render();
}

// ------------------------------------------------------------------ structure

void Controller::build() {
    const std::string area = areas_.empty() ? std::string() : areas_[size_t(area_)].name;
    const std::string section = areas_.empty() || areas_[size_t(area_)].sections.empty() ? std::string()
                                : areas_[size_t(area_)].sections[size_t(section_)].name;
    areas_.clear();
    refs_.clear();
    size_t waveNameMax = 0;
    for (const auto& n : waveNames_) waveNameMax = std::max(waveNameMax, n.size());
    auto item = [&](const edtables::Table& t, int field, char kind) {
        const edtables::Field& f = t.fields[size_t(field)];
        refs_.push_back({&t, field, kind});
        return Item{f.name, "", int(refs_.size()) - 1, valueWidth(f, t, waveNameMax)};
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
                const auto& f = tables_.oscillator.fields[i];
                if (!f.hidden && edpages::oscillatorSection(f.name) == sec) items.push_back(item(tables_.oscillator, int(i), 'o'));
            }
            std::vector<Page> pages = twoColumnPages(items, std::string(sec).rfind("ENV", 0) == 0);
            if (!pages.empty()) a.sections.push_back({sec, pages});
        }
        if (!a.sections.empty()) areas_.push_back(a);
    }
    // Keep the position if the same area and section still exist.
    area_ = section_ = page_ = 0;
    for (size_t i = 0; i < areas_.size(); ++i)
        if (areas_[i].name == area) {
            area_ = int(i);
            for (size_t j = 0; j < areas_[i].sections.size(); ++j) if (areas_[i].sections[j].name == section) section_ = int(j);
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
    default: return {0xFF, 0, 0, 0};
    }
}

// ------------------------------------------------------------------ reading

std::vector<std::vector<uint8_t>> Controller::pageReads() const {
    std::vector<std::vector<uint8_t>> reads;
    const Area* a = currentArea();
    if (!a || a->sections.empty()) return reads;
    const Section& s = a->sections[size_t(section_)];
    if (s.pages.empty()) return reads;
    std::map<char, std::pair<uint32_t, uint32_t>> ranges;   // kind -> [from, to)
    for (const Row& r : s.pages[size_t(page_)])
        for (const Item& x : r.items) {
            if (x.ref < 0) continue;
            const Ref& ref = refs_[size_t(x.ref)];
            if (ref.kind == 'n') {                   // a preset name: 16 characters at offset 8 of the stored preset
                reads.push_back(edproto::partialRead({edproto::kPreset, uint16_t(edproto::kStoredPreset + ref.field), 0, 0, 8, 16}));
                continue;
            }
            const edtables::Field& f = ref.table->fields[size_t(ref.field)];
            auto it = ranges.find(ref.kind);
            if (it == ranges.end()) ranges[ref.kind] = {f.offset, f.offset + uint32_t(f.size())};
            else it->second = {std::min(it->second.first, f.offset), std::max(it->second.second, f.offset + uint32_t(f.size()))};
        }
    for (const auto& kv : ranges) {
        const ObjKey k = keyFor(kv.first);
        reads.push_back(edproto::partialRead({k.type, k.number, k.subtype, k.subnumber, kv.second.first, kv.second.second - kv.second.first}));
    }
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
    edproto::Message r;
    if (!edproto::parse(m.data(), m.size(), r) || r.command != edproto::kPartialReply) return;
    const edproto::Address& a = r.address;
    if (a.type == edproto::kRawMemory && a.offset == edtables::kWaveList + 4) {
        waves_.ids.assign(size_t(edtables::kWaves), 0);
        for (size_t i = 0; i < size_t(edtables::kWaves) && 4 * i + 3 < r.data.size(); ++i)
            waves_.ids[i] = uint32_t(r.data[4 * i]) << 24 | uint32_t(r.data[4 * i + 1]) << 16 | uint32_t(r.data[4 * i + 2]) << 8 | r.data[4 * i + 3];
        return;
    }
    if (a.type == edproto::kPreset && a.number >= edproto::kStoredPreset && a.offset == 8) {
        const int n = a.number - edproto::kStoredPreset;
        if (n >= 0 && n < kPresets) {
            std::string name(r.data.begin(), r.data.end());
            for (char& c : name) if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = ' ';
            presetNames_[size_t(n)] = name;
        }
        return;
    }
    Obj& o = objs_[{a.type, a.number, a.subtype, a.subnumber}];
    const size_t end = size_t(a.offset) + r.data.size();
    if (end > 1 << 16) return;
    if (o.bytes.size() < end) { o.bytes.resize(end, 0); o.valid.resize(end, false); }
    std::copy(r.data.begin(), r.data.end(), o.bytes.begin() + long(a.offset));
    std::fill(o.valid.begin() + long(a.offset), o.valid.begin() + long(end), true);
}

// ------------------------------------------------------------------ rendering

void Controller::render() {
    View v;
    {
        std::lock_guard<std::mutex> lk(viewMtx_);
        v.serial = view_.serial;
    }
    v.follow = follow_;
    v.sound = sound_; v.osc = osc_;
    v.data = open_ && outstanding_ > 0;
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
            v.prev = page_ > 0 || section_ > 0;
            v.next = page_ + 1 < int(s.pages.size()) || section_ + 1 < int(a->sections.size());
            if (!s.pages.empty()) {
                Page page = s.pages[size_t(page_)];
                for (Row& r : page)
                    for (Item& x : r.items) {
                        if (x.ref < 0) continue;
                        const Ref& ref = refs_[size_t(x.ref)];
                        if (ref.kind == 'n') { x.value = presetNames_[size_t(ref.field)]; continue; }
                        const edtables::Field& f = ref.table->fields[size_t(ref.field)];
                        auto it = objs_.find(keyFor(ref.kind));
                        int64_t raw;
                        bool known = it != objs_.end() && f.offset + size_t(f.size()) <= it->second.bytes.size() &&
                                     std::all_of(it->second.valid.begin() + long(f.offset), it->second.valid.begin() + long(f.offset + size_t(f.size())), [](bool b) { return b; });
                        if (known && edpages::rawValue(f, it->second.bytes.data(), it->second.bytes.size(), 0, raw))
                            x.value = edpages::formatValue(f, *ref.table, raw, &waves_);
                        else
                            x.value.clear();             // not read yet
                    }
                v.screen = edpages::render(page);
            }
        }
    }
    std::lock_guard<std::mutex> lk(viewMtx_);
    const bool same = v.screen.chars == view_.screen.chars && v.screen.underline == view_.screen.underline && v.area == view_.area &&
                      v.section == view_.section && v.page == view_.page && v.pages == view_.pages && v.sound == view_.sound &&
                      v.osc == view_.osc && v.prev == view_.prev && v.next == view_.next && v.follow == view_.follow &&
                      v.data == view_.data && v.areas == view_.areas && v.sections == view_.sections &&
                      v.soundTargets == view_.soundTargets && v.oscTargets == view_.oscTargets;
    v.serial = same ? view_.serial : view_.serial + 1;
    view_ = std::move(v);
}

View Controller::view() const {
    std::lock_guard<std::mutex> lk(viewMtx_);
    return view_;
}

// ------------------------------------------------------------------ navigation

void Controller::go(int area, int section, int page) {
    area_ = area; section_ = section; page_ = page;
    render();
    readPage();
}

void Controller::command(const std::string& action, const std::string& text) {
    Area* a = currentArea();
    if (action == "area") {
        for (size_t i = 0; i < areas_.size(); ++i) if (areas_[i].name == text) go(int(i), 0, 0);
    } else if (action == "section" && a) {
        for (size_t j = 0; j < a->sections.size(); ++j) if (a->sections[j].name == text) go(area_, int(j), 0);
    } else if (action == "target" && text.size() == 2) {
        const int n = text[1] - '1';
        if (text[0] == 'S' && n >= 0 && n < 4) { sound_ = n; follow_ = false; objs_.clear(); render(); readPage(); }
        if (text[0] == 'O' && n >= 0 && n < 2) { osc_ = n; follow_ = false; render(); readPage(); }
    } else if (action == "page" && a && !a->sections.empty()) {
        const int n = std::atoi(text.c_str()) - 1;
        if (n >= 0 && n < int(a->sections[size_t(section_)].pages.size())) go(area_, section_, n);
    } else if (action == "page_prev" && a && !a->sections.empty()) {
        if (page_ > 0) go(area_, section_, page_ - 1);
        else if (section_ > 0) go(area_, section_ - 1, int(a->sections[size_t(section_ - 1)].pages.size()) - 1);
    } else if (action == "page_next" && a && !a->sections.empty()) {
        if (page_ + 1 < int(a->sections[size_t(section_)].pages.size())) go(area_, section_, page_ + 1);
        else if (section_ + 1 < int(a->sections.size())) go(area_, section_ + 1, 0);
    } else if (action == "toggle_follow") {
        follow_ = !follow_;
        if (follow_) followLeds();
        render();
    }
}

// Follow: the panel's Sound / OSC LEDs pick the target, the envelope LEDs the envelope section.
void Controller::followLeds() {
    if (!follow_) return;
    int sound = sound_, osc = osc_;
    for (int i = 0; i < 4; ++i) if (soundLeds_[size_t(i)] >= 0 && link_.ledMode(soundLeds_[size_t(i)]) != 0) { sound = i; break; }
    for (int i = 0; i < 2; ++i) if (oscLeds_[size_t(i)] >= 0 && link_.ledMode(oscLeds_[size_t(i)]) != 0) { osc = i; break; }
    bool changed = sound != sound_ || osc != osc_;
    if (sound != sound_) objs_.clear();
    sound_ = sound; osc_ = osc;
    if (Area* a = currentArea(); a && a->name == "OSC" && !a->sections.empty() && a->sections[size_t(section_)].name.rfind("ENV", 0) == 0) {
        for (int e = 0; e < 3; ++e)
            if (envLeds_[size_t(e)] >= 0 && link_.ledMode(envLeds_[size_t(e)]) != 0) {
                const std::string want = "ENV" + std::to_string(e + 1);
                for (size_t j = 0; j < a->sections.size(); ++j)
                    if (a->sections[j].name == want && int(j) != section_) { section_ = int(j); page_ = 0; changed = true; }
                break;
            }
    }
    if (changed) { render(); readPage(); }
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
        followLeds();
    }
    if (gestures != lastGestures_) { lastGestures_ = gestures; readDue_ = now_ + kDebounceMs; }
    if (triggers != lastTriggers_) {                 // the engine queued the standing read behind it
        outstanding_ += int(pageReads().size()) * int(triggers - lastTriggers_);
        lastSendAt_ = now_;
        lastTriggers_ = triggers;
    }
    const std::array<char, 4> d = link_.display();
    if (d != lastDisplay_) { lastDisplay_ = d; displayChangedAt_ = now_; }
    // A new preset number with no LED burst after it (a preset whose LEDs equal the previous one's): read anyway.
    if (displayChangedAt_ >= 0 && now_ - displayChangedAt_ >= kDisplayFallbackMs) { displayChangedAt_ = -1; readDue_ = now_; }
    if (readDue_ >= 0 && now_ >= readDue_) { readDue_ = -1; readPage(); }
    if (outstanding_ > 0 && now_ - lastSendAt_ > kReplyTimeoutMs) outstanding_ = 0;
    render();
}

}  // namespace editor
