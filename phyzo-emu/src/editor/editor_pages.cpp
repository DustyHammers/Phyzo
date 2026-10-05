#include "editor_pages.h"
#include <algorithm>
#include <cctype>
#include <cstdio>

namespace edpages {

namespace {
std::string upper(std::string s) {
    for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(' ');
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(' ') - a + 1);
}
bool contains(const std::string& hay, const char* needle) { return hay.find(needle) != std::string::npos; }

const std::pair<const char*, const char*> kShortWords[] = {
    {"CUTOFF", "CUT"},   {"KEYBD", "KBD"},        {"TRACK", "TRK"},       {"BREAKPOINT", "BRKPT"}, {"RESONANCE", "RES"},
    {"AMOUNT", "AMT"},   {"MODULATION", "MOD"},   {"VELOCITY", "VEL"},    {"ATTACK", "ATK"},       {"RELEASE", "REL"},
    {"LEVEL", "LVL"},    {"SCALING", "SCALE"},    {"SEMITONE", "SEMI"},   {"TUNING", "TUNE"},      {"PRESSURE", "PRESS"},
    {"VOLUME", "VOL"},   {"DELAY", "DLY"},        {"ENVELOPE", "ENV"},    {"FEEDBACK", "FDBK"},    {"DAMPING", "DAMP"},
    {"SOURCE", "SRC"}};

// Words at spaces and camel-case boundaries (a lower-case letter followed by a capital).
std::vector<std::string> words(const std::string& name) {
    std::vector<std::string> w;
    std::string cur;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        if (c == ' ' || c == '_') { if (!cur.empty()) w.push_back(cur); cur.clear(); continue; }
        if (!cur.empty() && std::isupper(static_cast<unsigned char>(c)) && std::islower(static_cast<unsigned char>(cur.back()))) {
            w.push_back(cur);
            cur.clear();
        }
        cur.push_back(c);
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}
bool isNumberWord(const std::string& w) {
    std::string s = w;
    if (s.size() >= 3 && s.front() == '(' && s.back() == ')') s = s.substr(1, s.size() - 2);
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
}
std::string joinWords(const std::vector<std::string>& w, const std::vector<bool>& joined) {
    std::string s;
    for (size_t i = 0; i < w.size(); ++i) {
        if (i > 0 && !joined[i]) s.push_back(' ');
        s += w[i];
    }
    return s;
}
std::string fit(const std::string& s, size_t width, bool right) {
    if (s.size() >= width) return s.substr(0, width);
    return right ? std::string(width - s.size(), ' ') + s : s + std::string(width - s.size(), ' ');
}
}  // namespace

std::string shortenName(const std::string& name, size_t width) {
    std::string n = upper(trim(name));
    if (n.size() <= width) return n;
    std::vector<std::string> w = words(trim(name));
    for (auto& x : w) x = upper(x);
    std::string number;                              // a trailing column number is kept to the end
    if (w.size() > 1 && isNumberWord(w.back())) { number = w.back(); w.pop_back(); }
    auto with = [&](const std::string& core) { return number.empty() ? core : core + " " + number; };
    std::vector<bool> joined(w.size(), false);
    if (with(joinWords(w, joined)).size() <= width) return with(joinWords(w, joined));
    for (auto& x : w)
        for (const auto& r : kShortWords)
            if (x == r.first) { x = r.second; break; }
    if (with(joinWords(w, joined)).size() <= width) return with(joinWords(w, joined));
    for (size_t i = w.size(); i-- > 1;) {            // join word pairs from the end, one space at a time
        joined[i] = true;
        if (with(joinWords(w, joined)).size() <= width) return with(joinWords(w, joined));
    }
    const std::string core = joinWords(w, joined);
    if (number.empty()) return core.substr(0, width);
    const size_t room = width > number.size() + 1 ? width - number.size() - 1 : 0;
    return room ? core.substr(0, room) + " " + number : number.substr(0, width);
}

