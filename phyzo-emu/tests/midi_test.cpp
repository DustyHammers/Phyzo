// MIDI ports test (no ROM data).
//  1. Parser: running status expanded, SysEx whole (also with real-time bytes inside), incomplete messages dropped.
//  2. Merge: two sources, each cut into random chunks and interleaved chunk by chunk, each parsed into whole messages
//     and merged into one stream: every message of each source arrives whole and in order.
//  3. Message ring: wrap-around, a full ring refuses whole messages, a threaded producer/consumer keeps every byte.
//  4. Engine: a 68k program of our own echoes every MIDI byte it receives. Host MIDI and the virtual port (fed from
//     another thread) go in at once; the echo comes back as whole messages on the virtual port and to the host.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "engine.h"
#include "machine.h"
#include "os_image.h"
#include "midi_stream.h"

// Allocation counter for the audio thread's library feed (this test's own operator new).
static thread_local bool g_countAllocs = false;
static std::atomic<int> g_allocs{0};
void* operator new(std::size_t n) {
    if (g_countAllocs) ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

using Msg = std::vector<uint8_t>;

std::vector<Msg> parseAll(MidiParser& p, const std::vector<uint8_t>& bytes) {
    std::vector<Msg> out;
    for (uint8_t b : bytes) if (p.feed(b)) out.emplace_back(p.data(), p.data() + p.size());
    return out;
}
std::vector<Msg> parseAll(const std::vector<uint8_t>& bytes) { MidiParser p; return parseAll(p, bytes); }

struct Rng {
    uint64_t s;
    uint32_t next() { s = s * 6364136223846793005ull + 1442695040888963407ull; return uint32_t(s >> 33); }
    int below(int n) { return int(next() % uint32_t(n)); }
};

// A random message of a source (channel `ch`); SysEx carries the source id as its first data byte.
Msg randomMessage(Rng& r, int ch, int src, bool realtime) {
    const int k = r.below(realtime ? 12 : 11);
    if (k < 6) { const uint8_t st = uint8_t(0x80 | (r.below(3) << 4) | ch); return {st, uint8_t(r.below(128)), uint8_t(r.below(128))}; }
    if (k == 6) return {uint8_t(0xB0 | ch), uint8_t(r.below(120)), uint8_t(r.below(128))};
    if (k == 7) return {uint8_t(0xC0 | ch), uint8_t(r.below(128))};
    if (k == 8) return {uint8_t(0xE0 | ch), uint8_t(r.below(128)), uint8_t(r.below(128))};
    if (k == 9 || k == 10) {
        Msg m{0xF0, uint8_t(src)};
        for (int i = 0, n = r.below(3) ? r.below(40) : r.below(6000); i < n; ++i) m.push_back(uint8_t(r.below(128)));
        m.push_back(0xF7);
        return m;
    }
    return {0xF8};
}

// Encodes messages as a sender with running status would (status omitted when it repeats), with real-time bytes
// sometimes placed inside a following message (they are their own messages; listed where the parser emits them).
std::vector<uint8_t> encode(const std::vector<Msg>& msgs) {
    std::vector<uint8_t> out;
    uint8_t running = 0;
    for (const Msg& m : msgs) {
        if (m[0] < 0xF0 && m[0] == running) out.insert(out.end(), m.begin() + 1, m.end());
        else out.insert(out.end(), m.begin(), m.end());
        running = m[0] < 0xF0 ? m[0] : (m[0] >= 0xF8 ? running : 0);
    }
    return out;
}

void testParser() {
    CHECK((parseAll({0x90, 0x3C, 0x40, 0x3E, 0x41, 0x80, 0x3C, 0x00}) == std::vector<Msg>{{0x90, 0x3C, 0x40}, {0x90, 0x3E, 0x41}, {0x80, 0x3C, 0x00}}));
    CHECK((parseAll({0xC1, 0x05, 0x06}) == std::vector<Msg>{{0xC1, 0x05}, {0xC1, 0x06}}));
    // real-time inside a note and inside SysEx: their own messages; the others stay whole
    CHECK((parseAll({0x90, 0xF8, 0x3C, 0x40, 0xF0, 0x01, 0xFE, 0x02, 0xF7}) == std::vector<Msg>{{0xF8}, {0x90, 0x3C, 0x40}, {0xFE}, {0xF0, 0x01, 0x02, 0xF7}}));
    // incomplete: a note cut by another status, SysEx cut by a status, data with no status, stray F7
    MidiParser p;
    CHECK((parseAll(p, {0x3C, 0x90, 0x3C, 0xB0, 0x07, 0x64, 0xF0, 0x01, 0x02, 0x90, 0x3C, 0x40, 0xF7}) == std::vector<Msg>{{0xB0, 0x07, 0x64}, {0x90, 0x3C, 0x40}}));
    CHECK(p.dropped() == 2);
    // system common cancels running status
    CHECK((parseAll({0x90, 0x3C, 0x40, 0xF2, 0x01, 0x02, 0x3E, 0x40}) == std::vector<Msg>{{0x90, 0x3C, 0x40}, {0xF2, 0x01, 0x02}}));
    // a SysEx longer than the parser's limit is dropped whole
    MidiParser small(16);
    Msg big{0xF0}; for (int i = 0; i < 40; ++i) big.push_back(1); big.push_back(0xF7);
    Msg after{0x90, 1, 2};
    Msg both = big; both.insert(both.end(), after.begin(), after.end());
    CHECK((parseAll(small, both) == std::vector<Msg>{after}));
    // long SysEx survives
    Msg huge{0xF0}; for (int i = 0; i < 200000; ++i) huge.push_back(uint8_t(i & 0x7F)); huge.push_back(0xF7);
    CHECK((parseAll(huge) == std::vector<Msg>{huge}));
}

// Two sources cut into random chunks, chunks interleaved, each source parsed by its own parser (as the engine does
// for the virtual port, while the host's events are whole already) and the messages appended to one stream.
void testMerge() {
    int bad = 0;
    for (uint64_t seed = 1; seed <= 200; ++seed) {
        Rng r{seed};
        std::vector<Msg> src[2];
        for (int s = 0; s < 2; ++s) for (int i = 0, n = 50 + r.below(200); i < n; ++i) src[s].push_back(randomMessage(r, s, s, s == 0));
        const std::vector<uint8_t> enc[2] = {encode(src[0]), encode(src[1])};
        MidiParser parser[2];
        size_t pos[2] = {0, 0};
        std::vector<uint8_t> merged;
        while (pos[0] < enc[0].size() || pos[1] < enc[1].size()) {
            const int s = pos[0] >= enc[0].size() ? 1 : pos[1] >= enc[1].size() ? 0 : r.below(2);
            const size_t n = std::min(enc[s].size() - pos[s], size_t(1 + r.below(r.below(4) ? 8 : 3000)));
            for (size_t i = 0; i < n; ++i)
                if (parser[s].feed(enc[s][pos[s] + i])) merged.insert(merged.end(), parser[s].data(), parser[s].data() + parser[s].size());
            pos[s] += n;
        }
        // The merged stream parses into exactly the two sources' messages, each source in order. Source 1 has no
        // real-time bytes, so F8 always belongs to source 0; otherwise channel / SysEx id tells the source.
        const std::vector<Msg> out = parseAll(merged);
        size_t k[2] = {0, 0};
        bool ok = out.size() == src[0].size() + src[1].size();
        for (const Msg& m : out) {
            if (!ok) break;
            const int s = m[0] == 0xF8 ? 0 : m[0] == 0xF0 ? m[1] : (m[0] & 0x0F);
            ok = s >= 0 && s < 2 && k[s] < src[s].size() && src[s][k[s]] == m;
            if (ok) ++k[s];
        }
        if (!ok) { if (!bad) std::printf("merge seed %llu: messages split, lost or out of order\n", (unsigned long long)seed); ++bad; }
    }
    CHECK(bad == 0);
}

void testRing() {
    MessageRing ring(64);
    const uint8_t a[20] = {1, 2, 3}, b[30] = {4};
    CHECK(ring.push(a, 20) && ring.push(b, 30));
    CHECK(!ring.push(a, 20));                         // 24 + 34 used of 64: a whole message must fit
    std::vector<uint8_t> got;
    ring.pop([&](uint8_t x) { got.push_back(x); });
    CHECK(got.size() == 20 && got[0] == 1 && got[2] == 3);
    CHECK(ring.push(a, 20));                          // wraps around
    got.clear(); ring.pop([&](uint8_t x) { got.push_back(x); });
    CHECK(got.size() == 30 && got[0] == 4);
    got.clear(); ring.pop([&](uint8_t x) { got.push_back(x); });
    CHECK(got.size() == 20 && ring.peekSize() == 0);

    // threaded: messages of varying size with a running counter; the consumer checks every byte
    MessageRing big(4096);
    constexpr int kMsgs = 200000;
    std::atomic<bool> okAll{true};
    std::thread producer([&] {
        Rng r{7}; uint8_t buf[600]; uint32_t c = 0;
        for (int i = 0; i < kMsgs; ++i) {
            const size_t n = 1 + size_t(r.below(600));
            for (size_t j = 0; j < n; ++j) buf[j] = uint8_t(c++);
            while (!big.push(buf, n)) std::this_thread::yield();
        }
    });
    uint32_t expect = 0; int msgs = 0;
    while (msgs < kMsgs) {
        if (!big.peekSize()) { std::this_thread::yield(); continue; }
        big.pop([&](uint8_t x) { if (x != uint8_t(expect++)) okAll = false; });
        ++msgs;
    }
    producer.join();
    CHECK(okAll.load());
}

// main() at 0x4080: channel B on, then echo every received byte: wait for RxRDY, read, wait for TxRDY, send.
const uint16_t kEchoOut[] = {0x13FC, 0x0005, 0x00F0, 0x071A,   // 4080 move.b #$05,$F0071A
                             0x0839, 0x0000, 0x00F0, 0x0719,   // 4088 btst #0,$F00719      RxRDY
                             0x67F6,                           // 4090 beq.b $4088
                             0x1239, 0x00F0, 0x071B,           // 4092 move.b $F0071B,d1
                             0x0839, 0x0002, 0x00F0, 0x0719,   // 4098 btst #2,$F00719      TxRDY
                             0x67F6,                           // 40A0 beq.b $4098
                             0x13C1, 0x00F0, 0x071B,           // 40A2 move.b d1,$F0071B
                             0x60DE};                          // 40A8 bra.b $4088

void testEngine() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_midi_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kEchoOut / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kEchoOut[i] >> 8); os[0x81 + 2 * i] = uint8_t(kEchoOut[i]); }
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    Engine e;
    e.setRoms((dir / "os").string(), (dir / "wave").string(), "os-md5", "wave-md5");
    for (int i = 0; i < 1000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(e.state() == Engine::State::Running);
    e.setVirtualPort(true); e.setMidiToHost(true);
    e.prepare(48000, 256);

    // Virtual port: notes with running status, SysEx (one of 3000 bytes), fed in odd chunks from another thread.
    Rng r{99};
    std::vector<Msg> vmsgs, hmsgs;
    for (int i = 0; i < 60; ++i) vmsgs.push_back(i == 30 ? Msg() : randomMessage(r, 1, 1, false));
    vmsgs[30] = {0xF0, 1}; for (int i = 0; i < 3000; ++i) vmsgs[30].push_back(uint8_t(i & 0x7F)); vmsgs[30].push_back(0xF7);
    for (int i = 0; i < 60; ++i) { Msg m = randomMessage(r, 0, 0, false); if (m[0] == 0xF0) m = {0x90, 0x40, 0x40}; hmsgs.push_back(m); }
    const std::vector<uint8_t> venc = encode(vmsgs);
    std::thread feeder([&] {
        Rng q{5};
        for (size_t p = 0; p < venc.size();) {
            const size_t n = std::min(venc.size() - p, size_t(1 + q.below(50)));
            e.virtualMidiIn(venc.data() + p, n);
            p += n;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    std::vector<float> l(256), rr(256);
    std::vector<Msg> virtOut, hostOut;
    size_t hi = 0;
    // Until every echo is back: blocks run while the feeder thread sends (it may be slower than these blocks), then
    // at most 30 s of audio more.
    std::atomic<bool> fed{false};
    std::thread watcher([&] { feeder.join(); fed = true; });
    for (int blk = 0, after = 0; after < 6000 && !(fed && virtOut.size() >= vmsgs.size() + hmsgs.size()); ++blk) {
        if (fed) ++after;
        std::vector<Engine::MidiEvent> ev;
        if (blk % 3 == 0 && hi < hmsgs.size()) { ev.push_back({17, hmsgs[hi].data(), int(hmsgs[hi].size())}); ++hi; }
        e.process(l.data(), rr.data(), 256, ev.data(), int(ev.size()));
        for (const auto& m : e.hostMidiEvents()) {
            CHECK(m.offset >= 0 && m.offset < 256);
            hostOut.emplace_back(e.hostMidiBytes() + m.start, e.hostMidiBytes() + m.start + m.size);
        }
        while (e.virtualMidiOut().peekSize()) { Msg m; e.virtualMidiOut().pop([&](uint8_t x) { m.push_back(x); }); virtOut.push_back(m); }
    }
    watcher.join();
    CHECK(hi == hmsgs.size());
    // Both outputs carry the same echo; it holds each source's messages whole and in order.
    CHECK(virtOut == hostOut);
    size_t k[2] = {0, 0};
    bool ok = virtOut.size() == vmsgs.size() + hmsgs.size();
    for (const Msg& m : virtOut) {
        if (!ok) break;
        const int s = m[0] == 0xF0 ? m[1] : (m[0] & 0x0F);
        const std::vector<Msg>& want = s == 1 ? vmsgs : hmsgs;
        ok = (s == 0 || s == 1) && k[s] < want.size() && want[k[s]] == m;
        if (ok) ++k[s];
    }
    std::printf("midi_test: engine echo %zu messages (virtual %zu, host %zu), dropped in %llu out %llu\n", virtOut.size(),
                vmsgs.size(), hmsgs.size(), (unsigned long long)e.virtualInDropped(), (unsigned long long)e.virtualOutDropped());
    CHECK(ok);
    CHECK(e.virtualInDropped() == 0 && e.virtualOutDropped() == 0);

    // Off: no output to the port or the host.
    e.setVirtualPort(false); e.setMidiToHost(false);
    const uint8_t note[3] = {0x90, 0x30, 0x30};
    for (int blk = 0; blk < 100; ++blk) {
        Engine::MidiEvent ev{0, note, 3};
        e.process(l.data(), rr.data(), 256, &ev, blk == 0 ? 1 : 0);
        CHECK(e.hostMidiEvents().empty());
    }
    CHECK(e.virtualMidiOut().peekSize() == 0);
    fs::remove_all(dir);
}
// 6. The plugin window's bank load (v0.9.0, Engine::libraryInput): a bank-sized message goes into the MIDI port whole
// (the echo returns it as one message), the host's MIDI sent meanwhile waits behind it (never inside it), one at a
// time, busy until the OS has read its last byte, and the audio thread allocates nothing for it.
void testLibraryInput() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_midi_lib_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kEchoOut / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kEchoOut[i] >> 8); os[0x81 + 2 * i] = uint8_t(kEchoOut[i]); }
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    Engine e;
    e.setRoms((dir / "os").string(), (dir / "wave").string(), "os-md5", "wave-md5");
    for (int i = 0; i < 1000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(e.state() == Engine::State::Running);
    e.setVirtualPort(true); e.setMidiToHost(false);
    e.prepare(48000, 256);
    std::vector<float> l(256), rr(256);
    std::vector<Msg> out;
    auto block = [&](const Engine::MidiEvent* ev, int n, bool count) {
        g_countAllocs = count;
        e.process(l.data(), rr.data(), 256, ev, n);
        g_countAllocs = false;
        while (e.virtualMidiOut().peekSize()) { Msg m; e.virtualMidiOut().pop([&](uint8_t x) { m.push_back(x); }); out.push_back(m); }
    };
    const uint8_t first[3] = {0x90, 0x10, 0x01};
    // Warm up at the cable's full rate (a host SysEx), so the machine's own output buffers have their working size.
    Msg warm = {0xF0, 0x7D};
    for (int i = 0; i < 3000; ++i) warm.push_back(uint8_t(i & 0x7F));
    warm.push_back(0xF7);
    { Engine::MidiEvent ev{0, warm.data(), int(warm.size())}; block(&ev, 1, false); }
    for (int i = 0; i < 400; ++i) { Engine::MidiEvent ev{0, first, 3}; block(&ev, i % 10 == 0 ? 1 : 0, false); }
    // A bank message as the OS sends one: F0 0F 11 01 <dev> 0D <bank> ... F7, 102,420 bytes.
    Msg bank = {0xF0, 0x0F, 0x11, 0x01, 0x00, 0x0D, 0x01};
    while (bank.size() < Engine::kLibraryMessageMax - 1) bank.push_back(uint8_t(bank.size() & 0x7F));
    bank.push_back(0xF7);
    CHECK(!e.libraryInputBusy());
    CHECK(!e.libraryInput(bank.data(), Engine::kLibraryMessageMax + 1) && !e.libraryInput(bank.data(), 0));
    CHECK(e.libraryInput(bank.data(), bank.size()));
    CHECK(e.libraryInputBusy() && !e.libraryInput(bank.data(), bank.size()));   // one at a time
    out.clear();
    g_allocs = 0;
    std::vector<Msg> notes;
    int blocks = 0;
    for (; blocks < 20000 && e.libraryInputBusy(); ++blocks) {
        if (blocks % 50 == 1) notes.push_back({0x90, uint8_t(notes.size() & 0x7F), 0x40});
        Engine::MidiEvent ev{5, notes.empty() ? first : notes.back().data(), 3};
        block(&ev, blocks % 50 == 1 ? 1 : 0, true);
    }
    for (int i = 0; i < 400; ++i) block(nullptr, 0, true);   // the notes queued behind it come out
    const double seconds = blocks * 256.0 / 48000.0;
    std::printf("midi_test: library input: %zu bytes delivered whole in %.1f s of audio, %zu host notes behind it, %d allocations\n",
                bank.size(), seconds, notes.size(), g_allocs.load());
    CHECK(!e.libraryInputBusy());
    CHECK(seconds > 30 && seconds < 40);                     // at cable speed
    CHECK(g_allocs.load() == 0);
    CHECK(out.size() == notes.size() + 1);
    if (!out.empty()) CHECK(out[0] == bank);                  // whole, first
    bool inOrder = out.size() == notes.size() + 1;
    for (size_t i = 0; inOrder && i < notes.size(); ++i) inOrder = out[i + 1] == notes[i];
    CHECK(inOrder);
    CHECK(e.virtualInDropped() == 0 && e.virtualOutDropped() == 0);
    // Taken again once the first is through.
    CHECK(e.libraryInput(bank.data(), bank.size()));
    fs::remove_all(dir);
}

// 5. Pacing (B-08): the emulated MIDI port (serial channel B) passes received bytes to the OS no faster than the
// cable (31,250 baud: one byte per 320 us) and never drops one, however much arrives at once. A 68k program of our
// own stores every byte it reads; a 20,000-byte SysEx, 2,000 CC messages and 3,000 clock bytes are queued together.
const uint16_t kStore[] = {0x13FC, 0x0005, 0x00F0, 0x071A,   // move.b #$05,$F0071A
                           0x41F9, 0x0BE0, 0x0200,           // lea $0BE00200,a0
                           0x0839, 0x0000, 0x00F0, 0x0719,   // btst #0,$F00719
                           0x67F6,                           // beq.b btst
                           0x10F9, 0x00F0, 0x071B,           // move.b $F0071B,(a0)+
                           0x60EE};                          // bra.b btst
void testPacing() {
    OsImage os; os.bytes.assign(0x1000, 0);
    for (size_t i = 0; i < sizeof kStore / 2; ++i) { os.bytes[0x80 + 2 * i] = uint8_t(kStore[i] >> 8); os.bytes[0x81 + 2 * i] = uint8_t(kStore[i]); }
    Machine m; Machine::Config cfg; std::string err;
    CHECK(m.init(os, cfg, err));
    m.runUntil(m.cyclesFromMs(5));
    std::vector<uint8_t> in = {0xF0, 0x7D};
    for (int i = 0; i < 20000; ++i) in.push_back(uint8_t(i % 127));
    in.push_back(0xF7);
    Rng r{3};
    for (int i = 0; i < 2000; ++i) { in.push_back(0xB0); in.push_back(uint8_t(r.below(120))); in.push_back(uint8_t(r.below(128))); }
    for (int i = 0; i < 3000; ++i) in.push_back(0xF8);
    const uint64_t t0 = m.cycles();
    m.serial.queueRx(1, in, t0);
    const double perByte = 320e-6 * m.config().cpuHz;
    bool tooFast = false;
    for (int step = 1; step <= 1000 && m.serial.ch_[1].rxBytes < in.size(); ++step) {
        m.runUntil(t0 + m.cyclesFromMs(10.0 * step));
        const double maxBytes = double(m.cycles() - t0) / perByte + 1;   // the first byte at once, then one per 320 us
        if (double(m.serial.ch_[1].rxBytes) > maxBytes + 3) tooFast = true;
    }
    const double seconds = double(m.cycles() - t0) / m.config().cpuHz;
    std::vector<uint8_t> got(in.size());
    for (size_t i = 0; i < in.size(); ++i) got[i] = uint8_t(m.peek(0x0BE00200 + uint32_t(i), 1));
    std::printf("midi_test: pacing: %zu bytes queued at once reached the OS in %.2f s (%.0f bytes/s), none lost: %s\n",
                in.size(), seconds, double(in.size()) / seconds, got == in ? "yes" : "NO");
    CHECK(!tooFast);
    CHECK(got == in);
    CHECK(double(in.size()) / seconds <= 3126.0);
}
}  // namespace

int main() {
    testParser();
    testMerge();
    testRing();
    testEngine();
    testLibraryInput();
    testPacing();
    std::printf("midi_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
