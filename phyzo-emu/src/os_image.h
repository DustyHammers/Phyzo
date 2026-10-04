// OS image loader. The image is user-supplied at run time and never embedded. It is read either as our decoded
// image or as the original manufacturer's OS update file (a Standard MIDI File or raw SysEx), decoded in memory.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// The forms the OS can be supplied in.
enum class OsForm { Image, UpdateMidiFile, UpdateSysex };
const char* osFormName(OsForm f);     // for status lines: "OS image", "OS update file (MIDI file)", ...

// The OS update file: 13 SysEx messages (manufacturer ID 0x0F); the first is not a data block, the ones after it
// are. Whether the 13th is a data block too is not settled until the real file has been checked, so both readings
// are decoded (11 or 12 blocks) and the decoded image's MD5 decides.
namespace osupdate {
constexpr uint8_t kManufacturer = 0x0F;
constexpr int kMessages = 13;
constexpr uint64_t kMaxFileSize = 4u << 20;      // larger files are not considered
// True when the bytes start like a MIDI file ("MThd") or raw SysEx (F0); sets `form`.
bool looksLikeUpdate(const std::vector<uint8_t>& file, OsForm& form);
// The SysEx messages of a MIDI file or a raw SysEx file, each F0 ... F7. False with `err` if malformed.
bool sysexMessages(const std::vector<uint8_t>& file, OsForm form, std::vector<std::vector<uint8_t>>& out, std::string& err);
// Decodes the update file: one image (loading at 0x4000) per reading that is structurally valid. False with a
// plain-language `err` when none is. Only the MD5 says whether an image is the supported OS.
bool decode(const std::vector<uint8_t>& file, std::vector<std::vector<uint8_t>>& images, OsForm& form, std::string& err);
// decode(), then the image whose MD5 is `md5` (else the first): false only when the file cannot be decoded.
bool decodeFor(const std::vector<uint8_t>& file, const std::string& md5, std::vector<uint8_t>& image, OsForm& form,
               std::string& err);
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

    // An image or an update file; an update file's reading whose MD5 is `md5` is preferred (empty: the supported
    // OS, romid::kOsImageMd5).
    bool load(const std::string& p, std::string& err, const std::string& md5 = std::string());
    bool setImage(std::vector<uint8_t> image, std::string& err);
    uint8_t at(uint32_t addr) const {
        uint32_t o = addr - kLoadAddress;
        return o < bytes.size() ? bytes[o] : 0;
    }
};

std::string md5Hex(const uint8_t* data, size_t len);
