// The preset object's fields for the COMMON and ARP areas (docs/editor/EDITOR_ROUND2.md sections 2 and 3). The preset
// object has no field table in the OS, so these come from our own analysis: offsets, sizes and encodings. Labels are
// neutral descriptions, the OS's own 4-character codes (arp, read from the user's OS at run time) or the skin's knob
// labels. Each field has an edit domain (the numbers a drag steps through) and converts between it and the stored
// bytes.
//
// ARP values are read from the live arp state (64 bytes in RAM, the layout of the preset's arp block, preset offsets
// 350-413): the panel's arp edits change only the live state. Live byte N = preset offset 350 + N. Arp on/off and
// keyboard come from the OS's runtime flags, the arp preset from the OS's current arp preset. ARP settings are changed
// only through the panel's own buttons (docs/editor/ROUND2_REVIEW.md): the arp control's button (Tempo, Range or Arp
// Mode, steered by the OS's arp-control variable), then -/No and +/Yes; on/off and keyboard by their own buttons.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "editor_tables.h"

namespace edfields {

constexpr uint32_t kPresetSize = 414;
constexpr uint32_t kArpBlock = 350, kArpBlockSize = 64;   // the preset's arp block (preset offsets 350-413)
constexpr uint32_t kLiveArp = 0x0BE093D0;                  // the live arp state in RAM (raw-memory read)
constexpr uint32_t kArpRecordBody = 24;                    // an arp preset record's body: record offsets 24-87
// Live bytes kept when an arp preset is loaded: held, keyboard, both key-range pairs.
constexpr uint32_t kKeptOnLoad[] = {0x01, 0x02, 0x14, 0x15, 0x16, 0x17};
constexpr int64_t kNone16 = 0xFFFF;                         // feel off, no pattern, no riff
// The OS's arp variables in RAM (raw-memory reads)
constexpr uint32_t kArpControlVar = 0x0BE01877;            // the arp control the panel shows: 0 Tempo, 1 Range, 2 PrSt ...
constexpr uint32_t kCurrentArpPreset = 0x0BE01880;         // 32-bit: -1 = the preset's own settings, else the arp preset
constexpr uint32_t kArpFlags = 0x0BE018BA;                 // 2 bytes: arp on, keyboard
// The OS's panel mode: 0x01 normal, 0x02 sound, 0x05 effects, 0x0C the arp menu (Arp Mode, Tempo, Range). The
// arp-control variable keeps its value in every mode (it reads 2 in normal mode), so -/No and +/Yes are pressed only
// when this reads 0x0C as well.
constexpr uint32_t kPanelMode = 0x0BE01A8B;
constexpr int kModeNormal = 0x01, kModeArpMenu = 0x0C;
// Panel buttons (raw ids)
constexpr int kBtnTempo = 0x1E, kBtnRange = 0x03, kBtnArpMode = 0x21, kBtnYes = 0x0D, kBtnNo = 0x0E,
              kBtnArpOnOff = 0x06, kBtnArpKeyboard = 0x05;
// The preset bank button: pressed once in the arp menu (mode 0x0C) it returns the panel to normal mode (0x01) with the
// display and LEDs as before; in normal mode it switches the bank, so it is pressed only when the mode reads 0x0C.
constexpr int kBtnBank = 0x19;
// The note resolutions (ticks, 384 per quarter) in the order of the OS's name pointer table.
constexpr uint32_t kResolutionTicks[12] = {1536, 768, 384, 192, 96, 48, 32, 64, 128, 256, 512, 1024};

enum class Kind {
    Number,       // plain number
    List,         // index into `list`
    Note,         // MIDI note number, shown as a note name
    OnOff,        // stored 0 / `onRaw`, shown OFF / ON
    Inverted,     // stored 0 = ON, 1 = OFF (the arp's keyboard)
    Interval,     // OnOff, at most one of the five on: setting one clears the others
    OctaveUp,     // stored 0, 12, ... 48, shown through the OS's range format with value / 12
    OctaveDown,   // stored 0, -12, ... -48 (signed), shown with -value / 12
    Touch,        // 0 = OFF, else the number
    Duration,     // stored 1-255, shown raw x 100 / 256
    Resolution,   // 4-byte tick count, shown by the OS's name
    Tempo,        // 4-byte number, the OS's tempo format
    Feel,         // 16-bit, 0xFFFF = OFF (domain -1), else the number (and the feel's name, `nameType`)
    RuntimeFlag,  // the OS's arp on / keyboard flag (shown 0 / 1), switched by its panel button
    Pattern,      // 16-bit number of an OS object (a note pattern or a riff, `nameType`), 0xFFFF = none, shown by its
                  // name (display only)
    Text,         // characters (display only)
    Signed16,     // 16-bit signed number (display only)
    PresetAction  // the OS's current arp preset: -1 = " Snd" (the preset's own settings), else its number (from 0, as
                  // the OS counts); the arp presets end where the OS stops answering their name reads
};

struct Field {
    std::string label;
    char area = 'm';                                 // 'm' COMMON, 'a' ARP
    std::string section;                             // COMMON: SLOTS or KNOBS
    int span = 1;                                    // ARP: the grid columns it takes (1 or 2)
    int slot = -1;                                   // COMMON SLOTS: the sound slot 0-3
    uint32_t offset = 0;                             // preset offset
    int size = 1;
    Kind kind = Kind::Number;
    int lo = 0, hi = 127;                            // the edit domain
    int onRaw = 1;                                   // OnOff: the stored value for ON
    std::vector<std::string> list;                   // List
    int nrpnLsb = -1;                                // COMMON: NRPN MSB 1, this LSB
    uint32_t mirror = 0;                             // ARP: the second key-range pair (the OS writes both)
    int arpControl = -1;                             // ARP: the OS's arp-control number (0 Tempo, 1 Range, 2 PrSt ...)
    int button = -1;                                 // ARP on/off and keyboard: the panel button that switches it
    int flag = -1;                                   // RuntimeFlag: 0 arp on, 1 keyboard
    int nameType = -1;                               // Pattern, Feel: the OS object type of the named number (9 note
                                                     // pattern, 10 feel, 11 riff), read with a partial read at offset 8
    bool editable = false;
    bool live() const {
        return area == 'a' && kind != Kind::RuntimeFlag && kind != Kind::PresetAction && offset >= kArpBlock &&
               offset + uint32_t(size) <= kArpBlock + kArpBlockSize;
    }
};

// COMMON: the four sound slots (mode, oscillators, key range), the five performance-knob values (labels from the
// skin, perf1-perf5). (The insert modulation is on INSERT > MOD, docs/editor/EFFECTS_EDITOR_SPEC.md.)
std::vector<Field> commonFields(const std::array<std::string, 5>& knobLabels);
// ARP, in page order (owner-approved mockup, v0.8.3.5): the arp controls in the order the panel's Arp Mode button
// steps through them (PrSt, nrES, ndur, FEEL, the intervals, Lo n, Hi n, tch, CEnt, ntLt, HELd), with the pattern and
// the riff right after the preset's row; then arp on/off, keyboard, tempo, range up and down, and the arp settings
// name. The arp preset, the pattern, the riff and the feel take two grid columns.
std::vector<Field> arpFields(const edtables::ArpText& t);

// The stored value (big-endian; signed for OctaveDown and Signed16) from `bytes`, which hold the object from offset
// `base`. False if the bytes are not there.
bool rawOf(const Field& f, const uint8_t* bytes, size_t size, uint32_t base, int64_t& raw);
void store(const Field& f, int64_t raw, uint8_t* bytes, size_t size, uint32_t base);
int64_t toDomain(const Field& f, int64_t raw);      // Resolution: -1 for a tick count not in the table
int64_t toRaw(const Field& f, int64_t d);
std::string format(const Field& f, int64_t d, const edtables::ArpText& t);
// The text shown for the arp preset n through the OS's format, with the OS's own number (the OS counts from 0).
std::string arpPresetLabel(const edtables::ArpText& t, int n);

}  // namespace edfields
