// OS update file test (no ROM data). A random image stands in for the OS; an inverse encoder of our own builds an
// update file from it (13 SysEx messages carrying the data in 7-bit groups), as a MIDI file and as raw SysEx, in both
// layouts the decoder accepts: 11 data blocks between a first and a last message, or 12 after a first message.
// Checks:
//  1. Round trip: both forms and layouts decode to exactly the image, whatever order the blocks come in; OsImage
//     loads them.
//  2. The ROM scanner finds the OS in either form by the decoded image's MD5, says which form, and writes nothing.
//  3. Rejected cleanly: a corrupted block, a wrong length, a missing message, SysEx from another manufacturer,
//     a damaged MIDI file.
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>
#include "os_image.h"
#include "rom_id.h"

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace {
std::mt19937 rng(20261004);

void put5(Bytes& m, uint32_t v) {                    // the manufacturer's 4-to-5 encoding
    for (int i = 0; i < 4; ++i) m.push_back(uint8_t((v >> (7 * i)) & 0x7F));
    m.push_back(uint8_t((v >> 28) & 0x0F));
}

// One data block: F0, 5 header bytes, address, length, payload (7 bytes -> 8 of 7 bits), 5 trailer bytes, F7.
Bytes dataMessage(uint32_t addr, const uint8_t* data, size_t len, uint32_t statedLen) {
    Bytes m = {0xF0, osupdate::kManufacturer, 0x32, 0x00, 0x7F, 0x01};
    put5(m, addr);
    put5(m, statedLen);
    for (size_t g = 0; g < len; g += 7) {
        uint64_t v = 0;
        for (size_t j = 0; j < 7 && g + j < len; ++j) v |= uint64_t(data[g + j]) << (8 * j);
        for (int j = 0; j < 8; ++j) m.push_back(uint8_t((v >> (7 * j)) & 0x7F));
    }
    for (int i = 0; i < 5; ++i) m.push_back(uint8_t(rng() & 0x7F));      // trailer (ignored by the decoder)
    m.push_back(0xF7);
    return m;
}

Bytes otherMessage(uint8_t kind) { return {0xF0, osupdate::kManufacturer, 0x32, 0x00, kind, 0x00, 0x00, 0xF7}; }

// The 13 messages for `image`: a first message, `order.size()` data blocks (11 or 12) of uneven size in `order`
// (indices into address order), and with 11 blocks a last message.
std::vector<Bytes> messagesFor(const Bytes& image, const std::vector<int>& order) {
    const int nb = int(order.size());
    std::vector<size_t> cut = {0};
    const size_t jitter = image.size() / size_t(2 * nb) + 1;
    for (int k = 1; k < nb; ++k) cut.push_back(image.size() * size_t(k) / size_t(nb) + size_t(rng() % jitter));
    cut.push_back(image.size());
    std::sort(cut.begin(), cut.end());
    std::vector<Bytes> blocks;
    for (int k = 0; k < nb; ++k)
        blocks.push_back(dataMessage(uint32_t(OsImage::kLoadAddress + cut[size_t(k)]), image.data() + cut[size_t(k)],
                                     cut[size_t(k) + 1] - cut[size_t(k)], uint32_t(cut[size_t(k) + 1] - cut[size_t(k)])));
    std::vector<Bytes> msgs = {otherMessage(0x10)};
    for (int k : order) msgs.push_back(blocks[size_t(k)]);
    if (nb == 11) msgs.push_back(otherMessage(0x11));
    return msgs;
}

Bytes rawSysex(const std::vector<Bytes>& msgs) {
    Bytes f;
    for (const Bytes& m : msgs) f.insert(f.end(), m.begin(), m.end());
    return f;
}

void vlq(Bytes& b, uint32_t v) {
    uint8_t t[5]; int n = 0;
    do { t[n++] = uint8_t(v & 0x7F); v >>= 7; } while (v);
    while (n--) b.push_back(uint8_t(t[n] | (n ? 0x80 : 0)));
}

// A type-0 MIDI file: a tempo meta event, a note in running status, and the SysEx messages; one of them is sent
// as an F0 packet plus an F7 continuation packet, as some files do.
Bytes midiFile(const std::vector<Bytes>& msgs) {
    Bytes t;
    vlq(t, 0); t.insert(t.end(), {0xFF, 0x51, 0x03, 0x07, 0xA1, 0x20});
    vlq(t, 0); t.insert(t.end(), {0x90, 0x3C, 0x40});
    vlq(t, 10); t.insert(t.end(), {0x3C, 0x00});
    for (size_t k = 0; k < msgs.size(); ++k) {
        const Bytes& m = msgs[k];
        if (k == 5) {
            const size_t half = m.size() / 2;
            vlq(t, 100); t.push_back(0xF0); vlq(t, uint32_t(half - 1)); t.insert(t.end(), m.begin() + 1, m.begin() + long(half));
            vlq(t, 100); t.push_back(0xF7); vlq(t, uint32_t(m.size() - half)); t.insert(t.end(), m.begin() + long(half), m.end());
        } else {
            vlq(t, 100); t.push_back(0xF0); vlq(t, uint32_t(m.size() - 1)); t.insert(t.end(), m.begin() + 1, m.end());
        }
    }
    vlq(t, 0); t.insert(t.end(), {0xFF, 0x2F, 0x00});
    Bytes f = {'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k'};
    for (int i = 3; i >= 0; --i) f.push_back(uint8_t(t.size() >> (8 * i)));
    f.insert(f.end(), t.begin(), t.end());
    return f;
}

void writeFile(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
}

std::string md5Of(const Bytes& b) { return md5Hex(b.data(), b.size()); }
// Decodes and picks the reading whose MD5 is the image's (as the scanner does with the real checksum).
bool decodes(const Bytes& file, const std::string& md5, Bytes& image, OsForm& form, std::string& err) {
    return osupdate::decodeFor(file, md5, image, form, err);
}
}  // namespace

