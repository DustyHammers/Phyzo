// A virtual MIDI port pair for one instance (macOS CoreMIDI): a destination other apps send to ("MIDI in" of the
// synth) and a source they receive from ("MIDI out"), both named "Phyzo", "Phyzo 2", "Phyzo 3", ... (the lowest
// number free in this process). An editor connects to them as to a hardware synth's MIDI ports.
//
// Received bytes go to `onBytes` on CoreMIDI's receive thread (one thread: the engine's single producer). Outgoing
// messages are taken from `out` (whole messages, filled by the audio thread) by a sender thread of the port's own.
// No JUCE here, so the loopback test can use it alone. Other systems: open() fails, nothing else happens.
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>
#include "midi_stream.h"

class VirtualMidiPort {
public:
    using Receiver = void (*)(void* user, const uint8_t* bytes, size_t n);
    VirtualMidiPort(MessageRing& out, Receiver onBytes, void* user);
    ~VirtualMidiPort();

    bool open(const std::string& baseName = "Phyzo");   // message thread; false if CoreMIDI refused
    void close();
    bool isOpen() const { return open_; }
    const std::string& name() const { return name_; }
    std::string error() const { return error_; }
    uint32_t destinationRef() const { return dest_; }      // CoreMIDI endpoint refs (tests)
    uint32_t sourceRef() const { return src_; }

    static void receive(void* self, const uint8_t* bytes, size_t n);   // from the CoreMIDI read proc

private:
    void senderLoop();

    MessageRing& out_;
    Receiver onBytes_;
    void* user_;
    bool open_ = false;
    int index_ = 0;
    std::string name_, error_;
    uint32_t dest_ = 0, src_ = 0;
    std::thread sender_;
    std::atomic<bool> run_{false};
    std::vector<uint8_t> msg_, packets_;
};
