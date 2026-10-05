// The editor's screens (docs/editor/EDITOR_SPEC.md, "Page generation"): 4 rows of 50 character cells (two halves of
// 25), built from items (a name and its value text) in the OS's table order. Page types:
//  - two-column list: two fields per row, name left and value right in each 25-cell half; a run of identically named
//    fields is numbered 1, 2, ...; a field whose value does not fit a half takes the whole row;
//  - stage table: envelope fields named "... Time (n)" / "... Level (n)": stages across, time and level down;
//  - code grid: the OS's 4-character codes with their values, four per row;
//  - name list: names (e.g. presets), two per row, 8 per view.
// Value cells are underlined from their first to their last character; names never are.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "editor_tables.h"

namespace edpages {

constexpr int kRows = 4, kCols = 50, kHalf = 25;

struct Screen {
    std::array<std::array<char, kCols>, kRows> chars;
    std::array<std::array<bool, kCols>, kRows> underline{};
    std::array<std::array<bool, kCols>, kRows> dot{};
    struct Span { int row, col, len, ref; };         // a value's cells, and the item's reference
    std::vector<Span> values;
    Screen() { for (auto& r : chars) r.fill(' '); }
    std::string rowText(int r) const { return std::string(chars[size_t(r)].begin(), chars[size_t(r)].end()); }
};

struct Item {
    std::string name, value;
    int ref = -1;                                    // the caller's reference (e.g. field index); -1: name only
    int width = 0;                                   // cells reserved for the value (its widest form), so the layout
                                                     // does not move when the value changes; 0: the value's length
    size_t room() const { return std::max(value.size(), size_t(width < 0 ? 0 : width)); }
};

enum class RowKind { Pair, Full, Stage, Grid, Names };
struct Row {
    RowKind kind = RowKind::Pair;
    std::vector<Item> items;                         // Pair: 1-2, Full: 1, Stage: label item (value empty) + stages,
    std::string label;                               // Grid: up to 4, Names: up to 2
};
using Page = std::vector<Row>;                       // at most kRows rows

// Name shortening (only when a name does not fit `width`): split at word and camel-case boundaries, replace words from
// the fixed list (CUTOFF -> CUT, ...), then join word pairs from the end one at a time, then truncate, keeping a
// trailing column number. Names are shown in capitals.
std::string shortenName(const std::string& name, size_t width);

std::vector<Row> twoColumnRows(const std::vector<Item>& items);
// Envelope stage table: items whose names end in "Time (n)" / "Level (n)" become a header row and a time and a level
// row; `rest` receives the other items (in order).
std::vector<Row> stageRows(const std::vector<Item>& items, std::vector<Item>& rest);
bool isStageName(const std::string& name, bool& isTime, int& stage);
std::vector<Row> gridRows(const std::vector<Item>& items);
std::vector<Row> nameRows(const std::vector<Item>& items);
std::vector<Page> paginate(const std::vector<Row>& rows);
Screen render(const Page& page);

// ---- values
// The raw value of a field in its object's bytes (`obj` holds the object from byte `objOffset`), sign-extended for
// signed types; for a bit field the bits it covers; for the 4-byte value the long. False if the bytes are not there.
bool rawValue(const edtables::Field& f, const uint8_t* obj, size_t objSize, uint32_t objOffset, int64_t& v);
// Wave IDs as the OS lists them (entry n = ID of wave number n, 1-58) and optional names by number.
struct Waves {
    std::vector<uint32_t> ids;                       // index n-1 = wave number n; empty until read
    std::vector<std::string> names;                  // index n-1; empty strings for no name
    int numberOf(uint32_t id) const;                 // 0 if unknown
};
// The value as the OS would show it: value lists, Off/On, note names, signed with a sign, 1-based numbers.
std::string formatValue(const edtables::Field& f, const edtables::Table& t, int64_t v, const Waves* waves = nullptr);
std::string noteName(int note);                     // MIDI note number -> C-1 ... G9 (60 = C4)

// ---- sections (the legends of rows 2 and 3)
// The oscillator section a field belongs to, from its name: WAVE, PITCH, FILTER, AMP, NOISE, LFO, ENV1, ENV2, ENV3 or
// GEN. The sound area's: TRACK, RANGE or RECEIVE.
std::string oscillatorSection(const std::string& name);
std::string soundSection(const std::string& name);

}  // namespace edpages
