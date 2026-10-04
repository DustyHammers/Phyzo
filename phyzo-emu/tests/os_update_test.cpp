// OS update file test (no ROM data). A random image stands in for the OS; an inverse encoder of our own builds an
// update file from it with the real file's layout: a start message (command 0x19), 12 data blocks (command 0x17:
// 49,152 bytes at 0x4000, 65,536 each at 0x10000 ... 0xA0000, 62,684 at 0xB0000; the payload in 7-bit groups) and an
// end message (0x18), as a MIDI file and as raw SysEx. Checks:
//  1. Round trip: both forms decode to exactly the image, whatever order the blocks come in; OsImage loads them.
//  2. The ROM scanner finds the OS in either form by the decoded image's MD5, says which form, and writes nothing.
//  3. Rejected cleanly: a corrupted block, a wrong length, a missing data block, no data blocks, SysEx from another
//     manufacturer, a cut-short MIDI file, an unterminated message.
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

constexpr uint8_t kStart = 0x19, kEnd = 0x18;
const Bytes kHeader = {osupdate::kManufacturer, 0x11, 0x01, 0x00};   // after F0, before the command byte

struct Span { uint32_t addr; size_t len; };
// The real file's blocks: 0x4000 (49,152), 0x10000 ... 0xA0000 (65,536 each), 0xB0000 (62,684): 767,196 bytes.
std::vector<Span> realLayout() {
    std::vector<Span> v = {{0x4000, 49152}};
    for (uint32_t a = 0x10000; a <= 0xA0000; a += 0x10000) v.push_back({a, 65536});
    v.push_back({0xB0000, 62684});
    return v;
}

void put5(Bytes& m, uint32_t v) {                    // the manufacturer's 4-to-5 encoding
    for (int i = 0; i < 4; ++i) m.push_back(uint8_t((v >> (7 * i)) & 0x7F));
    m.push_back(uint8_t((v >> 28) & 0x0F));
}

Bytes message(uint8_t command, const Bytes& body) {
    Bytes m = {0xF0};
    m.insert(m.end(), kHeader.begin(), kHeader.end());
    m.push_back(command);
    m.insert(m.end(), body.begin(), body.end());
    m.push_back(0xF7);
    return m;
}

// One data block: address, stated length, payload (7 bytes -> 8 of 7 bits), 5 trailer bytes.
Bytes dataMessage(uint32_t addr, const uint8_t* data, size_t len, uint32_t statedLen) {
    Bytes body;
    put5(body, addr);
    put5(body, statedLen);
    for (size_t g = 0; g < len; g += 7) {
        uint64_t v = 0;
        for (size_t j = 0; j < 7 && g + j < len; ++j) v |= uint64_t(data[g + j]) << (8 * j);
        for (int j = 0; j < 8; ++j) body.push_back(uint8_t((v >> (7 * j)) & 0x7F));
    }
    for (int i = 0; i < 5; ++i) body.push_back(uint8_t(rng() & 0x7F));      // trailer (ignored by the decoder)
    return message(osupdate::kDataBlock, body);
}

// Start (16 bytes), the data blocks for `image` split as `spans` in `order` (indices into `spans`), end (7 bytes).
std::vector<Bytes> messagesFor(const Bytes& image, const std::vector<Span>& spans, const std::vector<int>& order) {
    std::vector<Bytes> msgs = {message(kStart, Bytes(9, 0x00))};
    std::vector<size_t> offset(spans.size(), 0);
    for (size_t k = 1; k < spans.size(); ++k) offset[k] = offset[k - 1] + spans[k - 1].len;
    for (int k : order) {
        const Span& s = spans[size_t(k)];
        msgs.push_back(dataMessage(s.addr, image.data() + offset[size_t(k)], s.len, uint32_t(s.len)));
    }
    msgs.push_back(message(kEnd, {}));
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
bool decodes(const Bytes& file, Bytes& image, OsForm& form, std::string& err) { return osupdate::decode(file, image, form, err); }
}  // namespace

