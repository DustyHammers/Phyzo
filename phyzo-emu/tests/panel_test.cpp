#include <array>
// Panel model test (no ROM data): the answer to the OS's F4 request (docs/PANEL_CONTROLS.md, F-02), knob moves,
// and the control positions in the saved state.
#include <cstdio>
#include <vector>
#include "panel_model.h"
#include "state_io.h"

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

struct Sent { std::vector<uint8_t> bytes; uint64_t when; };

std::vector<uint8_t> encode(const uint16_t* raw) {
    std::vector<uint8_t> b;
    for (int cc = 0; cc < 26; ++cc) { b.push_back(uint8_t(0xB0 | ((raw[cc] >> 7) & 7))); b.push_back(uint8_t(cc)); b.push_back(uint8_t(raw[cc] & 0x7F)); }
    return b;
}
}  // namespace

int main() {
    // Fresh-instance raw values (F-03), in cc order.
    const uint16_t fresh[26] = {102, 508, 698, 2, 512, 2, 516, 512, 5, 2, 4, 1023, 2, 516, 2, 516, 3, 3, 512, 3, 512, 3, 516, 512, 0, 0};
    for (int i = 0; i < 26; ++i) CHECK(PanelModel::kFreshControls[size_t(i)] == fresh[i]);
    CHECK(PanelModel::kFreshControls[23] == 512 && PanelModel::kFreshControls[24] == 0 && PanelModel::kFreshControls[25] == 0);
    CHECK(PanelModel::kFreshControls[11] == 1023);

    std::vector<Sent> sent;
    PanelModel p;
    p.sendToOs = [&](const uint8_t* b, size_t n, uint64_t when) { sent.push_back({std::vector<uint8_t>(b, b + n), when}); };

    // F4 from the OS: one message per control, in cc order, B(0|(raw>>7)&7) cc (raw & 0x7F), 2 ms later.
    p.onOsByte(0xF4, 1000);
    CHECK(sent.size() == 1);
    if (!sent.empty()) {
        CHECK(sent[0].bytes == encode(fresh));
        CHECK(sent[0].bytes.size() == 78);
        CHECK(sent[0].when == 1000 + 2 * 16000);
        // spot checks of the encoding: Volume 1023 = B7 0B 7F, pitch wheel 512 = B4 17 00, Pan 508 = B3 01 7C
        CHECK(sent[0].bytes[33] == 0xB7 && sent[0].bytes[34] == 0x0B && sent[0].bytes[35] == 0x7F);
        CHECK(sent[0].bytes[69] == 0xB4 && sent[0].bytes[70] == 0x17 && sent[0].bytes[71] == 0x00);
        CHECK(sent[0].bytes[3] == 0xB3 && sent[0].bytes[4] == 0x01 && sent[0].bytes[5] == 0x7C);
    }

    // A knob move: one Bx cc vv with the new absolute value; the position is kept and answered to the next F4.
    sent.clear();
    p.moveControl(11, 700, 5000);
    CHECK(sent.size() == 1 && sent[0].bytes == std::vector<uint8_t>({0xB5, 0x0B, 0x3C}) && sent[0].when == 5000);
    p.moveControl(2, 2000, 6000);                         // clamped to 1023
    CHECK(p.controls[2] == 1023);
    sent.clear();
    p.onOsByte(0xF4, 7000);
    uint16_t moved[26]; for (int i = 0; i < 26; ++i) moved[i] = fresh[i];
    moved[11] = 700; moved[2] = 1023;
    CHECK(sent.size() == 1 && sent[0].bytes == encode(moved));

    // Positions are part of the saved panel state; a state saved before they existed loads with the fresh values.
    StateWriter w; p.save(w);
    PanelModel q;
    { StateReader r(w.bytes.data(), w.bytes.size()); q.load(r); CHECK(r.ok() && r.atEnd()); }
    CHECK(q.controls == p.controls);
    std::vector<uint8_t> old(w.bytes.begin(), w.bytes.end() - long(sizeof(uint16_t) * 26));
    PanelModel o; o.controls[5] = 999;
    { StateReader r(old.data(), old.size()); o.load(r); CHECK(r.ok()); }
    CHECK(o.controls == PanelModel::kFreshControls);

    // Switched off: no answer (the old behaviour, for comparisons).
    sent.clear();
    PanelModel off; off.answerF4 = false;
    off.sendToOs = [&](const uint8_t* b, size_t n, uint64_t when) { sent.push_back({std::vector<uint8_t>(b, b + n), when}); };
    off.onOsByte(0xF4, 0);
    CHECK(sent.empty());

    // LED messages reach onLed: 91 code (on), 92 code (flash), 9D code rate (beat flash).
    {
        PanelModel q;
        std::vector<std::array<int, 3>> seen;
        q.onLed = [&](uint8_t st, uint8_t code, uint8_t rate, uint64_t) { seen.push_back({st, code, rate}); };
        for (uint8_t b : {0x91, 0x12, 0x92, 0x13, 0x9D, 0x14, 0x40, 0x90, 0x12}) q.onOsByte(b, 0);
        CHECK(seen.size() == 4);
        if (seen.size() == 4) {
            CHECK((seen[0] == std::array<int, 3>{0x91, 0x12, 0}));
            CHECK((seen[1] == std::array<int, 3>{0x92, 0x13, 0}));
            CHECK((seen[2] == std::array<int, 3>{0x9D, 0x14, 0x40}));
            CHECK((seen[3] == std::array<int, 3>{0x90, 0x12, 0}));
        }
        CHECK(q.ledState[0x12] == 0 && q.ledState[0x13] == 2 && q.ledState[0x14] == 2);
    }

    std::printf("panel_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
