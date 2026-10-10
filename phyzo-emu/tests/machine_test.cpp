// Machine smoke test: needs no ROM data. A small 68k program of our own stands in for the OS image and exercises
// the CPU core (Musashi), RAM, serial channel B in both directions (the MIDI port) and the hard-stall detector.
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include "machine.h"
#include "os_image.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Loaded at 0x4000; main() at 0x4080 (os_profile.h). Assembled by hand, checked with os_disasm:
const uint16_t kProgram[] = {
    0x13FC, 0x0005, 0x00F0, 0x071A,     // 4080 move.b #$05,$F0071A    channel B: receiver and transmitter on
    0x7000,                             // 4088 moveq  #0,d0
    0x5280,                             // 408A addq.l #1,d0
    0x23C0, 0x0BE0, 0x0100,             // 408C move.l d0,$0BE00100
    0x0C80, 0x0000, 0x03E8,             // 4092 cmpi.l #1000,d0
    0x66F0,                             // 4098 bne    $408A
    0x13FC, 0x0090, 0x00F0, 0x071B,     // 409A move.b #$90,$F0071B    send one MIDI byte
    0x0839, 0x0000, 0x00F0, 0x0719,     // 40A2 btst   #0,$F00719      wait for a received byte
    0x67F6,                             // 40AA beq    $40A2
    0x1239, 0x00F0, 0x071B,             // 40AC move.b $F0071B,d1      read it
    0x13C1, 0x0BE0, 0x0200,             // 40B2 move.b d1,$0BE00200    store it
    0x60FE};                            // 40B8 bra    *

// Interrupt program: serial channel B receive interrupts (level 4, vector $40) through a vector table in RAM; the
// handler counts the bytes. Main loops counting in d2. Interrupts are raised while the devices advance (outside the
// CPU lock), which is the case the threaded test below covers.
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
}  // namespace

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

// The interrupt program with 60 received bytes, in steps of varying length. Returns the final state.
std::vector<uint8_t> runIrq(const OsImage& os, int& handled) {
    Machine m; Machine::Config cfg; std::string err;
    m.init(os, cfg, err);
    for (int i = 0; i < 300; ++i) {
        if (i % 5 == 2) m.serial.queueRx(1, {uint8_t(i)}, m.cycles() + uint64_t(31 * i));
        m.runUntil(m.cycles() + 900 + 53 * uint64_t(i % 17));
    }
    m.runUntil(m.cycles() + m.cyclesFromMs(2));
    handled = int(m.peek(0x0BE00400, 4));
    return m.saveState();
}

OsImage syntheticOs() {
    OsImage os;
    os.bytes.assign(0x1000, 0);
    for (size_t i = 0; i < sizeof kProgram / 2; ++i) {
        os.bytes[0x80 + 2 * i] = uint8_t(kProgram[i] >> 8);
        os.bytes[0x81 + 2 * i] = uint8_t(kProgram[i]);
    }
    return os;
}

// Runs the program to its end: counting loop, MIDI byte out, one byte in, stall. Returns the final state.
std::vector<uint8_t> runToEnd(Machine& m) {
    m.runUntil(m.cyclesFromMs(50), [&] { return m.peek(0x0BE00100, 4) == 1000; });
    m.runUntil(m.cycles() + m.cyclesFromMs(2));
    m.serial.queueRx(1, {0x3C}, m.cycles());
    m.runUntil(m.cycles() + m.cyclesFromMs(5), [&] { return m.peek(0x0BE00200, 1) == 0x3C; });
    m.runUntil(m.cycles() + m.cyclesFromMs(300));
    return m.saveState();
}