int main() {
    // A random "OS" of the real size (767,196 bytes).
    Bytes image(767196);
    for (uint8_t& b : image) b = uint8_t(rng());
    const std::string imageMd5 = md5Of(image);
    auto orders = [](int nb) {
        std::vector<int> in(size_t(nb), 0), sh;
        for (int k = 0; k < nb; ++k) in[size_t(k)] = k;
        sh = in;
        std::shuffle(sh.begin(), sh.end(), rng);
        return std::vector<std::vector<int>>{in, sh};
    };
    const std::vector<int> inOrder = orders(11)[0];
    const std::vector<int> shuffled = orders(12)[1];

    // 1. Round trip: both forms, both layouts, blocks in address order and shuffled.
    for (int nb : {11, 12})
        for (const std::vector<int>& order : orders(nb)) {
            const std::vector<Bytes> msgs = messagesFor(image, order);
            CHECK(msgs.size() == 13);
            Bytes out; OsForm form; std::string err;
            CHECK(decodes(rawSysex(msgs), imageMd5, out, form, err) && form == OsForm::UpdateSysex);
            CHECK(out == image);
            out.clear();
            CHECK(decodes(midiFile(msgs), imageMd5, out, form, err) && form == OsForm::UpdateMidiFile);
            CHECK(out == image);
            if (!err.empty()) std::printf("os_update_test: unexpected error: %s\n", err.c_str());
        }
    // Other image sizes (the last 7-bit group partly filled).
    for (size_t n : {16u, 1000u, 4099u, 123457u}) {
        Bytes small(n);
        for (uint8_t& b : small) b = uint8_t(rng());
        Bytes out; OsForm form; std::string err;
        CHECK(decodes(rawSysex(messagesFor(small, inOrder)), md5Of(small), out, form, err) && out == small);
    }
    std::printf("os_update_test: round trip of a %zu-byte image exact (MIDI file and SysEx, 11 or 12 blocks, any order)\n", image.size());

    const fs::path dir = fs::temp_directory_path() / "phyzo_os_update_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::vector<Bytes> msgs = messagesFor(image, shuffled);

    // OsImage loads the update file like the image (decoded in memory).
    writeFile(dir / "update.mid", midiFile(msgs));
    {
        OsImage os; std::string err;
        CHECK(os.load((dir / "update.mid").u8string(), err, imageMd5));
        CHECK(os.bytes == image && os.md5 == imageMd5 && os.form == OsForm::UpdateMidiFile);
    }

    // 2. The scanner: any file name, either form, reported as such; the folder is left as it was.
    Bytes wave(romid::kWaveImageSize);
    for (size_t i = 0; i < wave.size(); i += 4096) wave[i] = uint8_t(i >> 12);
    writeFile(dir / "waves", wave);
    romid::Expected ex;
    ex.osMd5 = imageMd5;
    ex.waveMd5 = md5Hex(wave.data(), wave.size());
    auto listing = [&] { std::vector<std::string> v; for (auto& e : fs::directory_iterator(dir)) v.push_back(e.path().filename().u8string()); std::sort(v.begin(), v.end()); return v; };
    const auto before = listing();
    romid::ScanResult r = romid::scanFolder(dir.u8string(), ex);
    CHECK(r.complete() && r.osForm == OsForm::UpdateMidiFile);
    CHECK(fs::u8path(r.osPath).filename() == "update.mid");
    CHECK(listing() == before);                                     // the decoded image is never written
    fs::remove(dir / "update.mid");
    writeFile(dir / "any name at all", rawSysex(msgs));
    r = romid::scanFolder(dir.u8string(), ex);
    CHECK(r.complete() && r.osForm == OsForm::UpdateSysex);
    fs::remove(dir / "any name at all");

    // 3. Rejections. Each is decoded or refused cleanly, and the scanner names the file.
    struct Bad { const char* name; Bytes file; bool decodes; };
    std::vector<Bad> bad;
    {   // a corrupted block: one payload byte changed -> decodes, but the MD5 differs
        std::vector<Bytes> m = msgs; m[4][100] ^= 0x01;
        bad.push_back({"corrupted block.syx", rawSysex(m), true});
    }
    {   // a wrong length: stated longer than the data -> refused
        std::vector<Bytes> m = msgs;
        Bytes& b = m[3]; Bytes len; put5(len, 0x7FFFFF);
        std::copy(len.begin(), len.end(), b.begin() + 11);
        bad.push_back({"wrong length.syx", rawSysex(m), false});
    }
    {   // a wrong length: stated shorter -> decodes, MD5 differs
        std::vector<Bytes> m = msgs;
        Bytes& b = m[7]; uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(b[size_t(11 + i)]) << (7 * i);
        Bytes len; put5(len, v - 1);
        std::copy(len.begin(), len.end(), b.begin() + 11);
        bad.push_back({"short length.syx", rawSysex(m), true});
    }
    {   // a missing message
        std::vector<Bytes> m = msgs; m.erase(m.begin() + 6);
        bad.push_back({"missing message.mid", midiFile(m), false});
    }
    {   // SysEx from another manufacturer
        std::vector<Bytes> m = msgs; for (Bytes& x : m) x[1] = 0x41;
        bad.push_back({"other maker.syx", rawSysex(m), false});
    }
    {   // a MIDI file cut short
        Bytes f = midiFile(msgs); f.resize(f.size() / 2);
        bad.push_back({"cut short.mid", f, false});
    }
    {   // an unterminated SysEx message
        Bytes f = rawSysex(msgs); f.pop_back();
        bad.push_back({"unterminated.syx", f, false});
    }
    for (const Bad& b : bad) {
        Bytes out; OsForm form; std::string err;
        const bool ok = decodes(b.file, imageMd5, out, form, err);
        CHECK(ok == b.decodes);
        if (ok) CHECK(md5Hex(out.data(), out.size()) != imageMd5);
        else CHECK(!err.empty());
        writeFile(dir / b.name, b.file);
        r = romid::scanFolder(dir.u8string(), ex);
        CHECK(!r.hasOs() && r.hasWave());
        const std::string msg = r.problem();
        CHECK(msg.find(b.name) != std::string::npos);
        OsImage os; std::string loadErr;
        if (!ok) CHECK(!os.load((dir / b.name).u8string(), loadErr, imageMd5) && !loadErr.empty());
        std::printf("os_update_test: %-20s rejected (%s)\n", b.name, ok ? "decoded OS has another MD5" : err.c_str());
        fs::remove(dir / b.name);
    }

    // The existing decoded image is still found as before.
    writeFile(dir / "os image", image);
    r = romid::scanFolder(dir.u8string(), ex);
    CHECK(r.complete() && r.osForm == OsForm::Image);

    fs::remove_all(dir);
    std::printf("os_update_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