int main() {
    const std::vector<Span> layout = realLayout();
    size_t total = 0;
    for (const Span& s : layout) total += s.len;
    CHECK(total == 767196 && layout.size() == 12);
    Bytes image(total);
    for (uint8_t& b : image) b = uint8_t(rng());
    const std::string imageMd5 = md5Of(image);
    std::vector<int> inOrder(layout.size()), shuffled;
    for (size_t k = 0; k < layout.size(); ++k) inOrder[k] = int(k);
    shuffled = inOrder;
    std::shuffle(shuffled.begin(), shuffled.end(), rng);

    // 1. Round trip: both forms, blocks in address order and shuffled.
    for (const std::vector<int>* order : {&inOrder, &shuffled}) {
        const std::vector<Bytes> msgs = messagesFor(image, layout, *order);
        CHECK(msgs.size() == 14 && msgs.front().size() == 16 && msgs.back().size() == 7);
        Bytes out; OsForm form; std::string err;
        CHECK(decodes(rawSysex(msgs), out, form, err) && form == OsForm::UpdateSysex);
        CHECK(out == image);
        out.clear();
        CHECK(decodes(midiFile(msgs), out, form, err) && form == OsForm::UpdateMidiFile);
        CHECK(out == image);
        if (!err.empty()) std::printf("os_update_test: unexpected error: %s\n", err.c_str());
    }
    // Other sizes and block counts (the last 7-bit group partly filled).
    for (size_t n : {16u, 1000u, 4099u, 123457u}) {
        Bytes small(n);
        for (uint8_t& b : small) b = uint8_t(rng());
        std::vector<Span> spans = {{0x4000, n / 3}, {uint32_t(0x4000 + n / 3), n - n / 3}};
        Bytes out; OsForm form; std::string err;
        CHECK(decodes(rawSysex(messagesFor(small, spans, {1, 0})), out, form, err) && out == small);
    }
    std::printf("os_update_test: round trip of a %zu-byte image exact (MIDI file and SysEx, blocks in any order)\n", image.size());

    const fs::path dir = fs::temp_directory_path() / "phyzo_os_update_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::vector<Bytes> msgs = messagesFor(image, layout, inOrder);

    // OsImage loads the update file like the image (decoded in memory).
    writeFile(dir / "update.mid", midiFile(msgs));
    {
        OsImage os; std::string err;
        CHECK(os.load((dir / "update.mid").u8string(), err));
        CHECK(os.bytes == image && os.md5 == imageMd5 && os.form == OsForm::UpdateMidiFile);
    }

    // 2. The scanner: any file name, either form, reported as such; the folder is left as it was.
    Bytes wave(romid::kWaveImageSize);
    for (size_t i = 0; i < wave.size(); i += 4096) wave[i] = uint8_t(i >> 12);
    writeFile(dir / "waves", wave);
    romid::Expected ex;
    ex.osMd5 = imageMd5;
    ex.waveMd5 = md5Of(wave);
    auto listing = [&] {
        std::vector<std::string> v;
        for (auto& e : fs::directory_iterator(dir)) v.push_back(e.path().filename().u8string());
        std::sort(v.begin(), v.end());
        return v;
    };
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
        std::vector<Bytes> m = msgs; Bytes len; put5(len, 0x7FFFFF);
        std::copy(len.begin(), len.end(), m[3].begin() + 11);
        bad.push_back({"wrong length.syx", rawSysex(m), false});
    }
    {   // a wrong length: stated shorter -> decodes, MD5 differs
        std::vector<Bytes> m = msgs; Bytes len; put5(len, uint32_t(layout[6].len - 1));
        std::copy(len.begin(), len.end(), m[7].begin() + 11);
        bad.push_back({"short length.syx", rawSysex(m), true});
    }
    {   // a missing data block -> decodes, MD5 differs
        std::vector<Bytes> m = msgs; m.erase(m.begin() + 6);
        bad.push_back({"missing block.mid", midiFile(m), true});
    }
    {   // only the start and end messages -> refused
        std::vector<Bytes> m = {msgs.front(), msgs.back()};
        bad.push_back({"no data blocks.syx", rawSysex(m), false});
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
        const bool ok = decodes(b.file, out, form, err);
        CHECK(ok == b.decodes);
        if (ok) CHECK(md5Of(out) != imageMd5);
        else CHECK(!err.empty());
        writeFile(dir / b.name, b.file);
        r = romid::scanFolder(dir.u8string(), ex);
        CHECK(!r.hasOs() && r.hasWave());
        CHECK(r.problem().find(b.name) != std::string::npos);
        OsImage os; std::string loadErr;
        if (!ok) CHECK(!os.load((dir / b.name).u8string(), loadErr) && !loadErr.empty());
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
