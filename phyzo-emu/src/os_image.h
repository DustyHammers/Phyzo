// OS image loader. The image is user-supplied at run time and never embedded. It is read either as our decoded
// image or as the original manufacturer's OS update file (a Standard MIDI File or raw SysEx), decoded in memory.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The forms the OS can be supplied in.
enum class OsForm { Image, UpdateMidiFile, UpdateSysex };
const char* osFormName(OsForm f);     // for status lines: "OS image", "OS update file (MIDI file)", ...

// The OS update file: SysEx messages with manufacturer ID 0x0F, each body starting 0F 11 01 00 <command>. Data
// blocks have command 0x17 (load address, length, 7-bit payload); the start (0x19) and end (0x18) messages and any
// other command are ignored. The blocks, concatenated in address order, give the OS image; only its MD5 says whether
// it is the supported OS.
namespace osupdate {
constexpr uint8_t kManufacturer = 0x0F;
constexpr uint8_t kDataBlock = 0x17;
constexpr uint64_t kMaxFileSize = 4u << 20;      // larger files are not considered
// True when the bytes start like a MIDI file ("MThd") or raw SysEx (F0); sets `form`.
bool looksLikeUpdate(const std::vector<uint8_t>& file, OsForm& form);
// The SysEx messages of a MIDI file or a raw SysEx file, each F0 ... F7. False with `err` if malformed.
bool sysexMessages(const std::vector<uint8_t>& file, OsForm form, std::vector<std::vector<uint8_t>>& out, std::string& err);
// Decodes the update file into the OS image (loading at 0x4000). False with a plain-language `err` when the file
// is not a well-formed update file.
bool decode(const std::vector<uint8_t>& file, std::vector<uint8_t>& image, OsForm& form, std::string& err);
}

struct OsImage {
    std::vector<uint8_t> bytes;   // the image, loaded at kLoadAddress (decoded in memory from an update file)
    std::string path;
    OsForm form = OsForm::Image;
    std::string md5;              // hex
    uint32_t bodyEnd = 0;         // header long at +0 (end of image body)
    uint32_t entry = 0;           // header long at +4
    uint32_t storedSum = 0;       // trailer long at bodyEnd
    uint32_t computedSum = 0;     // 32-bit sum of big-endian words below bodyEnd
    bool sumOk = false;

    static constexpr uint32_t kLoadAddress = 0x4000;

    bool load(const std::string& p, std::string& err);       // an image or an update file
    bool setImage(std::vector<uint8_t> image, std::string& err);
    uint8_t at(uint32_t addr) const {
        uint32_t o = addr - kLoadAddress;
        return o < bytes.size() ? bytes[o] : 0;
    }
};

std::string md5Hex(const uint8_t* data, size_t len);
