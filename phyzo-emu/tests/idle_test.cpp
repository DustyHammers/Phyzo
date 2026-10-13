// Idle runner test (F-63; no ROM data). A 68k program of our own stands in for the OS: it echoes the panel link
// (channel A) with every status byte + 0x10, so a button press 81 raw comes back as 91 raw (that LED on) and a release
// as 90 raw (off), and it echoes the MIDI port (channel B) byte for byte. Received bytes are stored in RAM (channel A
// from 0x0BE01000, channel B from 0x0BE00200).
//  1. Runner off (never enabled, or enabled and disabled again): a pause between host blocks changes nothing; output,
//     machine state and timing equal an engine processed without the pause.
//  2. Runner on and no host blocks: the machine runs; a queued button and knob reach it and the LEDs follow; a
//     virtual-port message goes in and its echo comes out; the editor's request goes in, a reply to it is taken out
//     for the editor, the standing read follows a program change; Audio In is silence; nothing reaches the host.
//  3. The next host block retimes and runs normally (sample-accurate MIDI, no late events); the runner stops.
//  4. getState while idling (the engine saves directly, as when the host stopped before), and the state restores.
//  5. The host stopping and starting again and again with a UI thread pressing buttons: every press arrives, the
//     longest wait for the engine lock is reported.
//  6. Not while a machine boots; destroying an engine while it idles is clean.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <vector>
#include "editor/editor_proto.h"
#include "engine.h"
#include "machine.h"

namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
using Msg = std::vector<uint8_t>;
using Clock = std::chrono::steady_clock;

// main() at 0x4080, checked with os_disasm.
const uint16_t kProgram[] = {
    0x13FC, 0x0005, 0x00F0, 0x0712,     // 4080 move.b #$05,$F00712    channel A: receiver and transmitter on
    0x13FC, 0x0005, 0x00F0, 0x071A,     // 4088 move.b #$05,$F0071A    channel B: the same
    0x41F9, 0x0BE0, 0x0200,             // 4090 lea    $0BE00200,a0
    0x43F9, 0x0BE0, 0x1000,             // 4096 lea    $0BE01000,a1
    0x0839, 0x0000, 0x00F0, 0x0711,     // 409C btst   #0,$F00711      A: byte received?
    0x6720,                             // 40A4 beq.b  $40C6
    0x1039, 0x00F0, 0x0713,             // 40A6 move.b $F00713,d0
    0x12C0,                             // 40AC move.b d0,(a1)+
    0x4A00,                             // 40AE tst.b  d0
    0x6A04,                             // 40B0 bpl.b  $40B6           data byte: as it is
    0x0600, 0x0010,                     // 40B2 addi.b #$10,d0         status byte: + 0x10
    0x0839, 0x0002, 0x00F0, 0x0711,     // 40B6 btst   #2,$F00711      A: transmitter ready?
    0x67F6,                             // 40BE beq.b  $40B6
    0x13C0, 0x00F0, 0x0713,             // 40C0 move.b d0,$F00713
    0x0839, 0x0000, 0x00F0, 0x0719,     // 40C6 btst   #0,$F00719      B: byte received?
    0x67CC,                             // 40CE beq.b  $409C
    0x1239, 0x00F0, 0x071B,             // 40D0 move.b $F0071B,d1
    0x10C1,                             // 40D6 move.b d1,(a0)+
    0x0839, 0x0002, 0x00F0, 0x0719,     // 40D8 btst   #2,$F00719      B: transmitter ready?
    0x67F6,                             // 40E0 beq.b  $40D8
    0x13C1, 0x00F0, 0x071B,             // 40E2 move.b d1,$F0071B
    0x60B2};                            // 40E8 bra.b  $409C

fs::path g_dir;

