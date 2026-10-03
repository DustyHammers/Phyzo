// Loopback test of the virtual MIDI port (macOS CoreMIDI, no ROMs). A CoreMIDI client of this test sends a note and
// a long SysEx to the port's destination and listens on its source: what goes in reaches the receiver byte for byte,
// and whole messages put in the outgoing ring come out of the source intact. A second port gets the next number.
#include <CoreMIDI/CoreMIDI.h>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>
#include "VirtualMidiPort.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

std::mutex mtx;
std::vector<uint8_t> intoPort, outOfPort;

void received(void*, const uint8_t* b, size_t n) { std::lock_guard<std::mutex> lk(mtx); intoPort.insert(intoPort.end(), b, b + n); }

void listen(const MIDIPacketList* list, void*, void*) {
    std::lock_guard<std::mutex> lk(mtx);
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) { outOfPort.insert(outOfPort.end(), p->data, p->data + p->length); p = MIDIPacketNext(p); }
}

bool waitFor(const std::vector<uint8_t>& v, size_t n) {
    for (int i = 0; i < 300; ++i) {
        { std::lock_guard<std::mutex> lk(mtx); if (v.size() >= n) return true; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
}  // namespace

int main() {
    MessageRing out(1 << 16);
    VirtualMidiPort port(out, &received, nullptr);
    if (!port.open("Phyzo Loopback")) { std::printf("FAIL: port did not open: %s\n", port.error().c_str()); return 1; }
    CHECK(port.name() == "Phyzo Loopback");
    {
        MessageRing out2(1024);
        VirtualMidiPort second(out2, &received, nullptr);
        CHECK(second.open("Phyzo Loopback") && second.name() == "Phyzo Loopback 2");
    }

    MIDIClientRef client = 0; MIDIPortRef sendPort = 0, listenPort = 0;
    CHECK(MIDIClientCreate(CFSTR("Phyzo loopback test"), nullptr, nullptr, &client) == noErr);
    CHECK(MIDIOutputPortCreate(client, CFSTR("send"), &sendPort) == noErr);
    CHECK(MIDIInputPortCreate(client, CFSTR("listen"), listen, nullptr, &listenPort) == noErr);
    CHECK(MIDIPortConnectSource(listenPort, MIDIEndpointRef(port.sourceRef()), nullptr) == noErr);

    // into the port: a note, then a 2000-byte SysEx in several packets
    std::vector<uint8_t> sent = {0x90, 0x3C, 0x64};
    std::vector<uint8_t> sysex = {0xF0, 0x7D};
    for (int i = 0; i < 2000; ++i) sysex.push_back(uint8_t(i & 0x7F));
    sysex.push_back(0xF7);
    std::vector<uint8_t> buf(8192);
    auto* list = reinterpret_cast<MIDIPacketList*>(buf.data());
    MIDIPacket* p = MIDIPacketListInit(list);
    p = MIDIPacketListAdd(list, buf.size(), p, 0, sent.size(), sent.data());
    for (size_t i = 0; i < sysex.size(); i += 200) p = MIDIPacketListAdd(list, buf.size(), p, 0, std::min<size_t>(200, sysex.size() - i), sysex.data() + i);
    CHECK(p != nullptr);
    CHECK(MIDISend(sendPort, MIDIEndpointRef(port.destinationRef()), list) == noErr);
    sent.insert(sent.end(), sysex.begin(), sysex.end());
    CHECK(waitFor(intoPort, sent.size()));
    { std::lock_guard<std::mutex> lk(mtx); CHECK(intoPort == sent); }

    // out of the port: whole messages from the ring, including a 5000-byte SysEx
    const uint8_t cc[3] = {0xB0, 0x47, 0x40};
    std::vector<uint8_t> dump = {0xF0, 0x0F};
    for (int i = 0; i < 5000; ++i) dump.push_back(uint8_t((i * 7) & 0x7F));
    dump.push_back(0xF7);
    CHECK(out.push(cc, 3) && out.push(dump.data(), dump.size()));
    std::vector<uint8_t> want(cc, cc + 3);
    want.insert(want.end(), dump.begin(), dump.end());
    CHECK(waitFor(outOfPort, want.size()));
    { std::lock_guard<std::mutex> lk(mtx); CHECK(outOfPort == want); }

    port.close();
    MIDIClientDispose(client);
    std::printf("midi_port_test: %s (in %zu bytes, out %zu bytes)\n", failures ? "FAILED" : "passed", sent.size(), want.size());
    return failures ? 1 : 0;
}
