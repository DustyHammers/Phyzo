// Cross-architecture state test (F-64): needs no ROM data. The plugin is universal (arm64 and x86_64), and a project
// saved on one architecture must load on the other (Logic under Rosetta, or a project moved to another Mac). A machine
// of our own runs here with every device busy: the 68k takes serial receive interrupts (the interrupt program from
// machine_test), 8 voices play a synthetic wave bank (integer-generated, so both architectures make the same one), and
// the ESP2 runs a seeded random program. It saves its complete state mid-run and runs on; the file holds the state and
// a hash of the continuation (voice and ESP2 output, MIDI out, the final state). Loading the file in another build
// must give the same state back when saved again, and the same continuation.
//   state_xarch_test                save and load in this process (ctest)
//   state_xarch_test save <file>    write the state and its continuation
//   state_xarch_test load <file>    load a file written by `save` (on any architecture), run on, compare
// CI runs `save` on arm64 and `load` under Rosetta (x86_64), and the other way round, and compares the two files.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "machine.h"
#include "os_image.h"

namespace {

#if defined(__aarch64__) || defined(__arm64__)
const char* kArch = "arm64";
#elif defined(__x86_64__)
const char* kArch = "x86_64";
#else
const char* kArch = "other";
#endif

int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// 68k: serial channel B receive interrupts through a vector table in RAM; the handler counts the bytes and main counts
// in d2 (machine_test's interrupt program, checked with os_disasm).
const uint16_t kIrqMain[] = {
    0x203C, 0x0BE1, 0x0000,             // 4080 move.l #$0BE10000,d0
    0x4E7B, 0x0801,                     // 4086 movec  d0,vbr
    0x23FC, 0x0000, 0x4100, 0x0BE1, 0x0100,   // 408A move.l #$4100,$0BE10100   vector $40 -> handler
    0x13FC, 0x0004, 0x00F0, 0x0704,     // 4094 move.b #4,$F00704       serial ILR = 4
    0x13FC, 0x0040, 0x00F0, 0x0705,     // 409C move.b #$40,$F00705     IVR = $40
    0x13FC, 0x0020, 0x00F0, 0x0715,     // 40A4 move.b #$20,$F00715     IER = RxRDYB
    0x13FC, 0x0005, 0x00F0, 0x071A,     // 40AC move.b #5,$F0071A       channel B on
    0x46FC, 0x2000,                     // 40B4 move.w #$2000,sr
    0x5282,                             // 40B8 addq.l #1,d2
    0x23C2, 0x0BE0, 0x0300,             // 40BA move.l d2,$0BE00300
    0x60F6};                            // 40C0 bra    $40B8
const uint16_t kIrqHandler[] = {
    0x1239, 0x00F0, 0x071B,             // 4100 move.b $F0071B,d1
    0x52B9, 0x0BE0, 0x0400,             // 4106 addq.l #1,$0BE00400
    0x13C1, 0x0BE0, 0x0500,             // 410C move.b d1,$0BE00500
    0x4E73};                            // 4112 rte

OsImage irqOs() {
    OsImage os;
    os.bytes.assign(0x1000, 0);
    auto put = [&](size_t off, const uint16_t* w, size_t n) {
        for (size_t i = 0; i < n; ++i) { os.bytes[off + 2 * i] = uint8_t(w[i] >> 8); os.bytes[off + 2 * i + 1] = uint8_t(w[i]); }
    };
    put(0x80, kIrqMain, sizeof kIrqMain / 2);
    put(0x100, kIrqHandler, sizeof kIrqHandler / 2);
    return os;
}

struct Rng {
    uint64_t s;
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    int below(int n) { return int(next() % uint32_t(n)); }
    template <size_t N> int pick(const int (&a)[N]) { return a[below(int(N))]; }
};

// Voice chip: a wave bank made with integers only (two triangle waves and noise), and the 8 voices of voice_core_test's
// golden hash (every filter mode, loop types, ramps, both sides).
std::vector<int16_t> waveBank() {
    std::vector<int16_t> m(1 << 16);
    Rng r{12345};
    for (size_t i = 0; i < m.size(); ++i) {
        const int t1 = int(i % 128), t2 = int((i * 7) % 44);
        const int a = (t1 < 64 ? t1 : 127 - t1) * 360 - 11520, b = (t2 < 22 ? t2 : 43 - t2) * 500 - 5500;
        m[i] = int16_t(a + b + (int(r.next() & 0xFFF) - 0x800));
    }
    return m;
}

void voices(VoiceCore& c) {
    constexpr uint32_t kBank2 = 0x20000000, LPE = 0x0008, BLE = 0x0010;
    auto reg = [&](int v, uint32_t r, uint32_t x) { c.write(0x7C, uint32_t(v), 4); c.write(r, x, 4); };
    c.write(0x6C, 7, 4);
    struct V { uint32_t mode, loop, fc, start, end, k1, k2, lv, rv, res; uint16_t lramp, k1ramp; };
    const V vs[8] = {
        {0x000, LPE, 2048, 100, 900, 0x8000, 0x1000, 0xC000, 0x9000, 0, 0, 0},
        {0x100, LPE | BLE, 3000, 1000, 1500, 0x4000, 0x2000, 0xF000, 0xF000, 0, 0xFF00, 0x0010},
        {0x200, LPE, 1500, 2000, 2600, 0x2000, 0x6000, 0xE000, 0xA000, 0, 0x0101, 0},
        {0x300, BLE, 2500, 3000, 3900, 0xF000, 0x0800, 0xD000, 0xD000, 0, 0, 0xFFF1},
        {0x500, LPE, 777, 4000, 4300, 0, 0, 0xFFFF, 0x8000, 0, 0, 0},
        {0x600, LPE, 2048, 5000, 5400, 0x3000, 0x3000, 0xE800, 0xE800, (0x020u << 9) | 0x013, 0, 0x0020},
        {0x700, LPE, 4096, 6000, 7000, 0x1800, 0x2400, 0xF800, 0xC000, (0x010u << 9) | 0x000, 0, 0},
        {0x600, 0, 2048, 8000, 30000, 0x7000, 0x7000, 0xFFFF, 0xFFFF, (0x030u << 9) | 0x10F, 0xFF01, 0}};
    for (int v = 0; v < 8; ++v) {
        const V& x = vs[v];
        reg(v, 0x08, x.lv); reg(v, 0x10, x.rv); reg(v, 0x24, x.k1); reg(v, 0x1C, x.k2); reg(v, 0x48, x.res);
        reg(v, 0x0C, x.lramp); reg(v, 0x28, x.k1ramp); reg(v, 0x18, 0x1FF);
        reg(v, 0x2C, kBank2 + x.start); reg(v, 0x30, kBank2 + x.end); reg(v, 0x38, kBank2 + x.start);
        reg(v, 0x04, x.fc); reg(v, 0x00, x.mode | x.loop | uint32_t(v % 10) << 12);
    }
}

// ESP2: esp2_core_test's instruction encoding, host-port protocol and plain random lines, inside its harness
// (ADC in, body, DAC out, BIOZ, jump to 0).
struct Line {
    int alu = 0x0B, A = 0x3FF, B = 0x3F1, C = 0x3F1;
    int mac = 2, D = 0x1D4, E = 0x1CC, F = 0x3FF, sh = 6;
    int ag = 6, G = 0x200, rgn = 0, dl = 0;
    bool aluSkip = false, macSkip = false, agSkip = false;
};

std::vector<uint8_t> encode(const Line& l) {
    uint64_t hi = (uint64_t(l.A) << 22) | (uint64_t(l.B) << 12) | (uint64_t(l.C) << 2) | uint64_t(l.alu >> 3);
    uint64_t lo = (uint64_t(l.alu & 7) << 61) | (uint64_t(l.aluSkip) << 60) | (uint64_t(l.D) << 50) | (uint64_t(l.E) << 40) |
                  (uint64_t(l.F) << 30) | (uint64_t(l.mac) << 25) | (uint64_t(l.sh) << 21) | (uint64_t(l.macSkip) << 20) |
                  (uint64_t(l.G & 0x1FF) << 11) | (uint64_t(l.ag) << 8) | (uint64_t(l.rgn) << 5) | (uint64_t(l.dl) << 1) |
                  uint64_t(l.agSkip);
    std::vector<uint8_t> w(12);
    for (int i = 0; i < 4; ++i) w[i] = uint8_t(hi >> (24 - 8 * i));
    for (int i = 0; i < 8; ++i) w[4 + i] = uint8_t(lo >> (56 - 8 * i));
    return w;
}

void hostReg(Esp2Core& e, uint32_t a, uint32_t v) {
    e.write8(0x11, uint8_t(v >> 16)); e.write8(0x12, uint8_t(v >> 8)); e.write8(0x13, uint8_t(v));
    e.write8(0x14, uint8_t(a >> 8)); e.write8(0x15, uint8_t(a)); e.write8(0x17, 0x80);
}
void hostIns(Esp2Core& e, uint32_t a, const std::vector<uint8_t>& w) {
    for (int i = 0; i < 12; ++i) e.write8(uint32_t(i), w[i]);
    e.write8(0x0C, uint8_t(a >> 8)); e.write8(0x0D, uint8_t(a)); e.write8(0x0F, 0x80);
}

Line plainLine(Rng& r) {
    const int reads[] = {0x000, 0x001, 0x002, 0x003, 0x00F, 0x010, 0x011, 0x0FF, 0x0FE, 0x0FD, 0x0FC, 0x1F0, 0x1FF, 0x1CC, 0x1CB, 0x3FF};
    const int writes[] = {0x000, 0x001, 0x002, 0x003, 0x00F, 0x010, 0x011, 0x0FD, 0x0FC, 0x1EF, 0x1E0, 0x3FF};
    const int aluOps[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
                          0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1A, 0x00, 0x03, 0x0B, 0x0B, 0x0D};
    const int macOps[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x14, 0x15, 0x16, 0x17,
                          0x18, 0x19, 0x1A, 0x1B, 0x00, 0x08};
    Line l;
    if (r.below(6)) { l.alu = r.below(40) ? r.pick(aluOps) : 0x1B; l.A = l.alu == 0x1B ? r.below(0x400) : r.pick(reads); l.B = r.pick(reads); l.C = r.pick(writes); }
    if (r.below(6)) { l.mac = r.pick(macOps); l.D = r.pick(reads); l.E = r.pick(reads); l.F = r.pick(writes); l.sh = r.below(16); }
    if (r.below(2)) { const int ops[] = {0, 1, 2, 3, 4, 5, 7}; l.ag = r.pick(ops); l.rgn = r.below(2); l.G = 0x200 + r.below(4); l.dl = r.below(16); }
    if (r.below(8) == 0) { l.aluSkip = r.below(2); l.macSkip = r.below(2); l.agSkip = r.below(2); }
    return l;
}

void esp2Program(Esp2Core& e, uint64_t seed) {
    Rng r{seed};
    std::vector<Line> p;
    Line in; in.B = 0x3EF; in.C = 0x0FF; in.mac = 3; in.D = 0x3EE; in.E = 0x1CB; in.F = 0x0FE; in.sh = 7;
    p.push_back(in);
    p.push_back(Line());
    for (int i = 0; i < 80; ++i) p.push_back(plainLine(r));
    p.push_back(Line());
    Line out; out.B = 0x0FD; out.C = 0x3ED; out.mac = 3; out.D = 0x0FC; out.E = 0x1CB; out.F = 0x3EC; out.sh = 7;
    p.push_back(out);
    Line bioz; bioz.alu = 0x19; p.push_back(bioz);
    Line jmp; jmp.alu = 0x1C; jmp.A = 0; jmp.B = 0; jmp.C = 0x3FF; p.push_back(jmp);
    p.push_back(Line());
    for (size_t i = 0; i < p.size(); ++i) hostIns(e, uint32_t(i), encode(p[i]));
    for (int a = 0; a < 0x20; ++a) hostReg(e, uint32_t(a), r.next() & 0xFFFFFF);
    for (int a = 0x200; a < 0x204; ++a) hostReg(e, uint32_t(a), uint32_t(r.below(0x40)));
    hostReg(e, 0x3DD, Esp2Core::kRamBase + 0xFF); hostReg(e, 0x3DE, 0xFF); hostReg(e, 0x3DF, Esp2Core::kRamBase);
    hostReg(e, 0x3DA, Esp2Core::kVoicePort + 0x1F); hostReg(e, 0x3DB, 0x1F); hostReg(e, 0x3DC, Esp2Core::kVoicePort);
    for (uint32_t q : {0x3FCu, 0x3FBu, 0x3FAu, 0x3C7u, 0x3C6u, 0x3C5u, 0x3C4u}) hostReg(e, q, uint32_t(r.below(0x20)));
    hostReg(e, 0x1D5, uint32_t(r.below(17) - 8) & 0xFFFFFF);
    e.write8(0x19, 0x31);                                // run, IOZ enabled
}

constexpr uint64_t kEsp2Seed = 12;   // a program whose output stays lively: 99 % of samples non-zero, none at full scale

bool build(Machine& m, uint64_t esp2Seed = kEsp2Seed) {
    Machine::Config cfg; std::string err;
    if (!m.init(irqOs(), cfg, err)) { std::printf("init: %s\n", err.c_str()); return false; }
    m.voice.setBank(2, waveBank());
    voices(m.voice);
    esp2Program(m.esp2, esp2Seed);
    m.esp2.audit = false;                                // the plugin's setting
    // The ESP2's serial input: a test signal of exact float values (k / 2^15), so both architectures convert it alike.
    m.audioIn.resize(2 * 1000);
    for (size_t i = 0; i < m.audioIn.size(); ++i) {
        const int t = int(i % 250);
        m.audioIn[i] = float((t < 125 ? t : 249 - t) * 200 - 12400) / 32768.0f;
    }
    return true;
}

// Runs in steps of varying length with a received MIDI byte every fifth step (from `first`, so a continuation differs
// from the run before it).
void runSteps(Machine& m, int first, int n) {
    for (int i = first; i < first + n; ++i) {
        if (i % 5 == 2) m.serial.queueRx(1, {uint8_t(i)}, m.cycles() + uint64_t(31 * (i % 97)));
        m.runUntil(m.cycles() + 900 + 53 * uint64_t(i % 17));
    }
}

uint64_t fnv(uint64_t h, const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}
template <typename T> uint64_t fnvValues(uint64_t h, const std::vector<T>& v) {
    for (const T& x : v) { const int64_t w = int64_t(x); uint8_t b[8]; for (int i = 0; i < 8; ++i) b[i] = uint8_t(uint64_t(w) >> (8 * i)); h = fnv(h, b, 8); }
    return h;
}

// The continuation from the current state: voice and ESP2 output, MIDI out, the 68k's counters and the final state.
uint64_t continuation(Machine& m, uint64_t* handled = nullptr) {
    m.captureAudio = true; m.audio.clear(); m.wet.clear(); m.midiOut.clear();
    runSteps(m, 1000, 600);
    auto nonZero = [](const std::vector<int32_t>& v) { size_t n = 0; for (int32_t x : v) n += x != 0; return n; };
    CHECK(m.audio.size() > 4000 && nonZero(m.audio) > m.audio.size() / 2);   // the voices play
    CHECK(m.wet.size() == m.audio.size() && nonZero(m.wet) > m.wet.size() * 9 / 10);   // and the ESP2 program runs
    uint64_t h = 1469598103934665603ull;
    h = fnvValues(h, m.audio);
    h = fnvValues(h, m.wet);
    for (const auto& [cycle, byte] : m.midiOut) { h = fnvValues(h, std::vector<uint64_t>{cycle, byte}); }
    h = fnvValues(h, std::vector<uint32_t>{m.peek(0x0BE00300, 4), m.peek(0x0BE00400, 4), m.peek(0x0BE00500, 1)});
    const std::vector<uint8_t> end = m.saveState();
    h = fnv(h, end.data(), end.size());
    if (handled) *handled = m.peek(0x0BE00400, 4);
    m.captureAudio = false;
    return h;
}

// The saved run: 400 steps from reset, then the state. Every device must have done something by then.
std::vector<uint8_t> savedRun(Machine& m) {
    runSteps(m, 0, 400);
    CHECK(m.peek(0x0BE00400, 4) == 80);                 // 80 bytes received and handled by the interrupt
    CHECK(m.peek(0x0BE00300, 4) > 1000);                // main ran between them
    CHECK(m.esp2.running() && m.esp2.cycle() > 0);
    CHECK(m.samplesProduced() > 1000);
    return m.saveState();
}

// File: "PHYZXST1", the continuation hash and the state size (little-endian u64 each), then the state.
bool writeFile(const char* path, const std::vector<uint8_t>& state, uint64_t hash) {
    std::vector<uint8_t> f = {'P', 'H', 'Y', 'Z', 'X', 'S', 'T', '1'};
    for (uint64_t v : {hash, uint64_t(state.size())}) for (int i = 0; i < 8; ++i) f.push_back(uint8_t(v >> (8 * i)));
    f.insert(f.end(), state.begin(), state.end());
    FILE* fp = std::fopen(path, "wb");
    if (!fp) return false;
    const bool ok = std::fwrite(f.data(), 1, f.size(), fp) == f.size();
    return std::fclose(fp) == 0 && ok;
}

bool readFile(const char* path, std::vector<uint8_t>& state, uint64_t& hash) {
    FILE* fp = std::fopen(path, "rb");
    if (!fp) return false;
    std::vector<uint8_t> f; uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, fp)) > 0) f.insert(f.end(), buf, buf + n);
    std::fclose(fp);
    if (f.size() < 24 || std::memcmp(f.data(), "PHYZXST1", 8) != 0) return false;
    uint64_t size = 0; hash = 0;
    for (int i = 0; i < 8; ++i) { hash |= uint64_t(f[8 + i]) << (8 * i); size |= uint64_t(f[16 + i]) << (8 * i); }
    if (f.size() != 24 + size) return false;
    state.assign(f.begin() + 24, f.end());
    return true;
}

