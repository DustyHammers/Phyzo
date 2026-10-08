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

std::string trimmed(const std::string& s) { return trim(s); }

std::string displayName(const std::string& name) {
    const std::string n = trim(name);
    auto up = [](char c) { return std::isupper(static_cast<unsigned char>(c)) != 0; };
    auto lo = [](char c) { return std::islower(static_cast<unsigned char>(c)) != 0; };
    auto dig = [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; };
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) {
        const char c = n[i];
        if (c == ' ') { if (!out.empty() && out.back() != ' ') out.push_back(' '); continue; }
        if (i > 0 && up(c)) {
            const char p = n[i - 1];
            const bool next = i + 1 < n.size() && lo(n[i + 1]);
            if (lo(p) || dig(p) || (up(p) && next)) if (!out.empty() && out.back() != ' ') out.push_back(' ');
        }
        out.push_back(c);
    }
    return out;
}

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

namespace {
bool tooLong(const Item& x) { return x.room() + 1 + 6 > size_t(kHalf); }   // a name needs at least 6 cells

// Fields laid out in order: runs of identical names numbered, two per row, too long a value on a row of its own.
void plainRows(const std::vector<Item>& items, std::vector<Row>& rows) {
    // Number runs of identically named fields 1, 2, ...
    std::vector<Item> it = items;
    for (size_t i = 0; i < it.size();) {
        size_t j = i + 1;
        while (j < it.size() && it[j].name == it[i].name) ++j;
        if (j - i > 1)
            for (size_t k = i; k < j; ++k) it[k].name += " " + std::to_string(k - i + 1);
        i = j;
    }
    // Two per row; a value too long for a half takes the whole row.
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
}

// The length of the longest block at `i` (at least 2 items) followed directly by a block with the same names in the
// same order; 0 if there is none.
size_t repeatedBlock(const std::vector<Item>& it, size_t i) {
    for (size_t n = (it.size() - i) / 2; n >= 2; --n) {
        bool same = true;
        for (size_t k = 0; k < n && same; ++k) same = it[i + k].name == it[i + n + k].name;
        if (same) return n;
    }
    return 0;
}
}  // namespace

