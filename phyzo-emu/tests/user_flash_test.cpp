// User flash test (no ROM data; v0.8.3): the boot ROM's flash-write service (vector slot 251) driven by small 68k
// programs of our own, the user flash in the machine state, seeding, the factory-restore sequence against a scripted
// stand-in for the OS, the factory cache file, and how the engine seeds new machines, old states and restarts.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <tuple>
#include <vector>
#include "engine.h"
#include "machine.h"
#include "os_image.h"
#include "state_io.h"
#include "user_flash.h"

namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

constexpr uint32_t kArgs = 0x0BE01000, kResult = 0x0BE01020, kDone = 0x0BE01024, kSource = 0x0BE10000;

// main() at 0x4080: calls the service through slot 251 as the OS does (C convention), with the arguments the test put
// at kArgs (destination, source, length, scratch, scratch length), then stores D0 and a done flag.
const uint16_t kCallService[] = {
    0x7055,                             // moveq  #$55,d0           (the result must come from the service)
    0x2F39, 0x0BE0, 0x1010,             // move.l $0BE01010,-(sp)   scratch length
    0x2F39, 0x0BE0, 0x100C,             // move.l $0BE0100C,-(sp)   scratch
    0x2F39, 0x0BE0, 0x1008,             // move.l $0BE01008,-(sp)   length
    0x2F39, 0x0BE0, 0x1004,             // move.l $0BE01004,-(sp)   source
    0x2F39, 0x0BE0, 0x1000,             // move.l $0BE01000,-(sp)   destination
    0x2078, 0x03EC,                     // movea.l ($03EC).w,a0     slot 251
    0x4E90,                             // jsr    (a0)
    0x4FEF, 0x0014,                     // lea    20(sp),sp
    0x23C0, 0x0BE0, 0x1020,             // move.l d0,$0BE01020
    0x13FC, 0x0001, 0x0BE0, 0x1024,     // move.b #1,$0BE01024
    0x60FE};                            // bra    *

// Plain CPU writes to flash: the user region, the service port (not from the service routine) and the OS image.
const uint16_t kPlainWrites[] = {
    0x13FC, 0x0012, 0x000D, 0x0000,     // move.b #$12,$000D0000
    0x23CF, 0x0000, 0x2040,             // move.l sp,($2040).l
    0x33FC, 0x1234, 0x000E, 0x0000,     // move.w #$1234,$000E0000
    0x13FC, 0x0077, 0x0000, 0x4100,     // move.b #$77,$00004100
    0x13FC, 0x0001, 0x0BE0, 0x1024,     // move.b #1,$0BE01024
    0x60FE};                            // bra    *

// For the engine: waits for a MIDI byte, copies it (and the 3 bytes after it in RAM) to 0xD0000 through the service,
// then asks the boot ROM for a restart (slot 1), as the OS does.
const uint16_t kWriteThenRestart[] = {
    0x13FC, 0x0005, 0x00F0, 0x071A,     // move.b #$05,$F0071A      channel B on
    0x0839, 0x0000, 0x00F0, 0x0719,     // btst   #0,$F00719        wait for a byte
    0x67F6,                             // beq.b  btst
    0x13F9, 0x00F0, 0x071B, 0x0BE0, 0x0010,   // move.b $F0071B,$0BE00010
    0x2F3C, 0x0000, 0x0010,             // move.l #16,-(sp)
    0x2F3C, 0x0BE0, 0x1000,             // move.l #$0BE01000,-(sp)
    0x2F3C, 0x0000, 0x0004,             // move.l #4,-(sp)
    0x2F3C, 0x0BE0, 0x0010,             // move.l #$0BE00010,-(sp)
    0x2F3C, 0x000D, 0x0000,             // move.l #$000D0000,-(sp)
    0x2078, 0x03EC,                     // movea.l ($03EC).w,a0
    0x4E90,                             // jsr    (a0)
    0x4FEF, 0x0014,                     // lea    20(sp),sp
    0x2078, 0x0004,                     // movea.l ($0004).w,a0     the restart service
    0x4ED0};                            // jmp    (a0)

