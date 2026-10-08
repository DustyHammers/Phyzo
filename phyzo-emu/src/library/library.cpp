#include "library/library.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#include "library/syx.h"
#include "os_image.h"

namespace fs = std::filesystem;

namespace library {

namespace {
constexpr char kIndexMagic[8] = {'P', 'H', 'Z', 'L', 'I', 'B', 'X', '1'};

void put32(std::vector<uint8_t>& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i))); }
bool get32(const std::vector<uint8_t>& b, size_t& p, uint32_t& v) {
    if (p > b.size() || b.size() - p < 4) return false;
    v = uint32_t(b[p]) | uint32_t(b[p + 1]) << 8 | uint32_t(b[p + 2]) << 16 | uint32_t(b[p + 3]) << 24;
    p += 4;
    return true;
}
std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
std::string tempName(const std::string& path) {
    return path + ".tmp" + std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()) ^
                                          uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
}
}  // namespace

// ------------------------------------------------------------------ index cache

std::string indexPath(const std::string& cacheDir, const std::string& fileMd5) { return cacheDir + "/" + fileMd5 + ".idx"; }

// magic (8), OS MD5 (32), other (u32), count (u32), per entry: accepted (u8), name length (u32), name, message
// length (u32), message; then the MD5 of everything before it (32).
bool writeIndex(const std::string& path, const std::string& osMd5, const Index& idx) {
    std::vector<uint8_t> b(kIndexMagic, kIndexMagic + sizeof kIndexMagic);
    std::string os = osMd5; os.resize(32, ' ');
    b.insert(b.end(), os.begin(), os.end());
    put32(b, uint32_t(idx.other));
    put32(b, uint32_t(idx.entries.size()));
    for (const Entry& e : idx.entries) {
        b.push_back(e.accepted ? 1 : 0);
        put32(b, uint32_t(e.name.size()));
        b.insert(b.end(), e.name.begin(), e.name.end());
        put32(b, uint32_t(e.message.size()));
        b.insert(b.end(), e.message.begin(), e.message.end());
    }
    const std::string sum = md5Hex(b.data(), b.size());
    b.insert(b.end(), sum.begin(), sum.end());
    return writeReplace(path, b);
}

bool readIndex(const std::string& path, const std::string& osMd5, Index& out) {
    std::vector<uint8_t> b;
    if (!readFile(path, b) || b.size() < sizeof kIndexMagic + 32 + 8 + 32) return false;
    const size_t body = b.size() - 32;
    if (md5Hex(b.data(), body) != std::string(reinterpret_cast<const char*>(&b[body]), 32)) return false;
    if (std::memcmp(b.data(), kIndexMagic, sizeof kIndexMagic) != 0) return false;
    std::string os = osMd5; os.resize(32, ' ');
    if (std::memcmp(&b[sizeof kIndexMagic], os.data(), 32) != 0) return false;
    b.resize(body);
    size_t p = sizeof kIndexMagic + 32;
    uint32_t other = 0, count = 0;
    if (!get32(b, p, other) || !get32(b, p, count) || count > 100000) return false;
    Index idx;
    idx.other = int(other);
    for (uint32_t i = 0; i < count; ++i) {
        Entry e;
        uint32_t n = 0;
        if (p >= b.size()) return false;
        e.accepted = b[p++] != 0;
        if (!get32(b, p, n) || n > b.size() - p) return false;
        e.name.assign(reinterpret_cast<const char*>(&b[p]), n); p += n;
        if (!get32(b, p, n) || n > b.size() - p) return false;
        e.message.assign(b.begin() + long(p), b.begin() + long(p + n)); p += n;
        idx.entries.push_back(std::move(e));
    }
    if (p != b.size()) return false;
    out = std::move(idx);
    return true;
}

// ------------------------------------------------------------------ files

bool readFile(const std::string& path, std::vector<uint8_t>& out, size_t limit) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536]; size_t n;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
        if (out.size() + n > limit) { ok = false; break; }
        out.insert(out.end(), buf, buf + n);
    }
    std::fclose(f);
    return ok;
}

