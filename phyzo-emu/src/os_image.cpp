#include "os_image.h"
#include "rom_id.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace {
// Compact MD5 (RFC 1321), used only to confirm the user's baseline file.
struct Md5 {
    uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    static uint32_t rotl(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                                  4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = p[i*4] | (p[i*4+1] << 8) | (p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
        uint32_t A = a0, B = b0, C = c0, D = d0;
        for (int i = 0; i < 64; ++i) {
            uint32_t F; int g;
            if (i < 16)      { F = (B & C) | (~B & D); g = i; }
            else if (i < 32) { F = (D & B) | (~D & C); g = (5*i + 1) % 16; }
            else if (i < 48) { F = B ^ C ^ D;          g = (3*i + 5) % 16; }
            else             { F = C ^ (B | ~D);       g = (7*i) % 16; }
            F = F + A + K[i] + M[g];
            A = D; D = C; C = B; B = B + rotl(F, S[i]);
        }
        a0 += A; b0 += B; c0 += C; d0 += D;
    }
};
}

std::string md5Hex(const uint8_t* data, size_t len) {
    Md5 m;
    size_t full = len / 64;
    for (size_t i = 0; i < full; ++i) m.block(data + i * 64);
    uint8_t tail[128] = {0};
    size_t rem = len - full * 64;
    std::memcpy(tail, data + full * 64, rem);
    tail[rem] = 0x80;
    size_t tlen = (rem < 56) ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) tail[tlen - 8 + i] = (uint8_t)(bits >> (8 * i));
    m.block(tail);
    if (tlen == 128) m.block(tail + 64);
    char out[33];
    uint32_t w[4] = {m.a0, m.b0, m.c0, m.d0};
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", (w[i / 4] >> (8 * (i % 4))) & 0xff);
    return std::string(out, 32);
}

const char* osFormName(OsForm f) {
    switch (f) {
    case OsForm::UpdateMidiFile: return "OS update file (MIDI file)";
    case OsForm::UpdateSysex: return "OS update file (SysEx)";
    default: return "OS image";
    }
}

