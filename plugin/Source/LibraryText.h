// The plugin window's LIBRARY texts (v0.9.0; no JUCE, so the tests can check them): counts, the Memory column's
// lines, the footer's confirmations and progress. UTF-8.
#pragma once
#include <cstdio>
#include <string>
#include "library/service.h"

namespace libtext {

inline const char* const kEnDash = "\xe2\x80\x93";   // –
inline const char* const kDots = "\xe2\x80\xa6";     // …
inline const char* const kMid = " \xc2\xb7 ";        // ·

inline std::string twoDigits(int n) { char b[16]; std::snprintf(b, sizeof b, "%02d", n); return b; }
inline std::string presetNumber(int preset) { return "P" + twoDigits(preset + 1); }   // 0-63 -> P01-P64
inline std::string bankName(int bank) { return "Bank " + std::to_string(bank + 1); }
inline std::string bankRange(int bank) { return std::to_string(bank * 32 + 1) + kEnDash + std::to_string(bank * 32 + 32); }
inline bool inBank(int preset, int bank) { return preset >= bank * 32 && preset < bank * 32 + 32; }

inline std::string groupHeading(library::Source::Group g) {
    switch (g) {
        case library::Source::Group::Factory: return "FACTORY";
        case library::Source::Group::Banks: return "BANKS";
        case library::Source::Group::Singles: return "SINGLES";
        case library::Source::Group::Backups: return "BACKUPS";
    }
    return {};
}

// A file row's count: its presets ("32"), or how far it is read ("12/32").
inline std::string count(const library::Service::SourceView& v) {
    return v.busy ? std::to_string(v.done) + "/" + std::to_string(v.total) : std::to_string(v.count);
}

// A preset in the PRESETS column: "nn  name" (numbered from 01).
inline std::string presetCell(int index, const std::string& name) { return twoDigits(index + 1) + "  " + name; }

// The Memory column: after "Bank N", the file last loaded into it in this project (empty: none, the row shows
// "Bank N" only), then the detail line.
inline std::string bankTitle(int /*bank*/, const std::string& lastLoaded) { return lastLoaded; }
inline std::string bankDetail(int bank, int playing) {
    return "presets " + bankRange(bank) + (inBank(playing, bank) ? std::string(kMid) + "playing " + presetNumber(playing) : std::string());
}

// The footer.
inline std::string loadConfirm(const std::string& file, int bank, int playing) {
    std::string s = "Load \"" + file + "\" into " + bankName(bank) + " (presets " + bankRange(bank) + ")? " + bankName(bank) + " is backed up first.";
    if (inBank(playing, bank)) s += " " + presetNumber(playing) + " will change; unsaved edits on it will be lost.";
    s += " MIDI input waits until it finishes (about 35 s).";   // a bank takes about 33 s at cable speed
    return s;
}
inline std::string savePrefix(int preset) { return "Save " + (preset >= 0 ? presetNumber(preset) : std::string("the preset")) + " to Singles as"; }
inline const char* const kSaveSuffix = ".syx";
inline const char* const kUnsaved = "UNSAVED EDITS  SAVE ON THE PANEL FIRST";
inline std::string deleteConfirm(const std::string& name) { return "Delete " + name + "? It goes to the Trash."; }
inline std::string bankJob(const library::Service::BankJob& j) {
    if (j.phase == library::Service::BankJob::Phase::Queued || j.phase == library::Service::BankJob::Phase::Backup)
        return "Backing up " + bankName(j.bank) + kDots;
    return "Loading " + j.file + " into " + bankName(j.bank) + kDots;
}
inline std::string notAccepted(int index, const std::string& name) { return presetCell(index, name) + "  NOT ACCEPTED BY THE OS"; }

}  // namespace libtext