bool waitFor(const std::function<bool()>& f, int ms) {
    const auto end = Clock::now() + std::chrono::milliseconds(ms);
    while (!f()) {
        if (Clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return true;
}

struct Rig {
    Engine e;
    std::vector<float> audio;
    std::vector<Msg> hostOut, virtOut;
    explicit Rig(bool audioIn = false) {
        e.setRoms((g_dir / "os").string(), (g_dir / "wave").string(), "os-md5", "wave-md5");
        CHECK(waitFor([&] { return e.state() == Engine::State::Running; }, 20000));
        e.setMidiToHost(true);
        e.setVirtualPort(true);
        e.prepare(48000, 256, audioIn);
    }
    // One host block of 256 samples; Audio In (when on) a constant 0.25.
    void block(const std::vector<Msg>& host = {}, int offset = 10) {
        std::vector<Engine::MidiEvent> ev;
        for (size_t i = 0; i < host.size(); ++i) ev.push_back({offset + int(i), host[i].data(), int(host[i].size())});
        std::vector<float> l(256), r(256), in(256, 0.25f);
        e.process(l.data(), r.data(), 256, ev.data(), int(ev.size()), in.data(), in.data());
        audio.insert(audio.end(), l.begin(), l.end());
        audio.insert(audio.end(), r.begin(), r.end());
        for (const auto& m : e.hostMidiEvents()) hostOut.emplace_back(e.hostMidiBytes() + m.start, e.hostMidiBytes() + m.start + m.size);
        drainVirtual();
    }
    void drainVirtual() {
        while (e.virtualMidiOut().peekSize()) { Msg m; e.virtualMidiOut().pop([&](uint8_t b) { m.push_back(b); }); virtOut.push_back(m); }
    }
    uint64_t samples() { uint64_t s = 0; e.withMachineForTest([&](Machine& m) { s = m.samplesProduced(); }); return s; }
    // The bytes the program received on channel A so far (from its store in RAM).
    Msg panelBytes() {
        Msg v;
        e.withMachineForTest([&](Machine& m) {
            for (uint32_t a = 0x0BE01000; a < 0x0BE01400; ++a) v.push_back(uint8_t(m.peek(a, 1)));
        });
        while (!v.empty() && v.back() == 0) v.pop_back();
        return v;
    }
    bool sawVirtual(const Msg& m) { drainVirtual(); return std::find(virtOut.begin(), virtOut.end(), m) != virtOut.end(); }
};

const edproto::Address kRead{edproto::kSound, 0, 2, 0, 0x30, 0x20};
Msg reply(uint8_t marker) {
    std::vector<uint8_t> data(kRead.length, 0);
    data[0] = marker;
    Msg m = edproto::partialWrite(kRead, data);
    m[4] = 0x00; m[5] = edproto::kPartialReply;
    return m;
}
bool contains(const Msg& hay, const Msg& needle) { return std::search(hay.begin(), hay.end(), needle.begin(), needle.end()) != hay.end(); }
}  // namespace

int main() {
    g_dir = fs::temp_directory_path() / ("phyzo_idle_test_" + std::to_string(uint32_t(Clock::now().time_since_epoch().count())));
    fs::create_directories(g_dir);
    {
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kProgram / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kProgram[i] >> 8); os[0x81 + 2 * i] = uint8_t(kProgram[i]); }
        std::ofstream(g_dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(g_dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    const Msg note = {0x90, 0x3C, 0x40}, pc = {0xC0, 0x07}, read = edproto::partialRead(kRead);

    // 1. Runner off: a pause changes nothing.
    {
        Rig a, b, c;
        c.e.setIdleRun(true);
        c.e.setIdleRun(false);                       // its thread runs, but the runner is off
        auto feed = [&](Rig& r, int i) {
            if (i % 9 == 4) r.e.pressRaw(0x20 + i % 4, i % 2 == 0);
            if (i % 13 == 6) r.e.setControl(i % 20, (i * 37) % 1024);
            if (i % 17 == 9) r.e.virtualMidiIn(note.data(), note.size());
            r.block(i % 7 == 3 ? std::vector<Msg>{pc} : std::vector<Msg>{});
        };
        for (int i = 0; i < 100; ++i) { feed(a, i); feed(b, i); feed(c, i); }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));   // the host pauses (a and c only)
        for (int i = 100; i < 200; ++i) { feed(a, i); feed(c, i); }
        for (int i = 100; i < 200; ++i) feed(b, i);
        CHECK(a.audio == b.audio && c.audio == b.audio);
        CHECK(a.hostOut == b.hostOut && c.hostOut == b.hostOut && !b.hostOut.empty());
        CHECK(a.virtOut == b.virtOut && c.virtOut == b.virtOut && !b.virtOut.empty());
        CHECK(a.e.machineStateForTest() == b.e.machineStateForTest());
        CHECK(c.e.machineStateForTest() == b.e.machineStateForTest());
        CHECK(a.e.anchorForTest() == b.e.anchorForTest());
        CHECK(a.e.idleSlicesForTest() == 0 && c.e.idleSlicesForTest() == 0 && !c.e.idleRunning());
        std::printf("idle_test: runner off: output, MIDI and machine state identical with and without a 400 ms pause\n");
    }

    // 2-4. Runner on, the host stops.
    {
        Rig r(true);
        for (int i = 0; i < 40; ++i) r.block();
        int32_t inBefore = 0;
        r.e.withMachineForTest([&](Machine& m) { inBefore = m.esp2.serialIn[0]; });
        CHECK(inBefore != 0);                        // Audio In reached the ESP2's input
        const uint64_t anchor0 = r.e.anchorForTest(), s0 = r.samples();
        const size_t hostBefore = r.hostOut.size();
        r.e.setIdleRun(true);
        CHECK(waitFor([&] { return r.e.idleRunning(); }, 3000));
        const auto t0 = Clock::now();
        CHECK(waitFor([&] { return r.samples() > s0 + 44100 / 10; }, 3000));   // 100 ms of machine time
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        int32_t inIdle = 1;
        r.e.withMachineForTest([&](Machine& m) { inIdle = m.esp2.serialIn[0]; });
        CHECK(inIdle == 0);                          // Audio In: silence

        // a button and its LED (the program answers 81 raw with 91 raw)
        CHECK(r.e.led(0x21).mode == Engine::LedOff);
        r.e.pressRaw(0x21, true);
        CHECK(waitFor([&] { return r.e.led(0x21).mode == Engine::LedOn; }, 3000));
        r.e.pressRaw(0x21, false);
        CHECK(waitFor([&] { return r.e.led(0x21).mode == Engine::LedOff; }, 3000));
        // a knob: Bx cc vv reaches the program
        const uint64_t knobs = r.e.controlMessagesForTest();
        r.e.setControl(9, 777);                      // 777 = B6 09 09
        CHECK(waitFor([&] { return contains(r.panelBytes(), {0xB6, 0x09, 0x09}); }, 3000));
        CHECK(r.e.controlMessagesForTest() == knobs + 1);
        // the virtual port: in, and the echo out
        r.e.virtualMidiIn(note.data(), note.size());
        CHECK(waitFor([&] { return r.sawVirtual(note); }, 3000));
        // the editor: its request goes in (echoed: requests are not replies), a reply to it comes back to the editor
        // only, and a program change is followed by the standing read
        r.e.setEditorOpen(true);
        r.e.setEditorStandingRead({read});
        CHECK(r.e.editorSend(read.data(), read.size()));
        CHECK(waitFor([&] { return r.sawVirtual(read); }, 3000));
        const Msg rep = reply(0x55);
        r.e.virtualMidiIn(rep.data(), rep.size());
        std::vector<Msg> replies;
        CHECK(waitFor([&] {
            while (r.e.editorReplies().peekSize()) { Msg m; r.e.editorReplies().pop([&](uint8_t b) { m.push_back(b); }); replies.push_back(m); }
            return !replies.empty();
        }, 3000));
        CHECK(replies.size() == 1 && replies[0] == rep);
        CHECK(!r.sawVirtual(rep));                   // the reply went to the editor, not to the port
        // (a SysEx going in counts as a dump: the standing read followed the reply too)
        CHECK(waitFor([&] { r.drainVirtual(); return std::count(r.virtOut.begin(), r.virtOut.end(), read) == 2; }, 3000));
        r.virtOut.clear();
        r.e.virtualMidiIn(pc.data(), pc.size());
        CHECK(waitFor([&] { r.drainVirtual(); return r.virtOut.size() >= 2; }, 3000));
        CHECK(r.virtOut.size() >= 2 && r.virtOut[0] == pc && r.virtOut[1] == read);
        r.e.setEditorOpen(false);
        CHECK(r.e.idleRunning());
        std::printf("idle_test: idling: 100 ms of machine time in %.0f ms; button, LED, knob, virtual port, editor request, "
                    "reply and standing read all worked\n", wall * 1000);

        // 4. getState while idling: saved directly; it restores
        const auto g0 = Clock::now();
        const std::vector<uint8_t> blob = r.e.getState();
        const double gms = std::chrono::duration<double, std::milli>(Clock::now() - g0).count();
        CHECK(!blob.empty());
        CHECK(gms < 300);
        {
            Engine g;
            g.setState(blob);
            g.setRoms((g_dir / "os").string(), (g_dir / "wave").string(), "os-md5", "wave-md5");
            CHECK(waitFor([&] { return g.state() == Engine::State::Running; }, 20000));
            Msg bytes;
            g.withMachineForTest([&](Machine& m) { for (uint32_t a = 0x0BE01000; a < 0x0BE01400; ++a) bytes.push_back(uint8_t(m.peek(a, 1))); });
            CHECK(contains(bytes, {0xB6, 0x09, 0x09}));   // made while idling
            CHECK(g.controlPosition(9) == 777);
        }
        std::printf("idle_test: getState while idling took %.1f ms and restores\n", gms);

        // 3. the host is back: the first block retimes, MIDI is on time, the runner stops and stays off
        const uint64_t idleSamples = r.samples();
        r.block();
        CHECK(r.hostOut.size() == hostBefore);       // nothing of what the OS sent while idling reached the host
        const uint64_t anchor1 = r.e.anchorForTest();
        CHECK(anchor1 > anchor0 && anchor1 >= idleSamples);
        const uint64_t late0 = r.e.lateMidiForTest(), slices = r.e.idleSlicesForTest();
        std::vector<uint64_t> queued;
        int64_t pos = 256;
        for (int i = 0; i < 100; ++i) {
            const auto start = Clock::now();
            if (i == 20 || i == 60) {
                r.block({note}, 100);
                queued.push_back(r.e.midiQueueForTest().back());
                CHECK(r.e.anchorForTest() == anchor1);
                const double cps = r.e.cpuHzForTest() / 44100.0, ratio = 44100.0 / 48000.0;
                const double expect = double(anchor1) + double(pos + 100) * ratio;
                CHECK(std::fabs(double(queued.back()) / cps - expect - r.e.latencySamples() * ratio) <= 1.0);
            } else {
                r.block();
            }
            pos += 256;
            std::this_thread::sleep_until(start + std::chrono::microseconds(5333));   // real time
        }
        CHECK(r.e.lateMidiForTest() == late0);
        CHECK(!r.e.idleRunning());
        CHECK(r.e.idleSlicesForTest() == slices);    // none while the host plays
        CHECK(std::count(r.hostOut.begin(), r.hostOut.end(), note) == 2);   // the echo reaches the host again
        int32_t inBack = 0;
        r.e.withMachineForTest([&](Machine& m) { inBack = m.esp2.serialIn[0]; });
        CHECK(inBack != 0);                          // Audio In again
        std::printf("idle_test: host back: retimed, MIDI on time, runner stopped\n");
    }

    // 5. The host stops and starts, a UI thread presses buttons all along.
    {
        Rig r;
        r.e.setIdleRun(true);
        std::atomic<bool> run{true};
        std::atomic<int> presses{0};
        const uint64_t b0 = r.e.buttonMessagesForTest();
        std::thread ui([&] {
            while (run) { r.e.pressRaw(0x30 + presses % 8, presses % 2 == 0); ++presses; std::this_thread::sleep_for(std::chrono::milliseconds(3)); }
        });
        double worst = 0;
        int idled = 0;
        for (int round = 0; round < 6; ++round) {
            const auto end = Clock::now() + std::chrono::milliseconds(300);
            while (Clock::now() < end) {
                const auto start = Clock::now();
                std::vector<float> l(256), rr(256);
                r.e.process(l.data(), rr.data(), 256, nullptr, 0);
                std::this_thread::sleep_until(start + std::chrono::microseconds(5333));
            }
            worst = std::max(worst, r.e.maxLockWaitUs());
            std::this_thread::sleep_for(std::chrono::milliseconds(400));   // the host stops: the runner takes over
            idled += r.e.idleRunning() ? 1 : 0;
        }
        run = false;
        ui.join();
        CHECK(idled == 6);
        CHECK(waitFor([&] { return r.e.buttonMessagesForTest() == b0 + uint64_t(presses.load()); }, 3000));
        for (int i = 0; i < 4; ++i) { std::vector<float> l(256), rr(256); r.e.process(l.data(), rr.data(), 256, nullptr, 0); }
        worst = std::max(worst, r.e.maxLockWaitUs());
        CHECK(worst < 50000);                         // never a long wait (a slice is 10 ms of machine time)
        std::printf("idle_test: host stopping and starting 6 times, %d presses all delivered; longest wait for the engine "
                    "lock in a host block: %.0f us\n", presses.load(), worst);
    }

    // 6. Not while a machine boots; destroying an idling engine.
    {
        Rig r;
        r.e.setIdleRun(true);
        CHECK(waitFor([&] { return r.e.idleRunning(); }, 3000));
        r.e.setRoms((g_dir / "os").string(), (g_dir / "wave").string(), "other-os-md5", "wave-md5");   // boots again
        CHECK(r.e.state() == Engine::State::Booting);
        // (a slice already under way when the boot began ends on the machine that was running, then nothing)
        CHECK(waitFor([&] { return !r.e.idleRunning(); }, 3000));
        const uint64_t s1 = r.e.idleSlicesForTest();
        bool idleWhileBooting = false;
        while (r.e.state() == Engine::State::Booting) { idleWhileBooting |= r.e.idleRunning(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        CHECK(!idleWhileBooting);
        CHECK(r.e.idleSlicesForTest() == s1);
        CHECK(r.e.state() == Engine::State::Running);
        CHECK(waitFor([&] { return r.e.idleRunning(); }, 3000));   // and again once it runs
        for (int i = 0; i < 5; ++i) {
            const auto t0 = Clock::now();
            {
                Rig d;
                d.e.setIdleRun(true);
                CHECK(waitFor([&] { return d.e.idleRunning(); }, 3000));
                d.e.pressRaw(0x22, true);
            }
            CHECK(Clock::now() - t0 < std::chrono::seconds(30));
        }
        std::printf("idle_test: no idle steps while booting; 5 engines destroyed while idling\n");
    }

    fs::remove_all(g_dir);
    std::printf("idle_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
