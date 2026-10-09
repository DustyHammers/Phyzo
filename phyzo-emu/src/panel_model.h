// Model of the front-panel controller on serial channel A.
// OS -> panel: F0 33 hello, F2, F3, F4 (report analog controls), 9x display/LED messages.
// Panel -> OS: F0 33 reply, 80/81 id button edges, 9x keys, Bx knobs.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct PanelEvent {
    uint64_t cycle;
    bool fromOs;                 // true: OS -> panel
    std::vector<uint8_t> bytes;
    std::string decoded;
};

struct DisplayChange {
    uint64_t cycle;
    std::string text;            // 4 characters, best-guess decode
    std::array<uint8_t, 4> raw;  // segment bytes, leftmost first
    uint8_t dots;
};

class StateWriter;
class StateReader;

class PanelModel {
public:
    // fontTable: 128 bytes (ASCII -> segments) read from the user's OS image.
    void setFont(const uint8_t* fontTable);
    // Bytes to the OS (serial channel A), whole messages; never allocates for the caller.
    std::function<void(const uint8_t* bytes, size_t n, uint64_t cycle)> sendToOs;
    std::function<double(uint64_t)> cyclesToMs;
    uint64_t cyclesPerMs = 16000;

    // configuration
    bool keepLog = true;         // false: no transcript or display history (the plugin runs for hours)
    bool replyHello = true;
    double helloDelayMs = 2.0;
    int replyF2 = -1;            // >= 0: answer F2 with "F2 xx"
    // Analog controls (docs/PANEL_CONTROLS.md): the panel's physical knob/wheel positions, raw 10-bit, in cc order.
    static constexpr int kControls = 26;
    static constexpr std::array<uint16_t, kControls> kFreshControls = {
        102, 508, 698, 2, 512, 2, 516, 512, 5, 2, 4, 1023, 2, 516, 2, 516, 3, 3, 512, 3, 512, 3, 516,
        512,                     // cc23 pitch wheel: must be 512, the OS takes the wheel's centre from it
        0, 0};                   // cc24 mod wheel, cc25 pressure
    std::array<uint16_t, kControls> controls = kFreshControls;
    bool answerF4 = true;        // answer the OS's F4 request with all 26 positions
    int answerF4Value = -1;      // >= 0: answer with this one value for every control (test override)

    // A control moved: store the position and send one Bx cc vv with the new absolute value.
    void moveControl(int cc, int raw, uint64_t cycle);

    // Called for every LED message from the OS (status 90-92 and 9C/9E-9F: code; 9D: code and rate).
    std::function<void(uint8_t status, uint8_t code, uint8_t rate, uint64_t cycle)> onLed;

    void onOsByte(uint8_t b, uint64_t cycle);
    void onResetPin(bool asserted, uint64_t cycle);
    void inject(const std::vector<uint8_t>& bytes, uint64_t cycle, const std::string& note);
    // The same without allocating (the audio thread's knob and button messages; the note is logged only with keepLog).
    void inject(const uint8_t* bytes, size_t n, uint64_t cycle, const char* note);

    std::string text() const { return text_; }
    const std::array<uint8_t, 4>& rawDigits() const { return raw_; }
    uint8_t dots() const { return dots_; }

    // Panel state (display, LEDs, message parser). Configuration and logs are not included.
    void save(StateWriter& w) const;
    void load(StateReader& r);
    std::vector<PanelEvent> transcript;
    std::vector<DisplayChange> displayHistory;
    std::map<uint8_t, int> ledState;     // LED code -> 0 off, 1 on, 2 flashing
    uint64_t helloCount = 0, f2Count = 0, f3Count = 0, f4Count = 0, resetPulses = 0;
    uint64_t unknownMessages = 0;

private:
    void complete();
    void log(PanelEvent e);
    void history(DisplayChange d);
    std::string decodeSeg(uint8_t s) const;
    int expectedData(uint8_t status) const;

    std::map<uint8_t, char> segToChar_;
    std::array<uint8_t, 4> raw_{};
    uint8_t dots_ = 0;
    std::string text_ = "    ";
    uint8_t status_ = 0;
    std::vector<uint8_t> msg_;
    uint64_t msgStart_ = 0;
    bool resetAsserted_ = false;
};