OsImage image(const uint16_t* code, size_t words) {
    OsImage os;
    os.bytes.assign(0x1000, 0);
    for (size_t i = 0; i < words; ++i) { os.bytes[0x80 + 2 * i] = uint8_t(code[i] >> 8); os.bytes[0x81 + 2 * i] = uint8_t(code[i]); }
    return os;
}
const OsImage& serviceOs() { static const OsImage os = image(kCallService, sizeof kCallService / 2); return os; }

std::vector<uint8_t> flashOf(const Machine& m, uint32_t from = 0, uint32_t to = Machine::kFlashSize) {
    std::vector<uint8_t> f;
    for (uint32_t a = from; a < to; ++a) f.push_back(uint8_t(m.peek(a, 1)));
    return f;
}
uint8_t pattern(uint32_t i) { return uint8_t(i * 7 + 3); }

struct Call { uint32_t dst, src, len; };
// One service call on a fresh machine; returns D0. `m` is left as the call left it.
uint32_t callService(Machine& m, const Call& c) {
    Machine::Config cfg; std::string err;
    CHECK(m.init(serviceOs(), cfg, err));
    m.write(kArgs, c.dst, 4); m.write(kArgs + 4, c.src, 4); m.write(kArgs + 8, c.len, 4);
    m.write(kArgs + 12, 0x0BE30000 - 0x100, 4); m.write(kArgs + 16, 0x100, 4);
    for (uint32_t i = 0; i < 0x10000; ++i) m.write(kSource + i, pattern(i), 1);
    CHECK(m.runUntil(m.cyclesFromMs(50), [&] { return m.peek(kDone, 1) == 1; }) == Machine::Stop::Predicate);
    return m.peek(kResult, 4);
}

void testService() {
    // Into the user region: copied, D0 = 0, counted.
    {
        Machine m;
        const auto before = [] { Machine x; Machine::Config cfg; std::string err; x.init(serviceOs(), cfg, err); return flashOf(x); }();
        CHECK(callService(m, {0xD0000, kSource, 0x10000}) == 0);
        bool same = true;
        for (uint32_t i = 0; i < 0x10000; ++i) same &= m.peek(0xD0000 + i, 1) == pattern(i);
        CHECK(same);
        const auto after = flashOf(m);
        CHECK(std::equal(after.begin(), after.begin() + 0xD0000, before.begin()));        // nothing else changed
        CHECK(std::equal(after.begin() + 0xE0000, after.end(), before.begin() + 0xE0000));
        CHECK(m.flashServiceCalls == 1 && m.flashServiceBytes == 0x10000 && m.flashServiceRefused == 0);
        CHECK(m.flashWrites == 0);
        CHECK(m.unmapped.empty());
    }
    // Lengths ending exactly at 0x100000 (the last two regions the OS writes).
    {
        Machine m;
        CHECK(callService(m, {0xFFE00, kSource, 0x200}) == 0);
        CHECK(m.peek(0xFFE00, 1) == pattern(0) && m.peek(0xFFFFF, 1) == pattern(0x1FF));
        CHECK(m.peek(0xFFDFF, 1) == 0xFF);
        CHECK(m.flashServiceBytes == 0x200);
        Machine n;
        CHECK(callService(n, {0xF0000, kSource, 0x10000}) == 0);
        CHECK(n.peek(0xF0000, 1) == pattern(0) && n.peek(0xFFFFF, 1) == pattern(0xFFFF));
    }
    // Source in flash (the OS image): copied as well.
    {
        Machine m;
        CHECK(callService(m, {0xE0000, 0x4080, 0x40}) == 0);
        bool same = true;
        for (uint32_t i = 0; i < 0x40; ++i) same &= m.peek(0xE0000 + i, 1) == serviceOs().bytes[0x80 + i];
        CHECK(same);
    }
    // Outside the user region, or running past its end: nothing written, D0 non-zero, logged.
    for (const Call& c : {Call{0xCFF00, kSource, 0x200}, Call{0xFFF00, kSource, 0x200}, Call{0x4000, kSource, 0x10},
                          Call{0x100000, kSource, 0x10}, Call{0xD0000, kSource, 0x30001}, Call{0x0BE20000, kSource, 4}}) {
        Machine m;
        const auto before = [] { Machine x; Machine::Config cfg; std::string err; x.init(serviceOs(), cfg, err); return flashOf(x); }();
        CHECK(callService(m, c) != 0);
        CHECK(flashOf(m) == before);
        CHECK(m.flashServiceCalls == 1 && m.flashServiceBytes == 0 && m.flashServiceRefused == 1);
        CHECK(m.unmapped.count(std::make_tuple(c.dst, 'S', 4)) == 1);
    }
    // Every other flash write stays read-only, the service port included when written by anything but the routine.
    {
        const OsImage os = image(kPlainWrites, sizeof kPlainWrites / 2);
        Machine m; Machine::Config cfg; std::string err;
        CHECK(m.init(os, cfg, err));
        const auto before = flashOf(m);
        CHECK(m.runUntil(m.cyclesFromMs(5), [&] { return m.peek(kDone, 1) == 1; }) == Machine::Stop::Predicate);
        CHECK(flashOf(m) == before);
        CHECK(m.flashWrites == 4 && m.flashServiceCalls == 0);
    }
}