bool writeNew(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
#ifdef _WIN32
    FILE* f = std::fopen(path.c_str(), "wbx");         // exclusive: fails if the file exists
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);   // exclusive: fails if the file exists
    FILE* f = fd >= 0 ? ::fdopen(fd, "wb") : nullptr;
    if (fd >= 0 && !f) ::close(fd);
#endif
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0 || !ok) { std::remove(path.c_str()); return false; }
    return true;
}

bool writeReplace(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = tempName(path);
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    return true;
}

std::string safeFileName(const std::string& name) {
    std::string s;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        s += (u < 0x20 || u == 0x7F || std::strchr("/\\:*?\"<>|", c)) ? '_' : c;
    }
    auto blank = [](char c) { return c == ' ' || c == '.'; };
    while (!s.empty() && blank(s.back())) s.pop_back();          // no trailing space or dot
    size_t first = 0;
    while (first < s.size() && blank(s[first])) ++first;          // no leading space or dot (hidden files)
    s.erase(0, first);
    return s.empty() ? std::string("Preset") : s;
}

std::string uniquePath(const std::string& dir, const std::string& base, const std::string& ext) {
    std::error_code ec;
    for (int k = 1;; ++k) {
        const std::string p = dir + "/" + base + (k == 1 ? std::string() : " " + std::to_string(k)) + ext;
        if (!fs::exists(p, ec)) return p;
    }
}

// ------------------------------------------------------------------ the Library folder

std::string factoryFile(const std::string& libraryDir, int bank) {
    return libraryDir + "/Factory/Factory Bank " + std::to_string(bank + 1) + ".syx";
}
std::string singlesDir(const std::string& libraryDir) { return libraryDir + "/Singles"; }

Scan scan(const std::string& libraryDir) {
    Scan s;
    std::error_code ec;
    std::vector<fs::path> paths;
    for (fs::recursive_directory_iterator it(libraryDir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || lower(it->path().extension().string()) != ".syx") continue;
        paths.push_back(it->path());
    }
    std::sort(paths.begin(), paths.end(), [](const fs::path& a, const fs::path& b) {
        const std::string la = lower(a.generic_string()), lb = lower(b.generic_string());
        return la != lb ? la < lb : a < b;
    });
    const fs::path f1 = fs::path(factoryFile(libraryDir, 0)).lexically_normal(), f2 = fs::path(factoryFile(libraryDir, 1)).lexically_normal();
    Source factory[2];
    for (int k = 0; k < 2; ++k) { factory[k].type = Source::Type::Factory; factory[k].title = kFactoryTitles[k]; factory[k].factoryBank = k; }
    std::vector<Source> files;
    Source singles; singles.type = Source::Type::Singles; singles.title = kSinglesTitle;
    for (const fs::path& p : paths) {
        std::vector<uint8_t> bytes;
        if (!readFile(p.string(), bytes)) continue;
        const Split sp = split(bytes);
        File f;
        f.path = p.string();
        f.rel = p.lexically_relative(libraryDir).generic_string();
        f.title = p.stem().string();
        f.md5 = md5Hex(bytes.data(), bytes.size());
        f.presets = sp.presetCount();
        f.single = sp.single();
        f.other = sp.other; f.truncated = sp.truncated;
        const int i = int(s.files.size());
        s.files.push_back(f);
        const fs::path norm = p.lexically_normal();
        if (sp.empty()) s.bad.push_back(i);
        else if (norm == f1 || norm == f2) factory[norm == f1 ? 0 : 1].files.push_back(i);
        else if (f.single) singles.files.push_back(i);
        else { Source src; src.title = f.title; src.files.push_back(i); files.push_back(src); }
    }
    std::stable_sort(files.begin(), files.end(), [](const Source& a, const Source& b) { return lower(a.title) < lower(b.title); });
    s.sources.push_back(factory[0]);
    s.sources.push_back(factory[1]);
    s.sources.insert(s.sources.end(), files.begin(), files.end());
    if (!singles.files.empty()) s.sources.push_back(singles);
    return s;
}

}  // namespace library
