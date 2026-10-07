// The OS's parameter tables, read at run time from the user's OS image (never copied into the repo). Layouts from our
// own analysis (docs/editor/PR_B_ANSWERS.md): each table is reached through a descriptor of 8 big-endian longs
// (field records, names array, offsets array, value-list array, -, count << 16 | flags, RAM copy, -); an array is a
// long count followed by that many longs.
#pragma once
#include <array>
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
// Number format flags (kind 1 / kind 9 sub-records). 0x0001: shown 1-based. 0xC000: three digits, zero-padded, as
// stored (0-based; the OS shows its system field id, stored 0, as "000").
constexpr uint16_t kFmtSigned = 0x2000, kFmtOffOn = 0x0002, kFmtNote = 0x0003, kFmtOneBased = 0x0001, kFmtThreeDigits = 0xC000;

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

// The arp page's texts (docs/editor/PR_B_ANSWERS.md section 4): there is no arp field table; the OS's display code
// uses these NUL-terminated strings directly. Codes are the OS's 4-character labels.
namespace arpaddr {
constexpr uint32_t kTempoFmt = 0x88812, kRangeFmt = 0x88818, kResolutionCode = 0x8881E, kOn = 0x8882E, kOff = 0x88834,
                   kHold = 0x88850, kCentre = 0x88856, kLow = 0x8885C, kHigh = 0x88868, kDuration = 0x8886E,
                   kLimit = 0x8887C, kPresetCode = 0x88882, kOwnSettings = 0x88888, kFeel = 0x88892, kTouch = 0x88898,
                   kArpPresetFmt = 0x8889E;
constexpr uint32_t kIntervals[5] = {0x88828, 0x8883A, 0x8883E, 0x88844, 0x8884A};   // -3rd 3rd 4th 5th Oct
constexpr uint32_t kResolutionNames = 0x8876C;      // 12 pointers to the note-resolution names
}  // namespace arpaddr
struct ArpText {
    bool ok = false;
    std::string on, off, hold, centre, low, high, duration, limit, feel, touch, resolutionCode, tempoFmt, rangeFmt;
    std::string presetCode, ownSettings, arpPresetFmt;   // PrSt, " Snd" (the preset's own arp settings), "AP%02d"
    std::array<std::string, 5> intervals;
    std::array<std::string, 12> resolution;          // in the OS's pointer-table order
};
ArpText readArpText(const std::vector<uint8_t>& image);
// A number through one of the OS's print formats: %d, %<width>d, %0<width>d (also with the l length modifier) and %%
// only (anything else is copied as text).
std::string formatNumber(const std::string& fmt, int v);

// ---- effects (docs/editor/EFFECTS_EDITOR_SPEC.md, docs/editor/ROUND3_ANSWERS.md)
namespace fxaddr {
constexpr uint32_t kInsertAlgos = 0xBE080;          // 41 pointers to the insert algorithms' headers
constexpr uint32_t kRoutingAlgo = 0xBE078;          // pointer to the routing shell's header
constexpr uint32_t kReverbAlgo = 0xBE128;           // pointer to the reverb's header
constexpr uint32_t kFamilies = 0xAC868;             // 32 pointers to family descriptors (0: none), by PID >> 8
constexpr uint32_t kRecordSizes = 0x71418;          // record walker sizes: header, sections 0-7 (0xFF: length byte)
constexpr uint32_t kDisplaySizes = 0x71424;         // display section walker sizes: header, sub-blocks 0-4
constexpr uint32_t kEditOrder = 0xBF040;            // pointers to edit-order maps, 0-terminated
constexpr uint32_t kCodes = 0x89390;                // the algorithms' panel codes, 5 bytes each
constexpr uint32_t kPreset = 0x8945E, kInsertVariationFmt = 0x89464, kReverbVariationFmt = 0x8946C;
constexpr uint32_t kPrefixList = 0xBEA58, kSuffixList = 0xBEA5C;   // pointers to value lists
constexpr uint32_t kSyncNames = 0xBE288;            // 11 pointers to the tempo-sync value names
constexpr uint32_t kMsFormat = 0x124A0;             // formatters 80 / 81: the milliseconds format
constexpr uint32_t kModDescriptor = 0x720B8;        // the insert modulation's descriptor (records etc. in RAM)
constexpr uint32_t kDataImage = 0xBD4A8, kDataRam = 0x0BE00000;   // the initialised data: image copy of RAM
constexpr int kInsertCount = 41, kFamilyCount = 32, kSyncCount = 11;
}  // namespace fxaddr

// An effect parameter's definition (a sectioned field record of its family). All effect values are 16-bit signed.
struct FxParam {
    bool found = false;                              // a record with this PID exists
    uint16_t pid = 0;
    std::string name;
    int16_t min = 0, max = 0;
    bool hasList = false;
    std::vector<std::string> list;                   // list: entry = the raw value's index
    int formatter = -1;                              // 80 tempo sync / ms, 81 ms, 82 insert slot name
    uint8_t prefix = 0, suffix = 0;                  // indexes + 1 into FxTables::prefixes / suffixes (0: none)
    int decimals = 0;
    bool hidden() const { return pid == 0x0007; }    // a placeholder slot
    bool tempoSync = false;                          // formatter 80, or a list starting with the 11 tempo-sync names
};
struct FxAlgo {
    bool ok = false;
    std::string name;                                // empty: none stored (use the panel code)
    uint16_t family = 0, sub = 0;
    std::vector<FxParam> params;                     // by slot k
    std::vector<int> order;                          // the visible slots in the OS's edit order
    std::vector<std::string> variations;             // variation names (16 characters, '~' shown as a space)
};
struct FxTables {
    bool ok = false;
    std::string error;
    std::vector<FxAlgo> inserts;                     // by algorithm index
    FxAlgo routing, reverb;
    std::vector<std::string> codes;                  // the insert algorithms' panel codes, by index
    std::string preset, insertVariationFmt, reverbVariationFmt, msFormat;
    std::vector<std::string> syncNames, prefixes, suffixes;
    // The insert modulation's fields in record order, with their offsets from preset byte 64.
    std::vector<FxParam> mod;
    std::vector<uint32_t> modOffsets;
};
FxTables readEffects(const std::vector<uint8_t>& image);
// A value as the OS shows it: list entry, tempo-sync name or milliseconds, the insert slot's name (formatter 82, from
// `insert`), or the number with its decimals, prefix and suffix.
std::string formatEffect(const FxParam& p, int v, const FxTables& t, const FxAlgo* insert);

struct OsTables {
    Table oscillator, soundSettings, soundCommon, system;
    ArpText arp;                                     // the arp page is left out when !arp.ok
    FxTables fx;                                     // the INSERT and REVERB pages are left out when !fx.ok
    bool ok = false;
    std::string error;
};
OsTables readAll(const std::vector<uint8_t>& image);

}  // namespace edtables
