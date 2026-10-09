// The preset library on disk (v0.8.3 step 2; no plugin framework here): the Library folder's sources, the index cache
// (each file's presets as the OS names them, read once in a hidden machine) and safe new file names.
//
// <data folder>/Library/                 the user's .syx files, any subfolders
// <data folder>/Library/Factory/         Factory Bank 1.syx, Factory Bank 2.syx (made from the user's own OS)
// <data folder>/Library/Singles/         presets saved from the panel, single-preset imports
// <data folder>/cache/library/<md5>.idx  the index of the file with that MD5
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace library {

// ---- index cache

struct Entry {
    std::string name;                              // the OS's name, trimmed (a refused preset: its file's title)
    std::vector<uint8_t> message;                  // the single-preset message, as the file has it or as the OS dumped it
    bool accepted = true;                          // false: the OS did not take it (no Compare LED, no name)
};
struct Index {
    std::vector<Entry> entries;                    // in file order (a bank: its 32 presets)
    int other = 0;                                 // other SysEx messages in the file (ignored)
};
// <cacheDir>/<file MD5>.idx. The file holds the OS image's MD5 too: an index made with another OS is not used.
std::string indexPath(const std::string& cacheDir, const std::string& fileMd5);
bool readIndex(const std::string& path, const std::string& osMd5, Index& out);
bool writeIndex(const std::string& path, const std::string& osMd5, const Index& idx);   // a temporary file, then renamed

// ---- file names

// A preset name as a file name: trimmed, characters file systems reserve (/ \ : * ? " < > | and control characters)
// replaced by '_', no leading dot or trailing dot; "Preset" if nothing is left.
std::string safeFileName(const std::string& name);
// `dir`/`base`.syx, or `base 2`.syx, `base 3`.syx ... the first that does not exist.
std::string uniquePath(const std::string& dir, const std::string& base, const std::string& ext = ".syx");
// Writes a new file; never overwrites one (false if `path` exists or cannot be written).
bool writeNew(const std::string& path, const std::vector<uint8_t>& bytes);
// Writes `path` whole (a temporary file renamed over it): the factory bank files.
bool writeReplace(const std::string& path, const std::vector<uint8_t>& bytes);
bool readFile(const std::string& path, std::vector<uint8_t>& out, size_t limit = size_t(64) << 20);

// ---- the Library folder

constexpr const char* kFactoryTitles[2] = {"FACTORY BANK 1", "FACTORY BANK 2"};
constexpr const char* kSinglesTitle = "SINGLES";
std::string factoryFile(const std::string& libraryDir, int bank);   // Factory/Factory Bank <bank + 1>.syx
std::string singlesDir(const std::string& libraryDir);

struct File {
    std::string path, rel, title;                  // full path, path inside Library/, file name without .syx
    std::string md5;
    size_t presets = 0;                            // single presets + 32 per bank
    bool single = false;                           // exactly one preset: listed under SINGLES
    int other = 0, truncated = 0;
};
struct Source {
    enum class Type { Factory, File, Singles } type = Type::File;
    std::string title;
    int factoryBank = -1;                          // Factory: 0 or 1
    std::vector<int> files;                        // indexes into Scan::files (Factory: none while the file is missing)
};
struct Scan {
    std::vector<File> files;
    std::vector<Source> sources;                   // FACTORY BANK 1, FACTORY BANK 2, files by title, SINGLES (if any)
    std::vector<int> bad;                          // files with no preset or bank
};
// Reads every .syx file under `libraryDir` (subfolders included) and lists the sources. The factory sources are
// always listed (their files may still be missing).
Scan scan(const std::string& libraryDir);

}  // namespace library
