#include "editor_proto.h"

namespace edproto {

void put14(std::vector<uint8_t>& m, uint32_t v) {
    m.push_back(uint8_t(v & 0x7F));
    m.push_back(uint8_t((v >> 7) & 0x7F));
}
void put32(std::vector<uint8_t>& m, uint32_t v) {
    for (int i = 0; i < 4; ++i) m.push_back(uint8_t((v >> (7 * i)) & 0x7F));
    m.push_back(uint8_t((v >> 28) & 0x0F));
}
uint32_t get14(const uint8_t* p) { return uint32_t(p[0] & 0x7F) | uint32_t(p[1] & 0x7F) << 7; }
uint32_t get32(const uint8_t* p) {
    return uint32_t(p[0] & 0x7F) | uint32_t(p[1] & 0x7F) << 7 | uint32_t(p[2] & 0x7F) << 14 | uint32_t(p[3] & 0x7F) << 21 |
           uint32_t(p[4] & 0x0F) << 28;
}

namespace {
std::vector<uint8_t> header(uint8_t device, uint8_t command) { return {0xF0, kManufacturer, kModel, kVersion, device, command}; }
void putAddress(std::vector<uint8_t>& m, const Address& a) {
    m.push_back(uint8_t(a.type & 0x7F));
    put14(m, a.number);
    m.push_back(uint8_t(a.subtype & 0x7F));
    put14(m, a.subnumber);
    put32(m, a.offset);
    put32(m, a.length);
}
constexpr size_t kAddressBytes = 1 + 2 + 1 + 2 + 5 + 5;
}  // namespace

std::vector<uint8_t> partialRead(const Address& a, uint8_t device) {
    std::vector<uint8_t> m = header(device, kPartialRead);
    putAddress(m, a);
    m.push_back(0xF7);
    return m;
}

std::vector<uint8_t> partialWrite(const Address& a, const std::vector<uint8_t>& data, uint8_t device) {
    std::vector<uint8_t> m = header(device, kPartialWrite);
    Address h = a;
    h.length = uint32_t(data.size());
    putAddress(m, h);
    for (size_t i = 0; i < data.size(); i += 4) {
        uint32_t w = 0;
        for (size_t j = 0; j < 4; ++j) w = (w << 8) | (i + j < data.size() ? data[i + j] : 0);
        put32(m, w);
    }
    m.push_back(0xF7);
    return m;
}

std::vector<uint8_t> wholeRead(uint8_t type, uint16_t number, uint8_t device) {
    std::vector<uint8_t> m = header(device, kWholeRead);
    m.push_back(uint8_t(type & 0x7F));
    put14(m, number);
    m.push_back(0xF7);
    return m;
}

std::vector<std::vector<uint8_t>> nrpn(int channel, int msb, int lsb, int value) {
    const uint8_t st = uint8_t(0xB0 | (channel & 0x0F));
    const uint32_t v = uint32_t(value) & 0x3FFF;
    return {{st, 0x63, uint8_t(msb & 0x7F)}, {st, 0x62, uint8_t(lsb & 0x7F)}, {st, 0x06, uint8_t(v >> 7)},
            {st, 0x26, uint8_t(v & 0x7F)},   {st, 0x63, 0x7F},              {st, 0x62, 0x7F}};
}

void oscillatorNrpn(int sound, int osc, int param, int& msb, int& lsb) {
    msb = 2 + sound * 4 + osc * 2 + (param >> 7);
    lsb = param & 0x7F;
}

bool parse(const uint8_t* m, size_t n, Message& out) {
    if (n < 6 + kAddressBytes + 1 || m[0] != 0xF0 || m[1] != kManufacturer || m[2] != kModel || m[3] != kVersion ||
        m[n - 1] != 0xF7)
        return false;
    out.device = m[4];
    out.command = m[5];
    if (out.command != kPartialReply && out.command != kPartialWrite) return false;
    const uint8_t* p = m + 6;
    out.address.type = p[0];
    out.address.number = uint16_t(get14(p + 1));
    out.address.subtype = p[3];
    out.address.subnumber = uint16_t(get14(p + 4));
    out.address.offset = get32(p + 6);
    out.address.length = get32(p + 11);
    p += kAddressBytes;
    const size_t words = (size_t(m + n - 1 - p)) / 5;
    if (size_t(m + n - 1 - p) % 5 != 0) return false;
    out.data.clear();
    out.data.reserve(words * 4);
    for (size_t i = 0; i < words; ++i) {
        const uint32_t w = get32(p + 5 * i);
        for (int b = 3; b >= 0; --b) out.data.push_back(uint8_t(w >> (8 * b)));
    }
    if (out.data.size() < out.address.length) return false;
    out.data.resize(out.address.length);
    return true;
}

bool isOsSysex(const uint8_t* m, size_t n, uint8_t command) {
    return n >= 7 && m[0] == 0xF0 && m[1] == kManufacturer && m[2] == kModel && m[3] == kVersion && m[5] == command &&
           m[n - 1] == 0xF7;
}
bool isReply(const uint8_t* m, size_t n) { return isOsSysex(m, n, kPartialReply) || isOsSysex(m, n, kWholeReply); }
bool isRequest(const uint8_t* m, size_t n) { return isOsSysex(m, n, kPartialRead) || isOsSysex(m, n, kWholeRead); }

bool isPresetTrigger(const uint8_t* m, size_t n) {
    if (n == 0) return false;
    const uint8_t hi = m[0] & 0xF0;
    if (hi == 0xC0 && n >= 2) return true;                                   // program change
    if (hi == 0xB0 && n >= 3 && (m[1] == 0x00 || m[1] == 0x20)) return true;  // bank select MSB / LSB
    return m[0] == 0xF0;                                                     // SysEx into the OS (a dump)
}

}  // namespace edproto