std::vector<Row> twoColumnRows(const std::vector<Item>& items) {
    // Number runs of identically named fields 1, 2, ...
    std::vector<Item> it = items;
    for (size_t i = 0; i < it.size();) {
        size_t j = i + 1;
        while (j < it.size() && it[j].name == it[i].name) ++j;
        if (j - i > 1)
            for (size_t k = i; k < j; ++k) it[k].name += " " + std::to_string(k - i + 1);
        i = j;
    }
    // Two per row; a value too long for a half (name needs at least 6 cells) takes the whole row.
    std::vector<Row> rows;
    const auto tooLong = [](const Item& x) { return x.room() + 1 + 6 > size_t(kHalf); };
    for (size_t i = 0; i < it.size();) {
        if (tooLong(it[i]) || i + 1 == it.size() || tooLong(it[i + 1])) {
            Row r;
            r.kind = tooLong(it[i]) ? RowKind::Full : RowKind::Pair;
            r.items.push_back(it[i]);
            rows.push_back(r);
            ++i;
            continue;
        }
        Row r;
        r.kind = RowKind::Pair;
        r.items = {it[i], it[i + 1]};
        rows.push_back(r);
        i += 2;
    }
    return rows;
}

bool isStageName(const std::string& name, bool& isTime, int& stage) {
    const std::string s = lower(trim(name));
    size_t end = s.size();
    if (end && s[end - 1] == ')') --end;
    size_t d = end;
    while (d > 0 && std::isdigit(static_cast<unsigned char>(s[d - 1]))) --d;
    if (d == end) return false;
    stage = std::stoi(s.substr(d, end - d));
    size_t p = d;
    if (p > 0 && s[p - 1] == '(') --p;
    while (p > 0 && s[p - 1] == ' ') --p;
    const std::string head = s.substr(0, p);
    auto endsWith = [&](const char* w) { const std::string x(w); return head.size() >= x.size() && head.compare(head.size() - x.size(), x.size(), x) == 0; };
    if (endsWith("time")) { isTime = true; return true; }
    if (endsWith("level")) { isTime = false; return true; }
    return false;
}

std::vector<Row> stageRows(const std::vector<Item>& items, std::vector<Item>& rest) {
    std::vector<std::pair<int, Item>> times, levels;
    rest.clear();
    for (const Item& x : items) {
        bool t; int st;
        if (isStageName(x.name, t, st)) (t ? times : levels).push_back({st, x});
        else rest.push_back(x);
    }
    if (times.empty() && levels.empty()) return {};
    int stages = 0;
    for (auto& p : times) stages = std::max(stages, p.first);
    for (auto& p : levels) stages = std::max(stages, p.first);
    Row head, t, l;
    head.kind = t.kind = l.kind = RowKind::Stage;
    head.label = "STAGE"; t.label = "TIME"; l.label = "LEVEL";
    for (int s = 1; s <= stages; ++s) {
        head.items.push_back({"", std::to_string(s), -1});
        Item ti{"", "", -1}, li{"", "", -1};
        for (auto& p : times) if (p.first == s) ti = p.second;
        for (auto& p : levels) if (p.first == s) li = p.second;
        t.items.push_back(ti);
        l.items.push_back(li);
    }
    std::vector<Row> rows = {head};
    if (!times.empty()) rows.push_back(t);
    if (!levels.empty()) rows.push_back(l);
    return rows;
}

std::vector<Row> gridRows(const std::vector<Item>& items) {
    std::vector<Row> rows;
    for (size_t i = 0; i < items.size(); i += 4) {
        Row r;
        r.kind = RowKind::Grid;
        r.items.assign(items.begin() + long(i), items.begin() + long(std::min(items.size(), i + 4)));
        rows.push_back(r);
    }
    return rows;
}

std::vector<Row> nameRows(const std::vector<Item>& items) {
    std::vector<Row> rows;
    for (size_t i = 0; i < items.size(); i += 2) {
        Row r;
        r.kind = RowKind::Names;
        r.items.assign(items.begin() + long(i), items.begin() + long(std::min(items.size(), i + 2)));
        rows.push_back(r);
    }
    return rows;
}