// A machine state without the user flash section (as saved before v0.8.3).
std::vector<uint8_t> withoutSection(const std::vector<uint8_t>& st, const char* drop) {
    if (st.size() < 8) return st;
    std::vector<uint8_t> out(st.begin(), st.begin() + 8);         // "PHZM", version
    size_t p = 8;
    while (p + 12 <= st.size()) {
        uint64_t len; std::memcpy(&len, &st[p + 4], 8);
        if (len > st.size() - p - 12) break;
        if (std::memcmp(&st[p], drop, 4) != 0) out.insert(out.end(), st.begin() + long(p), st.begin() + long(p + 12 + len));
        p += 12 + size_t(len);
    }
    return out;
}
bool hasSection(const std::vector<uint8_t>& st, const char* tag) { return withoutSection(st, tag).size() != st.size(); }

void testState() {
    Machine::Config cfg; std::string err;
    Machine a;
    CHECK(callService(a, {0xD0000, kSource, 0x10000}) == 0);
    const std::vector<uint8_t> saved = a.saveState();
    CHECK(hasSection(saved, "UFLS"));
    // Round trip with the section: byte-identical, user flash and counters included.
    {
        Machine b; CHECK(b.init(serviceOs(), cfg, err));
        int seeded = 0;
        b.seedUserFlash = [&](Machine&) { ++seeded; };
        CHECK(b.loadState(saved, err));
        CHECK(seeded == 0);                                      // the state has its own user flash
        CHECK(b.saveState() == saved);
        CHECK(std::memcmp(b.userFlash(), a.userFlash(), Machine::kUserFlashSize) == 0);
        CHECK(b.flashServiceCalls == 1 && b.flashServiceBytes == 0x10000);
    }
    // Without the section: loads as before, then the seeding hook gets the loaded machine.
    {
        const std::vector<uint8_t> old = withoutSection(saved, "UFLS");
        CHECK(old.size() + 12 + 8 * 4 + Machine::kUserFlashSize == saved.size());
        Machine b; CHECK(b.init(serviceOs(), cfg, err));
        int seeded = 0;
        std::vector<uint8_t> seed(Machine::kUserFlashSize);
        for (size_t i = 0; i < seed.size(); ++i) seed[i] = uint8_t(i ^ 0x5A);
        b.seedUserFlash = [&](Machine& m) { ++seeded; CHECK(&m == &b); CHECK(m.cycles() == a.cycles()); m.setUserFlash(seed.data(), seed.size()); };
        CHECK(b.loadState(old, err));
        CHECK(seeded == 1);
        CHECK(std::memcmp(b.userFlash(), seed.data(), seed.size()) == 0);
        CHECK(withoutSection(b.saveState(), "UFLS") == old);     // everything else exactly as saved
        // No hook: the user flash stays erased.
        Machine c; CHECK(c.init(serviceOs(), cfg, err));
        CHECK(c.loadState(old, err));
        const auto uf = flashOf(c, Machine::kUserFlashBase);
        CHECK(std::all_of(uf.begin(), uf.end(), [](uint8_t v) { return v == 0xFF; }));
    }
    // A damaged user flash section is refused.
    {
        std::vector<uint8_t> bad = saved;
        const std::vector<uint8_t> old = withoutSection(saved, "UFLS");
        bad.resize(old.size() + 12 + 8 * 4 + 1000);             // cut inside the flash bytes
        uint64_t len = bad.size() - old.size() - 12; std::memcpy(&bad[old.size() + 4], &len, 8);
        Machine b; CHECK(b.init(serviceOs(), cfg, err));
        CHECK(!b.loadState(bad, err));
    }
}

