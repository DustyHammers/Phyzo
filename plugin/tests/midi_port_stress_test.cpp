// Shutdown stress test of the virtual MIDI port (macOS CoreMIDI, no ROMs): while a client sends MIDI clock and notes
// to the current "Phyzo Stress" destination without pause, the test creates and destroys 200 engines with their
// port (as REAPER does when the plugin is added and removed). Closing must stop and drain CoreMIDI's callbacks before
// the engine goes away: the test passes when it does not crash and MIDI did reach the engines.
#include <CoreMIDI/CoreMIDI.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>
#include "VirtualMidiPort.h"
#include "engine.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {
std::atomic<MIDIEndpointRef> g_dest{0};
std::atomic<bool> g_run{true};
std::atomic<uint64_t> g_sent{0}, g_received{0};

void feed(void* engine, const uint8_t* b, size_t n) { g_received += n; static_cast<Engine*>(engine)->virtualMidiIn(b, n); }
}  // namespace

int main() {
    MIDIClientRef client = 0; MIDIPortRef out = 0;
    if (MIDIClientCreate(CFSTR("Phyzo stress test"), nullptr, nullptr, &client) != noErr ||
        MIDIOutputPortCreate(client, CFSTR("send"), &out) != noErr) { std::printf("FAIL: CoreMIDI client\n"); return 1; }

    std::thread sender([&] {
        std::vector<uint8_t> buf(1024);
        uint8_t note = 36;
        while (g_run) {
            const MIDIEndpointRef d = g_dest.load();
            if (d) {
                auto* list = reinterpret_cast<MIDIPacketList*>(buf.data());
                MIDIPacket* p = MIDIPacketListInit(list);
                const uint8_t clock[1] = {0xF8}, on[3] = {0x90, note, 100}, off[3] = {0x80, note, 0};
                p = MIDIPacketListAdd(list, buf.size(), p, 0, 1, clock);
                p = MIDIPacketListAdd(list, buf.size(), p, 0, 3, on);
                p = MIDIPacketListAdd(list, buf.size(), p, 0, 3, off);
                if (p && MIDISend(out, d, list) == noErr) g_sent += 7;   // a disposed destination just fails
                note = uint8_t(36 + (note - 35) % 48);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    for (int i = 0; i < 200; ++i) {
        auto engine = std::make_unique<Engine>();
        auto port = std::make_unique<VirtualMidiPort>(engine->virtualMidiOut(), &feed, engine.get());
        if (!port->open("Phyzo Stress")) { std::printf("FAIL: open: %s\n", port->error().c_str()); g_run = false; sender.join(); return 1; }
        g_dest = MIDIEndpointRef(port->destinationRef());
        std::this_thread::sleep_for(std::chrono::milliseconds(2 + i % 9));
        g_dest = 0;                                   // (the sender may still hold the old ref: that is the point)
        if (i % 2) port->close();                     // explicit close, or only the destructor
        port.reset();                                 // the port goes first, then the engine (as in the plugin)
        engine.reset();
    }
    g_run = false;
    sender.join();
    MIDIClientDispose(client);
    const bool ok = g_received.load() > 0;
    std::printf("midi_port_stress_test: %s (200 engines created and destroyed; %llu bytes sent, %llu reached the engines)\n",
                ok ? "passed" : "FAILED: no MIDI reached the engines", (unsigned long long)g_sent.load(), (unsigned long long)g_received.load());
    return ok ? 0 : 1;
}
