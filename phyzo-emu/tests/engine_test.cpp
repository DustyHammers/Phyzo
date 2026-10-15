// Engine test (no ROM data): a small MIDI-echo program of our own stands in for the OS image, with an empty wave
// image. Checks booting on the worker thread, block processing at several host rates, sample-accurate MIDI timing,
// the reported latency, and state save/restore through the engine.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include "engine.h"

namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// main() at 0x4080: enable channel B, then store every received MIDI byte at 0x0BE00200, 0x0BE00201, ...
const uint16_t kEcho[] = {0x13FC, 0x0005, 0x00F0, 0x071A,   // move.b #$05,$F0071A
                          0x41F9, 0x0BE0, 0x0200,           // lea $0BE00200,a0
                          0x0839, 0x0000, 0x00F0, 0x0719,   // btst #0,$F00719
                          0x67F6,                           // beq.b btst
                          0x10F9, 0x00F0, 0x071B,           // move.b $F0071B,(a0)+
                          0x60EE};                          // bra.b btst

bool waitRunning(Engine& e) {
    for (int i = 0; i < 1000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return e.state() == Engine::State::Running;
}
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_engine_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kEcho / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kEcho[i] >> 8); os[0x81 + 2 * i] = uint8_t(kEcho[i]); }
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    const std::string osPath = (dir / "os").string(), wavePath = (dir / "wave").string();

    Engine e;
    CHECK(e.state() == Engine::State::NoRoms);
    e.setRoms(osPath, wavePath, "os-md5", "wave-md5");
    CHECK(waitRunning(e));

    for (double rate : {44100.0, 48000.0, 96000.0}) {
        e.prepare(rate, 512);
        const double r = 44100.0 / rate;
        CHECK(e.latencySamples() > 0 && e.latencySamples() < int(0.01 * rate));
        std::vector<float> l(512), rr(512);
        // Two events at known host samples, in different blocks: each must be queued at exactly
        // anchor + H * 44100/rate + (lookahead + margin), converted to CPU cycles.
        const uint8_t a = 0x90, b = 0x3C;
        const int64_t Ha = 1000, Hb = 5321;
        int64_t pos = 0;
        std::vector<uint64_t> queued;
        uint64_t anchor = 0;
        for (int blk = 0; blk < 40; ++blk) {
            const int n = 137 + blk % 3;                       // odd block sizes on purpose
            std::vector<Engine::MidiEvent> ev;
            if (Ha >= pos && Ha < pos + n) ev.push_back({int(Ha - pos), &a, 1});
            if (Hb >= pos && Hb < pos + n) ev.push_back({int(Hb - pos), &b, 1});
            e.process(l.data(), rr.data(), n, ev.data(), int(ev.size()));
            if (blk == 0) anchor = e.anchorForTest();
            if (!ev.empty()) { auto q = e.midiQueueForTest(); CHECK(!q.empty()); if (!q.empty()) queued.push_back(q.back()); }
            CHECK(e.lateMidiForTest() == 0);
            pos += n;
        }
        CHECK(queued.size() == 2);
        if (queued.size() == 2) {
            const double cps = e.cpuHzForTest() / 44100.0;
            const double lat = double(e.latencySamples());   // host samples
            auto expect = [&](int64_t H) { return double(anchor) + double(H) * r; };
            const double dA = double(queued[0]) / cps - expect(Ha), dB = double(queued[1]) / cps - expect(Hb);
            // same constant offset for both events (sample-accurate), within one CPU cycle
            CHECK(std::fabs((dA - dB) * cps) <= 1.0);
            // and that offset is what the engine reports as latency (within one host sample)
            CHECK(std::fabs(dA / r - lat) <= 1.0);
        }
        // the program received both bytes in order (bytes stored one after another)
        for (int blk = 0; blk < 30; ++blk) e.process(l.data(), rr.data(), 256, nullptr, 0);
        CHECK(e.peekForTest(0x0BE00200, 1) == 0x90 || rate != 44100.0);
    }

    // MIDI clock (F8, F-68) reaches the OS but is not "other MIDI" for the editor's arp edits; a note is.
    {
        std::vector<float> l(256), rr(256);
        const uint8_t clock = 0xF8, noteOn[3] = {0x90, 0x40, 0x40};
        const uint64_t before = e.externalMidi();
        Engine::MidiEvent ev[2] = {{0, &clock, 1}, {100, &clock, 1}};
        e.process(l.data(), rr.data(), 256, ev, 2);
        CHECK(e.externalMidi() == before);
        Engine::MidiEvent nv[1] = {{0, noteOn, 3}};
        e.process(l.data(), rr.data(), 256, nv, 1);
        CHECK(e.externalMidi() == before + 1);
        e.virtualMidiIn(&clock, 1);
        e.process(l.data(), rr.data(), 256, nullptr, 0);
        CHECK(e.externalMidi() == before + 1);
    }

    // Knob move (panel control): applied to the running machine and kept as the position.
    {
        CHECK(e.controlPosition(11) == 1023 && e.controlPosition(23) == 512);   // fresh positions
        e.setControl(11, 700);
        std::vector<float> l(256), rr(256);
        e.process(l.data(), rr.data(), 256, nullptr, 0);
        CHECK(e.controlPosition(11) == 700);
        CHECK(e.controlMessagesForTest() == 1);
    }

    // Knob moves are paced like the panel's serial link: per control the latest value, at most one message per
    // 10 ms, and none while the link is backed up. Buttons are never merged or dropped. (This program never reads
    // the panel port, so its bytes stay queued: 3 per knob message.)
    {
        std::vector<float> l(256), rr(256);
        auto block = [&] { e.process(l.data(), rr.data(), 256, nullptr, 0); };   // 2.7-5.8 ms of machine time
        for (int i = 0; i < 100; ++i) e.setControl(5, i);
        block();
        CHECK(e.controlMessagesForTest() == 2);                      // 100 moves -> 1 message
        CHECK(e.controlPosition(5) == 99);
        e.setControl(5, 500);
        block();
        CHECK(e.controlMessagesForTest() == 2);                      // within 10 ms of the last one: waits
        for (int i = 0; i < 8; ++i) block();
        CHECK(e.controlMessagesForTest() == 3);                      // then the latest value goes, once
        e.setControl(6, 100);
        block();
        CHECK(e.controlMessagesForTest() == 4);                      // another control: its own 10 ms
        e.setControl(7, 1);
        for (int i = 0; i < 8; ++i) block();
        CHECK(e.controlMessagesForTest() == 4);                      // 12 bytes unread: the knob waits
        const uint64_t b0 = e.buttonMessagesForTest();
        for (int i = 0; i < 300; ++i) e.pressRaw(0x10, i % 2 == 0);
        block();
        CHECK(e.buttonMessagesForTest() == b0 + 300);                // buttons always go, in order
    }

    // The host asking for the state while audio runs: served by the audio thread at a block boundary.
    {
        std::atomic<bool> run{true};
        std::thread audio([&] {
            std::vector<float> l(256), rr(256);
            while (run) { e.process(l.data(), rr.data(), 256, nullptr, 0); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<uint8_t> blob = e.getState();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        run = false;
        audio.join();
        CHECK(!blob.empty());
        CHECK(ms < 100);                                             // not the 500 ms fallback
        CHECK(e.maxLockWaitUs() >= 0);
        Engine g;
        g.setState(blob);
        g.setRoms(osPath, wavePath, "os-md5", "wave-md5");
        CHECK(waitRunning(g));
        CHECK(g.controlPosition(11) == 700);
    }

    // Engine state round trip: a second engine restored from the first matches it exactly.
    {
        const std::vector<uint8_t> blob = e.getState();
        const std::vector<uint8_t> snap = e.machineStateForTest();
        CHECK(!blob.empty());
        Engine f;
        f.setState(blob);                                       // before the ROMs are known: kept pending
        CHECK(f.state() == Engine::State::NoRoms);
        f.setRoms(osPath, wavePath, "os-md5", "wave-md5");
        CHECK(waitRunning(f));
        CHECK(f.machineStateForTest() == snap);
        CHECK(f.peekForTest(0x0BE00200, 2) == e.peekForTest(0x0BE00200, 2));
        CHECK(f.controlPosition(11) == 700);                    // knob positions come back with the project
        // a state made with other ROM files is not applied
        Engine g;
        g.setState(blob);
        g.setRoms(osPath, wavePath, "other-os", "wave-md5");
        CHECK(waitRunning(g));
        CHECK(g.machineStateForTest() != snap);
        CHECK(!g.message().empty());
    }
    fs::remove_all(dir);
    std::printf("engine_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
