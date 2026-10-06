// The OS's parameter tables, read at run time from the user's OS image (never copied into the repo). Layouts from our
// own analysis (docs/editor/PR_B_ANSWERS.md): each table is reached through a descriptor of 8 big-endian longs
// (field records, names array, offsets array, value-list array, -, count << 16 | flags, RAM copy, -); an array is a
// long count followed by that many longs.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace edtables {

// Descriptors in the supported OS image (load address 0x4000).
constexpr uint32_t kOscillatorTable = 0x731F8;      // offsets into the oscillator sub-object (type 1, sub 2)
constexpr uint32_t kSoundSettingsTable = 0x72064;   // offsets into the track record (type 2, number 0x3FFF, sub 1)
constexpr uint32_t kSoundCommonTable = 0x731DC;     // offsets into the sound object (type 1)
constexpr uint32_t kSystemTable = 0x75528;          // offsets into the system object (type 0)
constexpr uint32_t kImageBase = 0x4000;
// The OS's wave list in RAM, built at boot: entry n (1-58) is the 4-byte wave ID at kWaveList + 4n.
constexpr uint32_t kWaveList = 0x0BE06F4C;
constexpr int kWaves = 58;

// Value types (field record bytes 2-3)
enum ValueType : uint16_t {
    kUnsignedByte = 0, kSignedByte = 1, kOffsetByte = 2 /* stored = shown + 64 */, kBitField = 3, kLong = 4, kSmallUnsigned = 5, kSignedFormatted = 6,
    kSignedListMinusOne = 7
};
// How a value is shown (the field record's sub-records)
enum class Display { Number, List, Formatter };
// Number format flags (kind 1 / kind 9 sub-records)
constexpr uint16_t kFmtSigned = 0x2000, kFmtOffOn = 0x0002, kFmtNote = 0x0003, kFmtOneBased = 0x0001, kFmtOneBasedB = 0xC000;

struct ValueList {
    bool hasFirst = false;                           // the list carries the value of its first entry (header 05 hi lo)
    int first = 0;
    std::vector<std::string> entries;
};

struct Field {
    int index = 0;                                   // position in the OS table
    std::string name;
    uint32_t offset = 0;                             // byte offset in the table's object
    uint8_t flags = 0;                               // record header flags
    uint16_t type = 0;
    int16_t min = 0, max = 0;
    Display display = Display::Number;
    uint16_t format = 0;                             // number format flags
    int32_t param = -1;                              // parameter number (oscillator: the NRPN parameter)
    int listIndex = -1;                              // into Table::lists (Display::List)
    int formatter = -1;                              // built-in formatter number (Display::Formatter)
    bool hidden = false;                             // not shown (docs/editor/PR_B_ANSWERS.md 1.2)
    int size() const { return type == kLong ? 4 : 1; }
    bool isSigned() const { return type == kSignedByte || type == kOffsetByte || type == kSignedFormatted || type == kSignedListMinusOne || min < 0; }
};

struct Table {
    std::string id;
    uint32_t descriptor = 0;
    uint16_t descriptorFlags = 0;
    std::vector<Field> fields;
    std::vector<ValueList> lists;
};

// Reads the table whose descriptor is at `descriptor` from `image` (bytes loading at kImageBase). False with `err`
// (and as much as could be read) if a pointer or record falls outside the image or is malformed.
bool readTable(const std::vector<uint8_t>& image, uint32_t descriptor, const std::string& id, Table& out, std::string& err);
// One value list at `address` (list string: optional 05 hi lo header, entries separated by CR, ends at NUL; bytes
// 11-1F copy that many characters minus 0x10 from the previous entry, 10 copies the rest of it and ends the entry; a
// 0x80 marker byte is ignored).
ValueList decodeList(const std::vector<uint8_t>& image, uint32_t address);
// Hidden fields: names containing SPARE, the wave class / wave checksum / program class fields, "Use Layer",
// "Old Keybd Track", the sound-finder fields, unused system codes ("????"). Compared without case, spaces or
// punctuation.
bool isHiddenName(const std::string& name);

struct OsTables {
    Table oscillator, soundSettings, soundCommon, system;
    bool ok = false;
    std::string error;
};
OsTables readAll(const std::vector<uint8_t>& image);

}  // namespace edtables
