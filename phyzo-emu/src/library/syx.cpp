#include "library/syx.h"

namespace library {

Kind classify(const std::vector<uint8_t>& m, int* bank) {
    // F0 0F 11 01 <device> <command> <type or bank> ... F7
    if (m.size() < 8 || m[0] != 0xF0 || m[1] != 0x0F || m[2] != 0x11 || m[3] != 0x01 || m.back() != 0xF7) return Kind::Other;
    if (m[5] == kPresetCommand && m[6] == kPresetType) return Kind::Preset;
    if (m[5] == kBankCommand && m[6] <= 1) {
        if (bank) *bank = m[6];
        return Kind::Bank;
    }
    return Kind::Other;
}

Split split(const std::vector<uint8_t>& file) {
    Split s;
    std::vector<uint8_t> cur;
    bool in = false;
    auto finish = [&] {
        int bank = 0;
        switch (classify(cur, &bank)) {
        case Kind::Preset: s.order.push_back(int(s.presets.size())); s.presets.push_back(cur); break;
        case Kind::Bank: s.order.push_back(-1 - int(s.banks.size())); s.banks.push_back({bank, cur}); break;
        case Kind::Other: ++s.other; break;
        }
        cur.clear();
        in = false;
    };
    for (const uint8_t b : file) {
        if (b == 0xF0) {
            if (in) ++s.truncated;                 // a new SysEx before the old one ended
            cur.assign(1, 0xF0);
            in = true;
        } else if (!in) {
            continue;                              // channel messages and running-status data outside SysEx
        } else if (b == 0xF7) {
            cur.push_back(b);
            finish();
        } else if (b >= 0xF8) {
            continue;                              // real-time bytes may be interleaved: not part of the message
        } else if (b & 0x80) {
            ++s.truncated;                         // another status byte: the SysEx was cut short
            cur.clear();
            in = false;
        } else {
            cur.push_back(b);
        }
    }
    if (in) ++s.truncated;
    return s;
}

}  // namespace library