// Seeding before boot changes the user flash and nothing else.
void testSeeding() {
    Machine::Config cfg; std::string err;
    Machine plain, seeded;
    CHECK(plain.init(serviceOs(), cfg, err)); CHECK(seeded.init(serviceOs(), cfg, err));
    std::vector<uint8_t> seed(Machine::kUserFlashSize);
    for (size_t i = 0; i < seed.size(); ++i) seed[i] = uint8_t(i * 13 + 1);
    CHECK(!seeded.setUserFlash(seed.data(), seed.size() - 1));   // wrong size: refused
    CHECK(seeded.setUserFlash(seed.data(), seed.size()));
    CHECK(flashOf(seeded, 0, Machine::kUserFlashBase) == flashOf(plain, 0, Machine::kUserFlashBase));
    CHECK(flashOf(seeded, Machine::kUserFlashBase) == seed);
    CHECK(seeded.saveState() != plain.saveState());
    CHECK(withoutSection(seeded.saveState(), "UFLS") == withoutSection(plain.saveState(), "UFLS"));
}

// ------------------------------------------------------------------ the factory-restore sequence

// A scripted stand-in for the OS's panel behaviour (times in ms): Save held and Compare pressed and released, then Save
// released, makes it ask `r5t?` after confirmMs; +/Yes then shows `burn` after burnMs for writeMs, then `P 01` again.
// It writes `bytes` from burn on, all of them by bytesMs (default: 90 % of writeMs).
struct FakeOs : userflash::Panel {
    double now = 0;
    double confirmMs = 30, burnMs = 40, writeMs = 340, bytesMs = -1;
    uint64_t bytes = userflash::kFactoryRestoreBytes;
    bool ignoreCompare = false, stopAtYes = false;
    bool saveHeld = false, compared = false;
    double confirmAt = -1, burnAt = -1;
    std::vector<std::tuple<double, uint8_t, bool>> presses;

    std::string text() const {
        if (burnAt >= 0 && now >= burnAt + writeMs) return "P 01";
        if (burnAt >= 0 && now >= burnAt) return "burn";
        if (confirmAt >= 0 && now >= confirmAt) return "r5t?";
        return "P 01";
    }
    void press(uint8_t raw, bool down) override {
        presses.emplace_back(now, raw, down);
        if (raw == userflash::kRawSave) {
            if (down) saveHeld = true;
            else { saveHeld = false; if (compared) confirmAt = now + confirmMs; }
        }
        if (raw == userflash::kRawCompare && down && saveHeld && !ignoreCompare) compared = true;
        if (raw == userflash::kRawYes && down && text() == "r5t?") burnAt = now + burnMs;
    }
    Run run(double ms, const std::function<bool(const std::string&)>& until) override {
        if (stopAtYes && burnAt >= 0) return Run::Stopped;
        for (double end = now + ms; now < end;) {
            now += 1;
            if (until && until(text())) return Run::Shown;
        }
        return Run::Time;
    }
    uint64_t serviceBytes() const override {
        if (burnAt < 0 || now < burnAt) return 0;
        const double span = bytesMs >= 0 ? bytesMs : writeMs * 0.9;
        const double f = span <= 0 ? 1.0 : std::min(1.0, (now - burnAt) / span);
        return uint64_t(double(bytes) * f);
    }
};

