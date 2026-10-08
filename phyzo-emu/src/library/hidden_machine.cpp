#include "library/hidden_machine.h"
#include <cstdio>
#include <deque>
#include "editor/editor_proto.h"
#include "library/syx.h"
#include "machine.h"
#include "os_image.h"

namespace library {

namespace {
constexpr double kStepMs = 5;                       // machine time between checks while waiting
constexpr double kHoldMs = 110, kGapMs = 250;       // a press as hands do it: held, then a pause before the next
constexpr double kMidiByteMs = 0.32;                // 31.25 kbaud

std::string presetText(int preset) { char b[16]; std::snprintf(b, sizeof b, "P %02d", preset + 1); return b; }
std::string trimRight(std::string s) { while (!s.empty() && (s.back() == ' ' || s.back() == '\0')) s.pop_back(); return s; }

class HiddenMachine : public Ops {
public:
    HiddenMachine(std::unique_ptr<Machine> m, const std::atomic<bool>& cancel) : m_(std::move(m)), cancel_(cancel) {}

    bool receive(const std::vector<uint8_t>& message, const Progress& progress) override {
        m_->serial.queueRx(1, message, m_->cycles());
        const size_t total = message.size();
        const bool drained = wait(double(total) * kMidiByteMs * 2 + 2000, [&] {
            const size_t left = m_->serial.rxBacklog(1);
            if (progress) progress(total - std::min(total, left), total);
            return left == 0;
        });
        if (!drained) return fail("the OS did not take the message");
        std::string name;                            // it answers again once it has dealt with the message
        return readName(0, name);
    }

    bool programChange(int preset) override {
        m_->serial.queueRx(1, edproto::programChange(0, preset), m_->cycles());
        const std::string want = presetText(preset);
        return wait(2000, [&] { return m_->panel.text() == want; }) || fail("the display did not show " + want);
    }

    bool readName(uint16_t number, std::string& name) override {
        const edproto::Address a{edproto::kPreset, number, 0, 0, 8, 16};
        m_->serial.queueRx(1, edproto::partialRead(a), m_->cycles());
        bool refused = false;
        const bool got = wait(3000, [&] {
            for (auto it = msgs_.begin(); it != msgs_.end(); ++it) {
                edproto::Message r;
                if (edproto::parse(it->data(), it->size(), r) && r.command == edproto::kPartialReply && r.address == a) {
                    name = trimRight(std::string(r.data.begin(), r.data.end()));
                    msgs_.erase(it);
                    return true;
                }
                if (edproto::isOsSysex(it->data(), it->size(), edproto::kError)) { refused = true; msgs_.erase(it); return true; }
            }
            return false;
        });
        if (!got || refused) return fail("the OS did not answer the name read");
        return true;
    }

    bool compareLit() override {
        const auto it = m_->panel.ledState.find(kLedCompare);
        return it != m_->panel.ledState.end() && it->second != 0;
    }

    bool dumpPreset(std::vector<uint8_t>& message) override {
        msgs_.clear();
        if (!press(kRawDump) || !press(kRawDump)) return false;
        return wait(6000, [&] { return take(Kind::Preset, -1, message); }) || fail("the OS sent no preset");
    }

    bool dumpBank(int bank, std::vector<uint8_t>& message, const Progress& progress) override {
        msgs_.clear();
        if (!press(kRawDump)) return false;
        const std::string want = bank == 0 ? "bAn1" : "bAn2";
        // The Dump list starts at the current preset; +/Yes steps through P 01 ... P 64, bAn1, bAn2. A blank display
        // (a blink) is not a step: the last text shown counts.
        for (int step = 0; shown() != want; ++step) {
            if (step > 70) return fail("the Dump list did not reach " + want);
            const std::string before = shown();
            if (!press(kRawYes)) return false;
            wait(1000, [&] { return shown() != before; });
        }
        if (!press(kRawDump)) return false;
        const size_t expected = 102420;
        return wait(double(expected) * kMidiByteMs * 1.5 + 5000, [&] {
            if (progress && in_) progress(std::min(cur_.size(), expected), expected);
            return take(Kind::Bank, bank, message);
        }) || fail("the OS sent no bank");
    }

    std::string error() const override { return err_; }

private:
    const std::string& shown() {
        const std::string t = m_->panel.text();
        if (t.find_first_not_of(' ') != std::string::npos) shown_ = t;
        return shown_;
    }
    bool fail(const std::string& e) { if (err_.empty()) err_ = cancel_ ? "cancelled" : e; return false; }