std::vector<Page> paginate(const std::vector<Row>& rows) {
    std::vector<Page> pages;
    for (size_t i = 0; i < rows.size(); i += kRows)
        pages.emplace_back(rows.begin() + long(i), rows.begin() + long(std::min(rows.size(), i + kRows)));
    if (pages.empty()) pages.emplace_back();
    return pages;
}

namespace {
void put(Screen& s, int row, int col, const std::string& text, int width, bool right, bool value, int ref) {
    const std::string t = fit(text, size_t(width), right);
    for (int i = 0; i < width && col + i < kCols; ++i) s.chars[size_t(row)][size_t(col + i)] = t[size_t(i)];
    if (!value || text.empty()) return;
    const int len = std::min<int>(int(text.size()), width);
    const int first = right ? col + width - len : col;
    for (int i = 0; i < len; ++i) s.underline[size_t(row)][size_t(first + i)] = true;
    s.values.push_back({row, first, len, ref});
}
// A name and its value in `width` cells; `nameRoom` cells for the name; the value right-aligned.
void field(Screen& s, int row, int col, int width, int nameRoom, const Item& x) {
    put(s, row, col, shortenName(x.name, size_t(nameRoom)), nameRoom, false, false, -1);
    if (x.ref >= 0 || !x.value.empty()) put(s, row, col + width - int(std::min<size_t>(x.value.size(), size_t(width))), x.value,
                                            int(std::min<size_t>(x.value.size(), size_t(width))), true, x.ref >= 0, x.ref);
}
}  // namespace

Screen render(const Page& page) {
    Screen s;
    for (size_t ri = 0; ri < page.size() && ri < size_t(kRows); ++ri) {
        const Row& r = page[ri];
        const int row = int(ri);
        switch (r.kind) {
        case RowKind::Full:
            if (!r.items.empty()) field(s, row, 0, kCols, kCols - 1 - int(r.items[0].room()), r.items[0]);
            break;
        case RowKind::Pair: {
            size_t longest = 0;                      // both halves get the same room for their names
            for (const Item& x : r.items) longest = std::max(longest, x.room());
            const int room = std::max(1, kHalf - 2 - int(longest));
            for (size_t k = 0; k < r.items.size() && k < 2; ++k)
                field(s, row, int(k) * kHalf, kHalf - 1, room, r.items[k]);
            break;
        }
        case RowKind::Stage: {
            const int label = 8, n = std::max<int>(1, int(r.items.size()));
            const int colW = (kCols - label) / n;
            put(s, row, 0, r.label, label, false, false, -1);
            for (int k = 0; k < int(r.items.size()); ++k) {
                const Item& x = r.items[size_t(k)];
                put(s, row, label + k * colW, x.value, colW - 1, true, x.ref >= 0, x.ref);
            }
            break;
        }
        case RowKind::Grid:
            for (size_t k = 0; k < r.items.size() && k < 4; ++k) {
                const Item& x = r.items[k];
                const int col = int(k) * 12 + int(k > 1 ? 2 : 0);   // the halves' gap after the second code
                put(s, row, col, x.name.substr(0, 4), 4, false, false, -1);
                put(s, row, col + 5, x.value, 7, false, x.ref >= 0, x.ref);
            }
            break;
        case RowKind::Names:
            for (size_t k = 0; k < r.items.size() && k < 2; ++k) {
                const Item& x = r.items[k];
                const int col = int(k) * kHalf;
                put(s, row, col, x.name, 3, false, false, -1);   // the number
                put(s, row, col + 4, x.value, 16, false, false, -1);   // names are never underlined
            }
            break;
        }
    }
    return s;
}

// ------------------------------------------------------------------ values

