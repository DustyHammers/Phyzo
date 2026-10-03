#include "VirtualMidiPort.h"
#include <chrono>
#include <mutex>
#include <set>

#if defined(__APPLE__)
#include <CoreMIDI/CoreMIDI.h>
// The MIDIPacketList calls (MIDI 1.0 byte streams) are what this port needs; their MIDI 2.0 successors would only
// add a conversion. They remain supported.
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

namespace {
std::mutex g_indexMtx;
std::set<int> g_used;                                 // port numbers in use in this process (1 = "Phyzo")

#if defined(__APPLE__)
MIDIClientRef client(std::string& error) {
    static MIDIClientRef c = 0;
    static OSStatus status = 0;
    static std::once_flag once;
    std::call_once(once, [] { status = MIDIClientCreate(CFSTR("Phyzo"), nullptr, nullptr, &c); });
    if (status != noErr) error = "CoreMIDI client error " + std::to_string(int(status));
    return status == noErr ? c : 0;
}

void readProc(const MIDIPacketList* list, void* refCon, void*) {
    const MIDIPacket* p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i) {
        VirtualMidiPort::receive(refCon, p->data, p->length);
        p = MIDIPacketNext(p);
    }
}

CFStringRef cfString(const std::string& s) { return CFStringCreateWithCString(kCFAllocatorDefault, s.c_str(), kCFStringEncodingUTF8); }
#endif
}  // namespace

VirtualMidiPort::VirtualMidiPort(MessageRing& out, Receiver onBytes, void* user) : out_(out), onBytes_(onBytes), user_(user) {
    msg_.reserve(size_t(1) << 20);
    packets_.resize(size_t(64) << 10);
}

VirtualMidiPort::~VirtualMidiPort() { close(); }

void VirtualMidiPort::receive(void* self, const uint8_t* bytes, size_t n) {
    auto* p = static_cast<VirtualMidiPort*>(self);
    p->onBytes_(p->user_, bytes, n);
}

bool VirtualMidiPort::open(const std::string& baseName) {
    if (open_) return true;
#if defined(__APPLE__)
    const MIDIClientRef c = client(error_);
    if (!c) return false;
    {
        std::lock_guard<std::mutex> lk(g_indexMtx);
        index_ = 1;
        while (g_used.count(index_)) ++index_;
        g_used.insert(index_);
    }
    name_ = index_ == 1 ? baseName : baseName + " " + std::to_string(index_);
    CFStringRef cf = cfString(name_);
    MIDIEndpointRef d = 0, s = 0;
    OSStatus st = MIDIDestinationCreate(c, cf, readProc, this, &d);
    if (st == noErr) st = MIDISourceCreate(c, cf, &s);
    CFRelease(cf);
    if (st != noErr) {
        if (d) MIDIEndpointDispose(d);
        error_ = "CoreMIDI could not create the port (error " + std::to_string(int(st)) + ")";
        std::lock_guard<std::mutex> lk(g_indexMtx);
        g_used.erase(index_);
        return false;
    }
    // Stable IDs per port number, so apps find the same port again next time (refused if another app holds them).
    const SInt32 base = 0x50687A00 + index_ * 2;      // "Phz" + number
    MIDIObjectSetIntegerProperty(d, kMIDIPropertyUniqueID, base);
    MIDIObjectSetIntegerProperty(s, kMIDIPropertyUniqueID, base + 1);
    MIDIObjectSetStringProperty(d, kMIDIPropertyManufacturer, CFSTR("DHammers"));
    MIDIObjectSetStringProperty(s, kMIDIPropertyManufacturer, CFSTR("DHammers"));
    dest_ = d; src_ = s;
    while (out_.peekSize()) out_.pop([](uint8_t) {});  // nothing old goes out of a new port
    open_ = true;
    run_ = true;
    sender_ = std::thread([this] { senderLoop(); });
    return true;
#else
    (void)baseName;
    error_ = "virtual MIDI ports need macOS";
    return false;
#endif
}

void VirtualMidiPort::close() {
    if (!open_) return;
    run_ = false;
    if (sender_.joinable()) sender_.join();
#if defined(__APPLE__)
    MIDIEndpointDispose(MIDIEndpointRef(dest_));
    MIDIEndpointDispose(MIDIEndpointRef(src_));
#endif
    dest_ = src_ = 0;
    open_ = false;
    std::lock_guard<std::mutex> lk(g_indexMtx);
    g_used.erase(index_);
}

// Sends whole messages from the ring as they come (checked every millisecond), in packets of up to 256 bytes (a long
// SysEx spans several packets, in order).
void VirtualMidiPort::senderLoop() {
#if defined(__APPLE__)
    auto* list = reinterpret_cast<MIDIPacketList*>(packets_.data());
    while (run_) {
        if (!out_.peekSize()) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        MIDIPacket* p = MIDIPacketListInit(list);
        while (out_.peekSize()) {
            msg_.clear();
            out_.pop([this](uint8_t b) { msg_.push_back(b); });
            for (size_t i = 0; i < msg_.size();) {
                const size_t n = std::min<size_t>(256, msg_.size() - i);
                p = MIDIPacketListAdd(list, packets_.size(), p, 0, n, msg_.data() + i);
                if (!p) {                             // list full: send it and start the next
                    MIDIReceived(MIDIEndpointRef(src_), list);
                    p = MIDIPacketListAdd(list, packets_.size(), MIDIPacketListInit(list), 0, n, msg_.data() + i);
                }
                i += n;
            }
        }
        MIDIReceived(MIDIEndpointRef(src_), list);
    }
#endif
}
