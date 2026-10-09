// The editor's MIDI messages to and from the OS (docs/editor/EDITOR_SPEC.md, "Write path" / "Read path"): SysEx partial
// reads and writes, whole-object reads, NRPN parameter messages, and the OS's replies. No allocation-free promises:
// these run on the message thread; the audio thread only uses the classification functions at the bottom.
//
// SysEx: F0 0F 11 01 <device> <command> ... F7. Numbers are sent 7 bits per byte, least significant first: a 14-bit
// number in 2 bytes, a 32-bit number in 5 (the last byte holds bits 28-31). Data is sent as 32-bit big-endian words,
// each encoded as a 32-bit number; a final partial word is padded with zeros.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace edproto {

constexpr uint8_t kManufacturer = 0x0F, kModel = 0x11, kVersion = 0x01;
constexpr uint8_t kAnyDevice = 0x7F;
// Commands
constexpr uint8_t kWholeRead = 0x01, kWholeDump = 0x02, kPartialRead = 0x04, kPartialWrite = 0x05;
constexpr uint8_t kWholeReply = 0x41, kPartialReply = 0x44;
// The OS's error message (F0 0F 11 01 <device> 7F ... F7): its answer to a request it cannot serve.
constexpr uint8_t kError = 0x7F;
// Object types
constexpr uint8_t kSystem = 0, kSound = 1, kPerformance = 2, kPreset = 7, kArpPreset = 8, kArpPattern = 9, kArpFeel = 10,
                  kArpRiff = 11, kRawMemory = 0x7F;
constexpr uint16_t kEditBuffer = 0x3FFF;             // performance edit buffer number
// Preset numbers (type kPreset): 0 = the edit buffer; 0x40 + n = stored preset n (the user's, in the user flash);
// 0x100 + n = the factory copy of preset n in the OS image (not what the user stored).
constexpr uint16_t kStoredPreset = 0x40;
constexpr uint16_t kFactoryPreset = 0x100;

// Where a partial read or write goes: object type and number, sub-object type and number, byte range.
struct Address {
    uint8_t type = 0;
    uint16_t number = 0;
    uint8_t subtype = 0;
    uint16_t subnumber = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
    bool operator==(const Address& o) const {
        return type == o.type && number == o.number && subtype == o.subtype && subnumber == o.subnumber &&
               offset == o.offset && length == o.length;
    }
};

void put14(std::vector<uint8_t>& m, uint32_t v);
void put32(std::vector<uint8_t>& m, uint32_t v);
uint32_t get14(const uint8_t* p);
uint32_t get32(const uint8_t* p);

std::vector<uint8_t> partialRead(const Address& a, uint8_t device = kAnyDevice);
std::vector<uint8_t> partialWrite(const Address& a, const std::vector<uint8_t>& data, uint8_t device = kAnyDevice);
std::vector<uint8_t> wholeRead(uint8_t type, uint16_t number, uint8_t device = kAnyDevice);
// A whole object into the OS (command 02; the layout of the OS's own 0x41 reply, docs/editor/EDITOR_ROUND2.md):
// F0 0F 11 01 <device> 02 <type> <number:14> <length:32> <data words> <checksum:14> F7. The data is the object as
// big-endian 32-bit words, each encoded as a 32-bit number (the last word padded with zeros); the checksum is the sum
// of the encoded data bytes & 0x3FFF.
std::vector<uint8_t> wholeDump(uint8_t type, uint16_t number, const std::vector<uint8_t>& object, uint8_t device = kAnyDevice);
// The checksum of a whole-object message's encoded data bytes (`n` bytes from `p`).
uint32_t dumpChecksum(const uint8_t* p, size_t n);
// A program change on `channel` (presets 0-63).
std::vector<uint8_t> programChange(int channel, int program);
// NRPN: Bn 63 msb, Bn 62 lsb, Bn 06 (value >> 7), Bn 26 (value & 7F), then the null Bn 63 7F, Bn 62 7F. The OS
// applies the value when CC 38 arrives; the value is 14 bits, two's complement (sign-extended from bit 13).
std::vector<std::vector<uint8_t>> nrpn(int channel, int msb, int lsb, int value);
// The OS's effect parameter message (docs/editor/EFFECTS_EDITOR_SPEC.md, write path 1): applies a value live,
// F0 0F 7F 7F 00 00 00 00 <slot> 00 <param> <value & 7F> <(value >> 7) & 7F> F7 (slot 0 routing, 1 insert, 3 reverb;
// the value 14 bits, two's complement). The OS does not clamp it.
std::vector<uint8_t> fxParameter(int slot, int param, int value);
// The preset edit buffer in RAM (raw-memory writes of effect values go to kEditBufferRam + preset offset).
constexpr uint32_t kEditBufferRam = 0x0BE0DA20;
// NRPN numbers: MSB 1 = preset-common parameter `param`; oscillator parameters MSB 2 + sound*4 + osc*2 + (param>>7).
void oscillatorNrpn(int sound, int osc, int param, int& msb, int& lsb);

// A decoded partial-read reply (command 0x44; also parses a partial write, 0x05, for tests): the header and the data
// bytes (the words unpacked big-endian, cut to the header's length).
struct Message {
    uint8_t device = 0, command = 0;
    Address address;
    std::vector<uint8_t> data;
};
bool parse(const uint8_t* m, size_t n, Message& out);
// A whole-object reply (command 0x41; also parses a dump, 02, for tests): type, number and the object's bytes. False if
// the layout or the checksum does not match.
bool parseWhole(const uint8_t* m, size_t n, uint8_t& type, uint16_t& number, std::vector<uint8_t>& object);

// ---- classification (allocation-free; used by the audio thread)
// F0 0F 11 01 <any device> <command>, F7-terminated.
bool isOsSysex(const uint8_t* m, size_t n, uint8_t command);
// An answer to a request: partial-read (0x44) or whole-object (0x41) reply, or the OS's error message (0x7F).
bool isReply(const uint8_t* m, size_t n);
// The address in a partial read (0x04) or its reply (0x44): type, number, sub-type, sub-number, offset, length; in a
// whole read (0x01) or its reply (0x41): type and number (the rest 0). False if the message is too short.
bool addressOf(const uint8_t* m, size_t n, Address& a);
// A request that the OS answers (partial read 0x04, whole read 0x01).
bool isRequest(const uint8_t* m, size_t n);
// A message into the OS after which the preset may have changed: program change, bank select (CC 0 / CC 32), or any
// SysEx (a dump).
bool isPresetTrigger(const uint8_t* m, size_t n);

}  // namespace edproto