namespace osupdate {

bool looksLikeUpdate(const std::vector<uint8_t>& file, OsForm& form) {
    if (file.size() >= 4 && std::memcmp(file.data(), "MThd", 4) == 0) { form = OsForm::UpdateMidiFile; return true; }
    if (!file.empty() && file[0] == 0xF0) { form = OsForm::UpdateSysex; return true; }
    return false;
}

namespace {
// MIDI file variable-length quantity; false past the end or longer than 4 bytes.
bool readVlq(const std::vector<uint8_t>& b, size_t& i, size_t end, uint32_t& v) {
    v = 0;
    for (int n = 0; n < 4; ++n) {
        if (i >= end) return false;
        const uint8_t c = b[i++];
        v = (v << 7) | (c & 0x7F);
        if (!(c & 0x80)) return true;
    }
    return false;
}

bool midiFileSysex(const std::vector<uint8_t>& b, std::vector<std::vector<uint8_t>>& out, std::string& err) {
    auto be32 = [&](size_t o) { return uint32_t(b[o]) << 24 | uint32_t(b[o + 1]) << 16 | uint32_t(b[o + 2]) << 8 | b[o + 3]; };
    size_t pos = 0;
    bool open = false;                               // a SysEx message continues in the next F7 packet
    while (pos + 8 <= b.size()) {
        const uint32_t len = be32(pos + 4);
        const size_t start = pos + 8, end = start + len;
        if (end > b.size() || end < start) { err = "the MIDI file is cut short"; return false; }
        if (std::memcmp(&b[pos], "MTrk", 4) == 0) {
            size_t i = start;
            uint8_t running = 0;
            while (i < end) {
                uint32_t delta, n;
                if (!readVlq(b, i, end, delta) || i >= end) { err = "the MIDI file has a damaged track"; return false; }
                uint8_t st = b[i];
                if (st == 0xFF) {                            // meta event
                    if (i + 2 > end) { err = "the MIDI file has a damaged track"; return false; }
                    i += 2;
                    if (!readVlq(b, i, end, n) || n > end - i) { err = "the MIDI file has a damaged track"; return false; }
                    i += n;
                } else if (st == 0xF0 || st == 0xF7) {       // SysEx, or a continuation / escape packet
                    ++i;
                    if (!readVlq(b, i, end, n) || n > end - i) { err = "the MIDI file has a damaged track"; return false; }
                    if (st == 0xF0) { out.push_back({0xF0}); open = true; }
                    if (open) {
                        out.back().insert(out.back().end(), b.begin() + long(i), b.begin() + long(i + n));
                        open = out.back().back() != 0xF7;
                    }
                    i += n;
                    running = 0;
                } else {                                     // channel message, possibly in running status
                    if (st & 0x80) { running = st; ++i; }
                    if (!running) { err = "the MIDI file has a damaged track"; return false; }
                    const int data = (running & 0xE0) == 0xC0 ? 1 : 2;
                    if (i + size_t(data) > end) { err = "the MIDI file has a damaged track"; return false; }
                    i += size_t(data);
                }
            }
        }
        pos = end;                                           // other chunks (and MThd) are skipped
    }
    if (open) { err = "a SysEx message in the MIDI file is not terminated"; return false; }
    return true;
}

bool rawSysex(const std::vector<uint8_t>& b, std::vector<std::vector<uint8_t>>& out, std::string& err) {
    bool open = false;
    for (uint8_t c : b) {
        if (c == 0xF0) {
            if (open) { err = "a SysEx message is not terminated"; return false; }
            out.push_back({0xF0}); open = true;
        } else if (open) {
            out.back().push_back(c);
            if (c == 0xF7) open = false;
            else if (c & 0x80) { err = "a SysEx message contains a status byte"; return false; }
        }
    }
    if (open) { err = "the last SysEx message is not terminated"; return false; }
    return true;
}

// The manufacturer's 4-to-5 encoding of a 32-bit value.
uint32_t value5(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 7 | uint32_t(p[2]) << 14 | uint32_t(p[3]) << 21 | uint32_t(p[4] & 0x0F) << 28;
}
}  // namespace

bool sysexMessages(const std::vector<uint8_t>& file, OsForm form, std::vector<std::vector<uint8_t>>& out, std::string& err) {
    out.clear();
    return form == OsForm::UpdateMidiFile ? midiFileSysex(file, out, err) : rawSysex(file, out, err);
}

namespace {
struct Block { uint32_t addr; std::vector<uint8_t> data; };

// One data block: F0, 5 header bytes, address (5), length (5), payload in 7-bit groups, trailer (5), F7.
bool decodeBlock(const std::vector<uint8_t>& m, int number, Block& bl, std::string& err) {
    const std::string which = "data block " + std::to_string(number);
    if (m.size() < 1 + 5 + 10 + 5 + 1) { err = which + " is too short"; return false; }
    const uint32_t length = value5(&m[11]);
    bl.addr = value5(&m[6]);
    bl.data.clear();
    const size_t p0 = 16, p1 = m.size() - 1 - 5;
    bl.data.reserve((p1 - p0) * 7 / 8 + 7);
    for (size_t g = p0; g < p1; g += 8) {                    // 8 bytes of 7 bits -> 7 bytes, little-endian
        const size_t n = std::min<size_t>(8, p1 - g);
        uint64_t v = 0;
        for (size_t j = 0; j < n; ++j) v |= uint64_t(m[g + j] & 0x7F) << (7 * j);
        for (size_t j = 0; j < n * 7 / 8; ++j) bl.data.push_back(uint8_t(v >> (8 * j)));
    }
    if (bl.data.size() < length) {
        err = which + " holds " + std::to_string(bl.data.size()) + " bytes, fewer than its stated length " + std::to_string(length);
        return false;
    }
    bl.data.resize(length);
    return true;
}

// The blocks concatenated in address order.
bool assemble(std::vector<Block> blocks, std::vector<uint8_t>& image, std::string& err) {
    uint64_t total = 0;
    for (const Block& bl : blocks) total += bl.data.size();
    if (total > 0x100000 - OsImage::kLoadAddress) { err = "the decoded OS is larger than the flash"; return false; }
    std::stable_sort(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.addr < b.addr; });
    image.clear();
    image.reserve(size_t(total));
    for (const Block& bl : blocks) image.insert(image.end(), bl.data.begin(), bl.data.end());
    return true;
}
}  // namespace