    // Moves the OS's MIDI output into whole SysEx messages.
    void collect() {
        for (const auto& [when, b] : m_->midiOut) {
            (void)when;
            if (b == 0xF0) { cur_.assign(1, b); in_ = true; }
            else if (!in_ || b >= 0xF8) continue;
            else if (b == 0xF7) { cur_.push_back(b); msgs_.push_back(std::move(cur_)); cur_.clear(); in_ = false; }
            else if (b & 0x80) { cur_.clear(); in_ = false; }
            else cur_.push_back(b);
        }
        m_->midiOut.clear();
    }
    bool take(Kind kind, int bank, std::vector<uint8_t>& out) {
        for (auto it = msgs_.begin(); it != msgs_.end(); ++it) {
            int b = -1;
            if (classify(*it, &b) == kind && (bank < 0 || b == bank)) { out = std::move(*it); msgs_.erase(it); return true; }
        }
        return false;
    }
    // Runs the machine until `done` (checked every kStepMs of machine time) or `limitMs` passed.
    bool wait(double limitMs, const std::function<bool()>& done) {
        const uint64_t end = m_->cycles() + m_->cyclesFromMs(limitMs);
        for (;;) {
            collect();
            if (done && done()) return true;
            if (cancel_) return fail("cancelled");
            if (m_->cycles() >= end) return false;
            const Machine::Stop st = m_->runUntil(m_->cycles() + m_->cyclesFromMs(kStepMs));
            m_->audio.clear(); m_->wet.clear();
            if (st == Machine::Stop::Trap) return fail("the OS stopped (" + m_->trapReason + ")");
            if (st == Machine::Stop::HardStall) return fail("the OS stopped (error loop)");
        }
    }
    bool idle(double ms) { wait(ms, nullptr); return err_.empty(); }
    bool press(uint8_t raw) {
        m_->panel.inject({0x81, raw}, m_->cycles(), std::string());
        if (!idle(kHoldMs)) return false;
        m_->panel.inject({0x80, raw}, m_->cycles(), std::string());
        return idle(kGapMs);
    }

    std::unique_ptr<Machine> m_;
    const std::atomic<bool>& cancel_;
    std::deque<std::vector<uint8_t>> msgs_;
    std::vector<uint8_t> cur_;
    bool in_ = false;
    std::string err_, shown_;
};
}  // namespace

RealMachines::RealMachines(std::string osPath, std::string wavePath, std::function<UserFlash()> factoryUserFlash)
    : osPath_(std::move(osPath)), wavePath_(std::move(wavePath)), factory_(std::move(factoryUserFlash)) {}
RealMachines::~RealMachines() = default;

std::unique_ptr<Machine> RealMachines::make(std::string& err) {
    if (!os_) {
        auto os = std::make_unique<OsImage>();
        if (!os->load(osPath_, err)) { err = "cannot read the OS image: " + err; return nullptr; }
        os_ = std::move(os);
    }
    auto m = std::make_unique<Machine>();
    Machine::Config cfg;
    if (!m->init(*os_, cfg, err) || !m->loadWaveMemory(wavePath_, err)) return nullptr;
    m->panel.keepLog = false;
    m->captureAudio = false;
    return m;
}

std::unique_ptr<Ops> RealMachines::fresh(const std::atomic<bool>& cancel, std::string& err) {
    std::unique_ptr<Machine> m = make(err);
    if (!m) return nullptr;
    if (!booted_.empty()) {
        if (!m->loadState(booted_, err)) return nullptr;
        return std::make_unique<HiddenMachine>(std::move(m), cancel);
    }
    // Boot the v0.8.3 way: the factory user flash seeded first, then to the preset display, then let it settle.
    const UserFlash factory = factory_ ? factory_() : nullptr;
    if (!factory) { err = "no factory user memory"; return nullptr; }
    m->setUserFlash(factory->data(), factory->size());
    const uint64_t limit = m->cyclesFromMs(8000);
    bool shown = false;
    while (!cancel && m->cycles() < limit && !shown) {
        const Machine::Stop st = m->runUntil(m->cycles() + m->cyclesFromMs(20), [&] { return m->panel.text() == "P 01"; });
        if (st == Machine::Stop::Trap || st == Machine::Stop::HardStall) { err = "the OS stopped while booting"; return nullptr; }
        shown = st == Machine::Stop::Predicate;
    }
    if (!shown) { err = cancel ? "cancelled" : "the OS did not boot to P 01"; return nullptr; }
    const uint64_t settle = m->cycles() + m->cyclesFromMs(500);
    while (!cancel && m->cycles() < settle) m->runUntil(std::min(settle, m->cycles() + m->cyclesFromMs(20)));
    if (cancel) { err = "cancelled"; return nullptr; }
    m->midiOut.clear();
    booted_ = m->saveState();
    return std::make_unique<HiddenMachine>(std::move(m), cancel);
}

std::unique_ptr<Ops> RealMachines::clone(const std::vector<uint8_t>& machineState, const std::atomic<bool>& cancel, std::string& err) {
    std::unique_ptr<Machine> m = make(err);
    if (!m) return nullptr;
    if (!m->loadState(machineState, err)) { err = "the running machine's state could not be restored: " + err; return nullptr; }
    m->midiOut.clear();
    return std::make_unique<HiddenMachine>(std::move(m), cancel);
}

}  // namespace library