void testFactoryRestore() {
    using userflash::kRawSave; using userflash::kRawCompare; using userflash::kRawYes;
    // The owner's sequence and timing; +/Yes as soon as `r5t?` shows; done the moment `P 01` is back.
    {
        FakeOs os; std::string err;
        CHECK(userflash::factoryRestore(os, err));
        CHECK(err.empty());
        using P = std::tuple<double, uint8_t, bool>;
        const std::vector<P> want = {P{0, kRawSave, true}, P{200, kRawCompare, true}, P{350, kRawCompare, false},
                                     P{550, kRawSave, false}, P{580, kRawYes, true}, P{690, kRawYes, false}};
        CHECK(os.presses == want);
        CHECK(os.now == os.burnAt + os.writeMs);                // no fixed delay after the display came back
    }
    // A slow OS: every display is waited for, however long it takes (within the limits).
    {
        FakeOs os; os.confirmMs = 1500; os.burnMs = 2500; os.writeMs = 4000; std::string err;
        CHECK(userflash::factoryRestore(os, err));
        CHECK(std::get<0>(os.presses[4]) == 550 + 1500);
        CHECK(os.now == os.burnAt + os.writeMs);
    }
    // `burn` already while +/Yes is held.
    {
        FakeOs os; os.burnMs = 20; std::string err;
        CHECK(userflash::factoryRestore(os, err));
        CHECK(os.now == os.burnAt + os.writeMs);
    }
    // Writing so fast that `burn` never reaches the display: done when the display is back and all bytes are written.
    {
        FakeOs os; os.writeMs = 0; std::string err;
        CHECK(userflash::factoryRestore(os, err));
        CHECK(os.now == 691);                                   // the first moment after +/Yes is released
    }
    // The display comes back before the last write: done only once every byte is written.
    {
        FakeOs os; os.bytesMs = os.writeMs + 50; std::string err;
        CHECK(userflash::factoryRestore(os, err));
        CHECK(os.now == os.burnAt + os.writeMs + 50);
    }
    // No confirmation: +/Yes is never pressed.
    {
        FakeOs os; os.ignoreCompare = true; std::string err;
        CHECK(!userflash::factoryRestore(os, err));
        CHECK(err.find("r5t?") != std::string::npos);
        CHECK(os.presses.size() == 4);
    }
    // Not the expected amount written: no factory user flash.
    {
        FakeOs os; os.bytes = 1000; std::string err;
        CHECK(!userflash::factoryRestore(os, err));
        CHECK(err.find("1000") != std::string::npos);
    }
    // The OS stops.
    {
        FakeOs os; os.stopAtYes = true; std::string err;
        CHECK(!userflash::factoryRestore(os, err));
        CHECK(!err.empty());
    }
}

void testCache(const fs::path& dir) {
    const std::string md5 = "0123456789abcdef0123456789abcdef", other = "fedcba9876543210fedcba9876543210";
    std::vector<uint8_t> data(Machine::kUserFlashSize);
    for (size_t i = 0; i < data.size(); ++i) data[i] = uint8_t(i * 31 + 7);
    const std::string path = userflash::cachePath((dir / "cache").string(), md5);
    CHECK(path.find(md5) != std::string::npos);
    std::vector<uint8_t> got;
    CHECK(!userflash::readCache(path, md5, got));               // none yet
    CHECK(userflash::writeCache(path, md5, data));            // makes the folder
    CHECK(userflash::readCache(path, md5, got) && got == data);
    CHECK(!userflash::readCache(path, other, got));             // another OS image
    CHECK(!userflash::writeCache(path, md5, std::vector<uint8_t>(100)));   // wrong size
    std::vector<char> raw;
    { std::ifstream f(path, std::ios::binary); raw.assign(std::istreambuf_iterator<char>(f), {}); }
    auto writeRaw = [&](const std::vector<char>& b) { std::ofstream(path, std::ios::binary).write(b.data(), long(b.size())); };
    auto damaged = raw; damaged[1000] ^= 1;
    writeRaw(damaged);
    CHECK(!userflash::readCache(path, md5, got));               // damaged
    writeRaw(std::vector<char>(raw.begin(), raw.end() - 10));
    CHECK(!userflash::readCache(path, md5, got));               // cut short
    writeRaw(raw);
    CHECK(userflash::readCache(path, md5, got) && got == data);
    for (const auto& e : fs::directory_iterator(dir / "cache")) CHECK(e.path().string() == path);   // no temporary left
}

