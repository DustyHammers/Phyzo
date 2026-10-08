// .syx files for the preset library (v0.8.3 step 2; no plugin framework here): split a file into SysEx messages and
// sort them into single presets and banks, as the OS sends them from its panel's Dump. No OS data is kept here: the
// messages are the user's own files.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace library {

// The OS's single-preset dump: F0 0F 11 01 <device> 02 17 <number:14> ... F7 (the slot is not in the message;
// 3,097 bytes). Its bank dump: F0 0F 11 01 <device> 0D <bank> ... F7, byte 6 = 0 for Bank 1 (presets 1-32),
// 1 for Bank 2 (33-64); one 102,420-byte message.
constexpr uint8_t kPresetCommand = 0x02, kPresetType = 0x17, kBankCommand = 0x0D;
constexpr int kBankPresets = 32;

enum class Kind { Preset, Bank, Other };
// The kind of one whole SysEx message (F0 ... F7); `bank` is 0 or 1 for a bank.
Kind classify(const std::vector<uint8_t>& m, int* bank = nullptr);

struct Split {
    std::vector<std::vector<uint8_t>> presets;     // single-preset messages, in file order
    struct Bank { int number; std::vector<uint8_t> message; };   // number 0 = Bank 1, 1 = Bank 2
    std::vector<Bank> banks;                       // in file order
    // The order of the presets and banks in the file: index into presets (>= 0) or -1 - index into banks.
    std::vector<int> order;
    int other = 0;                                 // other SysEx messages (sounds, settings...): counted, ignored
    int truncated = 0;                             // SysEx cut short (a status byte or the end of the file before F7)
    bool empty() const { return presets.empty() && banks.empty(); }
    // A file that holds exactly one preset (and no bank) goes to SINGLES.
    bool single() const { return presets.size() == 1 && banks.empty(); }
    size_t presetCount() const { return presets.size() + banks.size() * size_t(kBankPresets); }
};
// Splits a file into its SysEx messages. MIDI bytes outside SysEx (channel messages, running status) are skipped,
// real-time bytes (F8-FF) inside a SysEx are dropped from it, and a SysEx cut short by another status byte or by the
// end of the file is counted as truncated.
Split split(const std::vector<uint8_t>& file);

}  // namespace library