int main() {
    const OsImage os = syntheticOs();
    Machine m; Machine::Config cfg; std::string err;
    CHECK(m.init(os, cfg, err));

    // The counting loop finishes and its result is in RAM.
    auto counted = [&] { return m.peek(0x0BE00100, 4) == 1000; };
    CHECK(m.runUntil(m.cyclesFromMs(50), counted) == Machine::Stop::Predicate);

    // The transmitted byte leaves channel B one character time later.
    m.runUntil(m.cycles() + m.cyclesFromMs(2));
    CHECK(m.midiOut.size() == 1 && m.midiOut[0].second == 0x90);

    // A byte arriving on channel B is read by the program and stored.
    const uint64_t sent = m.cycles();
    m.serial.queueRx(1, {0x3C}, sent);
    auto stored = [&] { return m.peek(0x0BE00200, 1) == 0x3C; };
    CHECK(m.runUntil(sent + m.cyclesFromMs(5), stored) == Machine::Stop::Predicate);

    // The final `bra *` is reported as a hard stall at its address.
    CHECK(m.runUntil(m.cycles() + m.cyclesFromMs(1000)) == Machine::Stop::HardStall);
    CHECK(m.stallPc == 0x40B8);
    CHECK(m.unmapped.empty());

    // State round trip: save in the middle of the counting loop, restore into a new machine, run both to the end.
    {
        Machine a; CHECK(a.init(os, cfg, err));
        a.runUntil(a.cycles() + 7777);
        const uint32_t mid = a.peek(0x0BE00100, 4);
        CHECK(mid > 0 && mid < 1000);
        const std::vector<uint8_t> saved = a.saveState();
        Machine b; CHECK(b.init(os, cfg, err));
        CHECK(b.loadState(saved, err));
        CHECK(b.peek(0x0BE00100, 4) == mid && b.cycles() == a.cycles());
        const auto endA = runToEnd(a), endB = runToEnd(b);
        CHECK(endA == endB);
        CHECK(a.midiOut.size() == 1 && b.midiOut.size() == 1);
        // A damaged state is refused.
        std::vector<uint8_t> bad = saved; bad.resize(bad.size() / 2);
        Machine c; CHECK(c.init(os, cfg, err));
        CHECK(!c.loadState(bad, err));
    }

    // Several machines in one process (plugin instances) take turns on the single CPU core: interleaved runs must
    // end exactly as a machine that ran alone.
    {
        // (Same run steps for all three: the step boundaries themselves decide where the CPU pauses.)
        Machine solo; CHECK(solo.init(os, cfg, err));
        for (int i = 0; i < 40; ++i) solo.runUntil(solo.cycles() + 997 + 37 * i);
        const auto endSolo = runToEnd(solo);
        Machine x, y; CHECK(x.init(os, cfg, err)); CHECK(y.init(os, cfg, err));
        for (int i = 0; i < 40; ++i) {
            x.runUntil(x.cycles() + 997 + 37 * i);
            y.runUntil(y.cycles() + 997 + 37 * i);
        }
        const auto endY = runToEnd(y), endX = runToEnd(x);
        CHECK(endX == endSolo && endY == endSolo);
    }

    // Instances on several threads at once: each machine's devices run outside the CPU lock, in parallel with the
    // other machines; only 68k slices take turns. Interrupts raised by a device step reach the CPU under the lock.
    // Every machine must end exactly as one that ran alone.
    {
        const OsImage io = irqOs();
        int soloHandled = 0;
        const auto solo = runIrq(io, soloHandled);
        CHECK(soloHandled == 60);
        constexpr int kThreads = 3, kRounds = 4;
        std::vector<int> bad(kThreads, 0);
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t)
            threads.emplace_back([&, t] {
                for (int k = 0; k < kRounds; ++k) { int h = 0; if (runIrq(io, h) != solo || h != 60) ++bad[size_t(t)]; }
            });
        for (auto& th : threads) th.join();
        for (int t = 0; t < kThreads; ++t) CHECK(bad[size_t(t)] == 0);
    }

    // A saved state holds only what the machine set, never leftover memory: machines that start in memory filled with
    // different patterns and run the same save the same bytes, struct padding included, and a machine restored from
    // that state saves it again unchanged. (The ESP2's pending memory operation was copied whole with its two padding
    // bytes never set, so states of the same machine could differ in them.)
    {
        alignas(Machine) static unsigned char memA[sizeof(Machine)], memB[sizeof(Machine)], memC[sizeof(Machine)];
        std::memset(memA, 0xAA, sizeof memA); std::memset(memB, 0x55, sizeof memB); std::memset(memC, 0x3C, sizeof memC);
        Machine* a = new (memA) Machine();
        Machine* b = new (memB) Machine();
        Machine* c = new (memC) Machine();
        CHECK(a->init(os, cfg, err)); CHECK(b->init(os, cfg, err)); CHECK(c->init(os, cfg, err));
        a->runUntil(a->cycles() + 7777); b->runUntil(b->cycles() + 7777);
        const std::vector<uint8_t> sa = a->saveState(), sb = b->saveState();
        CHECK(sa == sb);
        CHECK(c->loadState(sa, err)); CHECK(c->saveState() == sa);
        a->~Machine(); b->~Machine(); c->~Machine();
    }

    std::printf("machine_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
