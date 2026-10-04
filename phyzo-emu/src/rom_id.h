// Finds the user's two ROM files in a folder by MD5 checksum, whatever their file names.
// Only checksums live here; the files themselves are user-supplied and never committed.
// The OS is accepted as our decoded image or as the original manufacturer's OS update file (MIDI file or SysEx),
// which is decoded in memory and identified by the MD5 of the decoded image.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "os_image.h"

namespace romid {

// The supported versions. The addresses in os_profile.h belong to this OS image.
constexpr char kOsImageMd5[] = "8de06f48bfb0d847cacab05b763e22c5";
constexpr char kWaveImageMd5[] = "42a974e31e48b05815d07554d0063171";

// Size limits used to skip unrelated files before hashing: the OS image must fit the flash below
// user flash (Machine::init), the native wave image is exactly 4 MB (Machine::loadWaveMemory).
constexpr uint64_t kOsImageMaxSize = 0xF0000 - 0x4000;
constexpr uint64_t kWaveImageSize = 0x400000;

struct Expected {
    std::string osMd5 = kOsImageMd5;
    std::string waveMd5 = kWaveImageMd5;
};

struct Rejected {
    std::string name;    // file name inside the folder
    std::string md5;     // empty: not hashed (size cannot match either ROM); for an update file, of the decoded OS
    std::string reason;  // an update file that could not be decoded: why
};

struct ScanResult {
    std::string folder;
    Expected expected;
    bool folderExists = false;
    std::string osPath, wavePath;          // empty when not found
    OsForm osForm = OsForm::Image;         // the form the OS was found in
    std::vector<Rejected> rejected;        // files that are not one of the two ROMs

    bool hasOs() const { return !osPath.empty(); }
    bool hasWave() const { return !wavePath.empty(); }
    bool complete() const { return hasOs() && hasWave(); }
    // Plain-language status naming what is missing and the folder; empty when complete.
    std::string problem() const;
};

// Scans the regular files directly inside `folder` (not subfolders; names starting with '.' are skipped).
ScanResult scanFolder(const std::string& folder, const Expected& expected = Expected());

// MD5 of a whole file as lowercase hex; empty if it cannot be read.
std::string fileMd5(const std::string& path);

}  // namespace romid
