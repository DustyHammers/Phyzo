#include "rom_id.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>
#include "os_image.h"

namespace fs = std::filesystem;

namespace romid {

namespace {
bool readFile(const std::string& path, std::vector<uint8_t>& b) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !f.bad();
}
}  // namespace

std::string fileMd5(const std::string& path) {
    std::vector<uint8_t> b;
    if (!readFile(path, b)) return {};
    return md5Hex(b.data(), b.size());
}

ScanResult scanFolder(const std::string& folder, const Expected& expected) {
    ScanResult r;
    r.folder = folder;
    r.expected = expected;
    std::error_code ec;
    r.folderExists = fs::is_directory(fs::u8path(folder), ec);
    if (!r.folderExists) return r;

    std::vector<fs::path> files;
    for (fs::directory_iterator it(fs::u8path(folder), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        if (p.filename().u8string().rfind('.', 0) == 0) continue;     // .DS_Store and other hidden files
        if (it->is_regular_file(ec)) files.push_back(p);
    }
    std::sort(files.begin(), files.end());                             // deterministic choice among duplicates

    for (const fs::path& p : files) {
        const std::string name = p.filename().u8string();
        const uint64_t size = fs::file_size(p, ec);
        if (ec) { r.rejected.push_back({name, "", ""}); continue; }
        const bool maybeOs = size >= 16 && size <= kOsImageMaxSize;
        const bool maybeWave = size == kWaveImageSize;
        const bool maybeUpdate = size >= 4 && size <= osupdate::kMaxFileSize;
        if (!maybeOs && !maybeWave && !maybeUpdate) { r.rejected.push_back({name, "", ""}); continue; }
        std::vector<uint8_t> bytes;
        if (!readFile(p.u8string(), bytes)) { r.rejected.push_back({name, "", ""}); continue; }
        // The original OS update file (MIDI file or SysEx): decoded in memory, identified by the decoded OS's MD5.
        OsForm form;
        if (maybeUpdate && osupdate::looksLikeUpdate(bytes, form)) {
            std::vector<uint8_t> image;
            std::string why;
            if (!osupdate::decode(bytes, image, form, why)) { r.rejected.push_back({name, "", why}); continue; }
            const std::string md5 = md5Hex(image.data(), image.size());
            if (md5 == expected.osMd5) { if (!r.hasOs()) { r.osPath = p.u8string(); r.osForm = form; } }
            else r.rejected.push_back({name, md5, ""});
            continue;
        }
        if (!maybeOs && !maybeWave) { r.rejected.push_back({name, "", ""}); continue; }
        const std::string md5 = md5Hex(bytes.data(), bytes.size());
        if (maybeOs && md5 == expected.osMd5 && !r.hasOs()) { r.osPath = p.u8string(); r.osForm = OsForm::Image; }
        else if (maybeWave && md5 == expected.waveMd5 && !r.hasWave()) r.wavePath = p.u8string();
        else if (md5 != expected.osMd5 && md5 != expected.waveMd5) r.rejected.push_back({name, md5, ""});
        // a second copy of a ROM already found is neither used nor reported
    }
    return r;
}

std::string ScanResult::problem() const {
    if (!folderExists) return "The ROM folder does not exist: " + folder;
    if (complete()) return {};
    std::string s = "Missing from the ROM folder " + folder + ":\n";
    if (!hasOs()) s += "  - the OS: the OS image or the OS update file v1.10 (.mid or .syx); decoded OS MD5 " + expected.osMd5 + "\n";
    if (!hasWave()) s += "  - the native wave image (MD5 " + expected.waveMd5 + ", 4 MB)\n";
    s += "Files are recognised by checksum, so any file name works.";
    std::vector<const Rejected*> hashed;
    for (const Rejected& x : rejected) if (!x.md5.empty() || !x.reason.empty()) hashed.push_back(&x);
    if (!hashed.empty()) {
        s += "\nThese files were not accepted (another version, or damaged):";
        for (const Rejected* x : hashed)
            s += "\n  - " + x->name + (x->reason.empty() ? " (MD5 " + x->md5 + ")" : " (OS update file not readable: " + x->reason + ")");
    }
    return s;
}

}  // namespace romid