bool decode(const std::vector<uint8_t>& file, std::vector<std::vector<uint8_t>>& images, OsForm& form, std::string& err) {
    images.clear();
    if (!looksLikeUpdate(file, form)) { err = "not a MIDI file or a SysEx file"; return false; }
    std::vector<std::vector<uint8_t>> msgs;
    if (!sysexMessages(file, form, msgs, err)) return false;
    if (int(msgs.size()) != kMessages) {
        err = "expected " + std::to_string(kMessages) + " SysEx messages, found " + std::to_string(msgs.size());
        return false;
    }
    for (size_t k = 0; k < msgs.size(); ++k) {
        if (msgs[k].size() < 3 || msgs[k][1] != kManufacturer) {
            char id[8]; std::snprintf(id, sizeof id, "0x%02X", msgs[k].size() > 1 ? msgs[k][1] : 0);
            err = "SysEx message " + std::to_string(k + 1) + " is from another manufacturer (ID " + id + ")";
            return false;
        }
    }
    // Messages 2-12 are data blocks in both readings; message 13 is one in the second.
    std::vector<Block> blocks(size_t(kMessages - 2));
    for (int k = 1; k < kMessages - 1; ++k)
        if (!decodeBlock(msgs[size_t(k)], k, blocks[size_t(k - 1)], err)) return false;
    std::vector<uint8_t> image;
    if (!assemble(blocks, image, err)) return false;
    images.push_back(std::move(image));
    Block last;
    std::string ignored;
    if (decodeBlock(msgs[size_t(kMessages - 1)], kMessages - 1, last, ignored)) {
        blocks.push_back(std::move(last));
        if (assemble(std::move(blocks), image, ignored)) images.push_back(std::move(image));
    }
    return true;
}

bool decodeFor(const std::vector<uint8_t>& file, const std::string& md5, std::vector<uint8_t>& image, OsForm& form,
               std::string& err) {
    std::vector<std::vector<uint8_t>> images;
    if (!decode(file, images, form, err)) return false;
    size_t pick = 0;
    for (size_t i = 0; i < images.size(); ++i)
        if (md5Hex(images[i].data(), images[i].size()) == md5) { pick = i; break; }
    image = std::move(images[pick]);
    return true;
}

}  // namespace osupdate

bool OsImage::load(const std::string& p, std::string& err, const std::string& md5) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "cannot open OS image: " + p; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    path = p;
    form = OsForm::Image;
    OsForm updateForm;
    if (osupdate::looksLikeUpdate(file, updateForm)) {           // decoded here, in memory; never written anywhere
        std::vector<uint8_t> image;
        std::string why;
        if (!osupdate::decodeFor(file, md5.empty() ? std::string(romid::kOsImageMd5) : md5, image, updateForm, why)) {
            err = "OS update file: " + why;
            return false;
        }
        form = updateForm;
        return setImage(std::move(image), err);
    }
    return setImage(std::move(file), err);
}

bool OsImage::setImage(std::vector<uint8_t> image, std::string& err) {
    bytes = std::move(image);
    if (bytes.size() < 16) { err = "OS image too small"; return false; }
    if (bytes.size() > 0x100000 - kLoadAddress) { err = "OS image larger than flash"; return false; }
    md5 = md5Hex(bytes.data(), bytes.size());
    auto be32 = [&](size_t o) {
        return (uint32_t)bytes[o] << 24 | (uint32_t)bytes[o+1] << 16 | (uint32_t)bytes[o+2] << 8 | bytes[o+3];
    };
    bodyEnd = be32(0);
    entry = be32(4);
    uint32_t endOff = bodyEnd - kLoadAddress;
    if (endOff + 4 <= bytes.size() && (endOff & 1) == 0) {
        storedSum = be32(endOff);
        uint32_t s = 0;
        for (uint32_t o = 0; o < endOff; o += 2) s += (uint32_t)(bytes[o] << 8 | bytes[o+1]);
        computedSum = s;
        sumOk = (s == storedSum);
    }
    return true;
}