bool rawValue(const edtables::Field& f, const uint8_t* obj, size_t objSize, uint32_t objOffset, int64_t& v) {
    using namespace edtables;
    if (f.offset < objOffset) return false;
    const size_t at = f.offset - objOffset;
    if (at + size_t(f.size()) > objSize) return false;
    const uint8_t b = obj[at];
    switch (f.type) {
    case kSignedByte: case kSignedFormatted: case kSignedListMinusOne: v = int8_t(b); return true;
    case kLong: v = int64_t(uint32_t(obj[at]) << 24 | uint32_t(obj[at + 1]) << 16 | uint32_t(obj[at + 2]) << 8 | obj[at + 3]); return true;
    case kBitField: {
        const int width = (uint16_t(f.min) >> 8) & 0xFF, bit = uint16_t(f.min) & 0xFF;
        if (width >= 1 && width <= 8 && bit + width <= 8) { v = (b >> bit) & ((1 << width) - 1); return true; }
        v = b;                                       // not a bit position (one oscillator field): the byte
        return true;
    }
    default: v = f.min < 0 ? int64_t(int8_t(b)) : int64_t(b); return true;
    }
}

int Waves::numberOf(uint32_t id) const {
    for (size_t i = 0; i < ids.size(); ++i) if (ids[i] == id && id != 0) return int(i) + 1;
    return 0;
}

std::string noteName(int note) {
    static const char* const kNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    if (note < 0 || note > 127) return std::to_string(note);
    return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

std::string formatValue(const edtables::Field& f, const edtables::Table& t, int64_t v, const Waves* waves) {
    using namespace edtables;
    if (f.type == kLong) {                           // the wave ID: the wave number, and a name if the user has one
        if (!waves) return "?";
        const int n = waves->numberOf(uint32_t(v));
        if (n == 0) return waves->ids.empty() ? "..." : "?";
        const std::string& nm = size_t(n - 1) < waves->names.size() ? waves->names[size_t(n - 1)] : std::string();
        return nm.empty() ? std::to_string(n) : std::to_string(n) + " " + nm;
    }
    if (f.display == Display::List && f.listIndex >= 0 && size_t(f.listIndex) < t.lists.size()) {
        const ValueList& l = t.lists[size_t(f.listIndex)];
        const int first = l.hasFirst ? l.first : (f.type == kBitField ? 0 : (f.type == kSignedListMinusOne ? -1 : std::min<int>(0, f.min)));
        const int64_t i = v - first;
        if (i >= 0 && size_t(i) < l.entries.size()) return trim(l.entries[size_t(i)]);
        return std::to_string(v);                    // past the list's end: a number
    }
    const bool sign = f.display == Display::Formatter || (f.format & kFmtSigned) || f.min < 0;
    if (f.display == Display::Number) {
        if (f.format == kFmtOffOn) return v ? "On" : "Off";
        if (f.format == kFmtNote) return noteName(int(v));
        if (f.format == kFmtOneBased || f.format == kFmtOneBasedB) return std::to_string(v + 1);
    }
    if (sign && v > 0) return "+" + std::to_string(v);
    return std::to_string(v);
}

// ------------------------------------------------------------------ sections

std::string oscillatorSection(const std::string& name) {
    const std::string s = lower(name);
    for (int e = 1; e <= 3; ++e) {
        const std::string n = std::to_string(e);
        if (contains(s, ("env " + n).c_str()) || contains(s, ("env" + n).c_str()) || contains(s, ("envelope " + n).c_str()))
            return "ENV" + n;
    }
    if (contains(s, "lfo")) return "LFO";
    if (contains(s, "noise")) return "NOISE";
    if (contains(s, "filter") || contains(s, "cutoff") || contains(s, "reson") || contains(s, "flt")) return "FILTER";
    if (contains(s, "pitch") || contains(s, "tune") || contains(s, "transp") || contains(s, "octave") ||
        contains(s, "semi") || contains(s, "glide") || contains(s, "porta") || contains(s, "bend"))
        return "PITCH";
    if (contains(s, "wave")) return "WAVE";
    if (contains(s, "amp") || contains(s, "volume") || contains(s, "pan")) return "AMP";
    return "GEN";
}

std::string soundSection(const std::string& name) {
    const std::string s = lower(name);
    if (contains(s, "rcv") || contains(s, "receive") || contains(s, "recv") || contains(s, "rx ")) return "RECEIVE";
    if (contains(s, "low") || contains(s, "high") || contains(s, "range") || contains(s, "lo ") || contains(s, "hi ") ||
        contains(s, "limit"))
        return "RANGE";
    return "TRACK";
}

}  // namespace edpages