// ------------------------------------------------------------------ the engine

bool waitRunning(Engine& e) {
    for (int i = 0; i < 1000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return e.state() == Engine::State::Running;
}
std::vector<uint8_t> userFlashOf(Engine& e) {
    std::vector<uint8_t> f;
    e.withMachineForTest([&](Machine& m) { f.assign(m.userFlash(), m.userFlash() + Machine::kUserFlashSize); });
    return f;
}
bool erased(const std::vector<uint8_t>& f) {
    return f.size() == Machine::kUserFlashSize && std::all_of(f.begin(), f.end(), [](uint8_t v) { return v == 0xFF; });
}
// The engine blob with the user flash section taken out of its machine state (a project saved before v0.8.3).
std::vector<uint8_t> oldBlob(const std::vector<uint8_t>& blob) {
    StateReader r(blob.data(), blob.size());
    char magic[4]; r.raw(magic, 4);
    const uint32_t version = r.get<uint32_t>();
    std::string os, wave; std::vector<uint8_t> machine;
    r.str(os); r.str(wave); r.vec(machine);
    CHECK(r.ok());
    StateWriter w;
    w.raw(magic, 4); w.put(version); w.str(os); w.str(wave); w.vec(withoutSection(machine, "UFLS"));
    return w.bytes;
}

void testEngine(const fs::path& dir) {
    {
        const OsImage os = image(kWriteThenRestart, sizeof kWriteThenRestart / 2);
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.bytes.data()), long(os.bytes.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    const std::string osPath = (dir / "os").string(), wavePath = (dir / "wave").string();
    auto factory = std::make_shared<std::vector<uint8_t>>(Machine::kUserFlashSize);
    for (size_t i = 0; i < factory->size(); ++i) (*factory)[i] = uint8_t(i * 3 + 11);
    Engine::setFactoryUserFlashForTest("os-factory", factory);
    const uint64_t runs = Engine::factoryRunsForTest();

    // Off (the default): erased user flash, as before v0.8.3.
    std::vector<uint8_t> offState;
    {
        Engine e;
        e.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(e));
        CHECK(erased(userFlashOf(e)));
        offState = e.getState();
    }
    // A new machine is seeded before it boots.
    {
        Engine e;
        e.setFactoryUserFlash(true, std::string());
        e.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(e));
        CHECK(userFlashOf(e) == *factory);
        // Booted like the unseeded machine: the same state but for the user flash.
        Engine off;
        off.setState(offState);
        off.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(off));
        CHECK(withoutSection(off.machineStateForTest(), "UFLS") == withoutSection(e.machineStateForTest(), "UFLS"));
    }
    // A project with the user flash keeps its own (here the erased one of the engine that was off).
    {
        Engine e;
        e.setFactoryUserFlash(true, std::string());
        e.setState(offState);
        e.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(e));
        CHECK(erased(userFlashOf(e)));
    }
    // A project saved before v0.8.3: restored as saved, then given the factory user flash.
    {
        const std::vector<uint8_t> old = oldBlob(offState);
        Engine ref;
        ref.setState(offState);
        ref.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(ref));
        Engine e;
        e.setFactoryUserFlash(true, std::string());
        e.setState(old);
        e.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(e));
        CHECK(userFlashOf(e) == *factory);
        CHECK(withoutSection(e.machineStateForTest(), "UFLS") == withoutSection(ref.machineStateForTest(), "UFLS"));
        // Factory off: it stays erased.
        Engine f;
        f.setState(old);
        f.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(f));
        CHECK(erased(userFlashOf(f)));
    }
    CHECK(Engine::factoryRunsForTest() == runs);                 // no hidden machine was needed
    // The OS writes its flash and restarts itself: the new machine boots with that user flash, not the factory's.
    {
        Engine e;
        e.setFactoryUserFlash(true, std::string());
        e.setRoms(osPath, wavePath, "os-factory", "wave");
        CHECK(waitRunning(e));
        e.prepare(44100, 256);
        std::vector<float> l(256), r(256);
        const uint8_t note = 0x90;
        const Engine::MidiEvent ev{0, &note, 1};
        e.process(l.data(), r.data(), 256, &ev, 1);
        for (int i = 0; i < 200 && e.state() == Engine::State::Running; ++i) e.process(l.data(), r.data(), 256, nullptr, 0);
        CHECK(e.state() == Engine::State::Stopped);
        e.service();
        CHECK(waitRunning(e));
        std::vector<uint8_t> want = *factory;
        want[0] = 0x90; want[1] = want[2] = want[3] = 0;          // the byte and the RAM after it
        CHECK(userFlashOf(e) == want);
    }
    // The factory user flash comes from the cache file when there is one (no hidden machine).
    {
        const std::string md5 = "00112233445566778899aabbccddeeff";
        std::vector<uint8_t> cached(Machine::kUserFlashSize, 0x42);
        CHECK(userflash::writeCache(userflash::cachePath((dir / "cache2").string(), md5), md5, cached));
        Engine e;
        e.setFactoryUserFlash(true, (dir / "cache2").string());
        e.setRoms(osPath, wavePath, md5, "wave");
        CHECK(waitRunning(e));
        CHECK(userFlashOf(e) == cached);
        CHECK(Engine::factoryRunsForTest() == runs);
    }
    // It cannot be made (this OS has no factory restore): said once, the synth still starts (erased), and no other
    // engine in the process tries again. Nothing is cached.
    {
        const std::string md5 = "ffeeddccbbaa99887766554433221100";
        Engine e;
        e.setFactoryUserFlash(true, (dir / "cache3").string());
        e.setRoms(osPath, wavePath, md5, "wave");
        CHECK(waitRunning(e));
        CHECK(Engine::factoryRunsForTest() == runs + 1);
        CHECK(e.message().find("could not be made") != std::string::npos);
        CHECK(erased(userFlashOf(e)));
        Engine f;
        f.setFactoryUserFlash(true, (dir / "cache3").string());
        f.setRoms(osPath, wavePath, md5, "wave");
        CHECK(waitRunning(f));
        CHECK(Engine::factoryRunsForTest() == runs + 1);
        CHECK(!fs::exists(userflash::cachePath((dir / "cache3").string(), md5)));
    }
    // While a project is restored (here, an old one whose factory user flash is being made first), the host still
    // gets the project's state, never an empty one.
    {
        const std::string md5 = "0f1e2d3c4b5a69788796a5b4c3d2e1f0";
        Engine off;
        off.setRoms(osPath, wavePath, md5, "wave");
        CHECK(waitRunning(off));
        const std::vector<uint8_t> old = oldBlob(off.getState());
        Engine e;
        e.setFactoryUserFlash(true, std::string());
        e.setState(old);
        e.setRoms(osPath, wavePath, md5, "wave");
        CHECK(e.state() == Engine::State::Booting);
        CHECK(e.getState() == old);
        CHECK(waitRunning(e));
        CHECK(Engine::factoryRunsForTest() == runs + 2);
        CHECK(e.getState() != old);                              // running: its own state again
    }
}
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_user_flash_test";
    fs::remove_all(dir); fs::create_directories(dir);
    testService();
    testState();
    testSeeding();
    testFactoryRestore();
    testCache(dir);
    testEngine(dir);
    fs::remove_all(dir);
    std::printf("user_flash_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