// Loads `state` into a fresh machine: it must save the same bytes again and continue as `hash` says.
void loadAndCompare(const std::vector<uint8_t>& state, uint64_t hash) {
    Machine m; CHECK(build(m));
    std::string err;
    CHECK(m.loadState(state, err));
    if (!err.empty()) std::printf("load: %s\n", err.c_str());
    CHECK(m.saveState() == state);
    uint64_t handled = 0;
    const uint64_t h = continuation(m, &handled);
    CHECK(handled == 200);
    std::printf("continuation %016" PRIx64 " (expected %016" PRIx64 ")\n", h, hash);
    CHECK(h == hash);
}

}  // namespace

int main(int argc, char** argv) {
    std::printf("state_xarch_test (%s)\n", kArch);
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "save" && argc == 3) {
        Machine m; CHECK(build(m));
        const std::vector<uint8_t> state = savedRun(m);
        const uint64_t h = continuation(m);
        CHECK(writeFile(argv[2], state, h));
        std::printf("saved %zu bytes, continuation %016" PRIx64 "\n", state.size(), h);
    } else if (mode == "load" && argc == 3) {
        std::vector<uint8_t> state; uint64_t hash = 0;
        const bool ok = readFile(argv[2], state, hash);
        CHECK(ok);
        if (ok) { std::printf("loaded %zu bytes\n", state.size()); loadAndCompare(state, hash); }
    } else if (argc == 1) {
        // In one process: the file round trip, a fresh machine continuing as the saved one did, and a second machine
        // from reset saving the same state (the state depends only on what ran).
        Machine a; CHECK(build(a));
        const std::vector<uint8_t> state = savedRun(a);
        const uint64_t h = continuation(a);
        Machine b; CHECK(build(b));
        CHECK(savedRun(b) == state);
        const std::string path = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/phyzo_state_xarch_" +
                                 std::to_string(reinterpret_cast<uintptr_t>(&a) & 0xFFFFFF) + ".bin";
        CHECK(writeFile(path.c_str(), state, h));
        std::vector<uint8_t> back; uint64_t hb = 0;
        CHECK(readFile(path.c_str(), back, hb));
        std::remove(path.c_str());
        CHECK(back == state && hb == h);
        loadAndCompare(back, hb);
    } else {
        std::printf("usage: state_xarch_test [save <file> | load <file>]\n");
        return 2;
    }
    std::printf("state_xarch_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