std::vector<Row> twoColumnRows(const std::vector<Item>& items) {
    std::vector<Row> rows;
    std::vector<Item> plain;                         // fields before a repeated block, laid out as usual
    for (size_t i = 0; i < items.size();) {
        const size_t n = repeatedBlock(items, i);
        if (n == 0) { plain.push_back(items[i]); ++i; continue; }
        plainRows(plain, rows);
        plain.clear();
        for (size_t k = 0; k < n; ++k) {             // row by row: the first block left, the second right
            Item a = items[i + k], b = items[i + n + k];
            a.name += " 1";
            b.name += " 2";
            if (tooLong(a) || tooLong(b)) {
                for (const Item& x : {a, b}) {
                    Row r;
                    r.kind = tooLong(x) ? RowKind::Full : RowKind::Pair;
                    r.items.push_back(x);
                    rows.push_back(r);
                }
                continue;
            }
            Row r;
            r.kind = RowKind::Pair;
            r.items = {a, b};
            rows.push_back(r);
        }
        i += 2 * n;
    }
    plainRows(plain, rows);
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
    int slot = 4;
    for (Item x : items) {
        x.span = x.span == 2 ? 2 : 1;
        if (x.span == 2 && slot % 2) ++slot;         // a whole half
        if (slot + x.span > 4) {
            Row r;
            r.kind = RowKind::Grid;
            rows.push_back(r);
            slot = 0;
        }
        x.col = slot;
        slot += x.span;
        rows.back().items.push_back(x);
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
    // Underlined from the first to the last character shown, never over blank cells around it.
    int first = -1, last = -1;
    for (int i = 0; i < width && col + i < kCols; ++i)
        if (t[size_t(i)] != ' ') { if (first < 0) first = i; last = i; }
    if (first < 0) return;
    for (int i = first; i <= last; ++i) s.underline[size_t(row)][size_t(col + i)] = true;
    s.values.push_back({row, col + first, last - first + 1, ref});
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
        case RowKind::Pair:                          // each name takes what its half has left after its value and a space
            for (size_t k = 0; k < r.items.size() && k < 2; ++k)
                field(s, row, int(k) * kHalf, kHalf - 1, std::max(1, kHalf - 2 - int(r.items[k].room())), r.items[k]);
            break;
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
                const int slot = x.col >= 0 && x.col < 4 ? x.col : int(k);
                const int col = slot * 12 + int(slot > 1 ? 2 : 0);   // the halves' gap after the second code
                if (x.span == 2 && slot % 2 == 0) {  // a whole half: the name in full, the value up to the half's end
                    const std::string name = upper(trim(x.name)).substr(0, size_t(kHalf - 8));
                    const int at = col + std::max(5, int(name.size()) + 1);
                    put(s, row, col, name, int(name.size()), false, false, -1);
                    put(s, row, at, x.value, col + kHalf - 1 - at, false, x.ref >= 0, x.ref);
                    continue;
                }
                put(s, row, col, upper(trim(x.name)).substr(0, 4), 4, false, false, -1);
                put(s, row, col + 5, x.value, 7, false, x.ref >= 0, x.ref);
            }
            break;
        case RowKind::Names:
            for (size_t k = 0; k < r.items.size() && k < 2; ++k) {
                const Item& x = r.items[k];
                const int col = int(k) * kHalf;
                put(s, row, col, x.name, 3, false, false, -1);   // the number
                put(s, row, col + 4, x.value, 16, false, false, -1);   // names are never underlined
                if (x.ref >= 0) s.values.push_back({row, col, 4 + 16, x.ref, true});   // the number and name pick it
            }
            break;
        case RowKind::Fixed:
            for (const Item& x : r.items) {
                if (x.col < 0 || x.col >= kCols) continue;
                const std::string name = upper(trim(x.name));
                put(s, row, x.col, name, int(name.size()), false, false, -1);
                const int at = x.col + (name.empty() ? 0 : int(name.size()) + 1);
                if (x.ref >= 0 && at < kCols) put(s, row, at, x.value, std::min(int(x.room()), kCols - at), false, true, x.ref);
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
    case kOffsetByte: v = int64_t(b) - 64; return true;   // stored = shown + 64
    case kLong: v = int64_t(uint32_t(obj[at]) << 24 | uint32_t(obj[at + 1]) << 16 | uint32_t(obj[at + 2]) << 8 | obj[at + 3]); return true;
    case kBitField: {                                // the record's min: mask << 8 | bit (the mask is 1 for flags)
        const int mask = (uint16_t(f.min) >> 8) & 0xFF, bit = uint16_t(f.min) & 0xFF;
        v = bit <= 7 ? (b >> bit) & mask : b;
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
    const bool sign = f.display == Display::Formatter || (f.format & kFmtSigned) || f.min < 0 || f.type == kOffsetByte;
    if (f.display == Display::Number) {
        if (f.format == kFmtOffOn) return v ? "On" : "Off";
        if (f.format == kFmtNote) return noteName(int(v));
        if (f.format == kFmtOneBased) return std::to_string(v + 1);
        if (f.format == kFmtThreeDigits) {
            char b[24];
            std::snprintf(b, sizeof b, "%03lld", static_cast<long long>(v));
            return b;
        }
    }
    if (sign && v > 0) return "+" + std::to_string(v);
    return std::to_string(v);
}

// ------------------------------------------------------------------ sections

const char* oscillatorSection(int index) {
    struct Range { int from, to; const char* section; };
    static const Range kMap[] = {
        {0, 0, ""},        {1, 11, "GEN"},     {12, 13, "AMP"},    {14, 18, "PITCH"},  {19, 20, "GEN"},
        {21, 22, ""},      {23, 28, "PITCH"},  {29, 29, ""},       {30, 38, "FILTER"}, {39, 40, ""},
        {41, 47, "FILTER"}, {48, 50, ""},      {51, 51, "WAVE"},   {52, 52, ""},       {53, 58, "WAVE"},
        {59, 60, ""},      {61, 67, "AMP"},    {68, 69, ""},       {70, 70, "NOISE"},  {71, 73, ""},
        {74, 74, "PITCH"}, {75, 75, "GEN"},    {76, 76, ""},       {77, 77, "NOISE"},  {78, 92, "ENV1"},
        {93, 93, ""},      {94, 108, "ENV2"},  {109, 109, ""},     {110, 124, "ENV3"}, {125, 125, ""},
        {126, 136, "LFO"}, {137, 137, ""}};
    for (const Range& r : kMap) if (index >= r.from && index <= r.to) return r.section;
    return nullptr;
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
