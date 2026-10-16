#include "engine.h"
#include "editor/editor_proto.h"
#include <algorithm>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "machine.h"
#include "os_image.h"
#include "os_profile.h"
#include "state_io.h"
#include "user_flash.h"
#include <map>

namespace {
constexpr uint32_t kEngineStateVersion = 1;

// The factory user flash per OS image MD5, shared by every engine in the process (null: it could not be made).
std::mutex g_factoryMtx;
std::map<std::string, Engine::UserFlash> g_factory;
std::mutex g_factoryMakeMtx;                        // one hidden machine at a time
std::atomic<uint64_t> g_factoryRuns{0};

struct Blob { std::string osMd5, waveMd5; std::vector<uint8_t> machine; };

// MIDI clock (F8, 24 a quarter note while the host's tempo is followed, F-68) is not "other MIDI" for the editor's arp
// edits: it changes neither the panel mode nor the preset.
bool isClock(const uint8_t* d, size_t n) { return n == 1 && d[0] == 0xF8; }

int64_t steadyMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Audio In while the idle runner runs the machine: silence (no host audio arrives then).
struct SilentInput : Machine::LiveInput {
    void next(int32_t& l, int32_t& r) override { l = r = 0; }
};
SilentInput g_silence;

std::vector<uint8_t> packBlob(const Blob& b) {
    StateWriter w;
    w.raw("PHZE", 4); w.put(kEngineStateVersion); w.str(b.osMd5); w.str(b.waveMd5); w.vec(b.machine);
    return std::move(w.bytes);
}
bool unpackBlob(const std::vector<uint8_t>& data, Blob& b) {
    StateReader r(data.data(), data.size());
    char magic[4] = {};
    r.raw(magic, 4);
    if (!r.ok() || std::memcmp(magic, "PHZE", 4) != 0 || r.get<uint32_t>() != kEngineStateVersion) return false;
    r.str(b.osMd5); r.str(b.waveMd5); r.vec(b.machine);
    return r.ok();
}
}  // namespace

// Audio In FIFO (single thread: the audio thread fills it before running the machine, the machine empties it).
struct Engine::InputFifo : Machine::LiveInput {
    std::vector<float> l = std::vector<float>(size_t(1) << 16), r = std::vector<float>(size_t(1) << 16);
    const uint64_t mask = (uint64_t(1) << 16) - 1;
    uint64_t head = 0, tail = 0, taken = 0, lastClip = 0, underruns = 0, overflows = 0;
    bool clipped = false;
    void reset(int prefill) { head = tail = 0; for (int i = 0; i < prefill; ++i) push(0.0f, 0.0f); }
    void push(float a, float b) {
        if (head - tail > mask) { ++overflows; return; }
        l[head & mask] = a; r[head & mask] = b; ++head;
    }
    static int32_t word(float x) { return int32_t(std::clamp(x, -1.0f, 1.0f) * 8388607.0f); }   // as the play tool's input
    void next(int32_t& wl, int32_t& wr) override {
        float a = 0, b = 0;
        if (tail < head) { a = l[tail & mask]; b = r[tail & mask]; ++tail; } else ++underruns;
        wl = word(a); wr = word(b);
        if (wl >= kClipWord || wl <= -kClipWord || wr >= kClipWord || wr <= -kClipWord) { lastClip = taken; clipped = true; }
        ++taken;
    }
    bool clipLit() const { return clipped && taken - lastClip <= kClipHoldSamples; }
};

Engine::Engine() : fifo_(std::make_unique<InputFifo>()) {
    snap_.reserve(size_t(4) << 20);                 // machine state is about 0.8 MB: snapshots never allocate
    hostEvents_.reserve(8192);
    for (int i = 0; i < 26; ++i) { controls_[size_t(i)] = PanelModel::kFreshControls[size_t(i)]; positions_[size_t(i)] = controls_[size_t(i)].load(); }
    for (uint32_t i = 0; i < kButtonRing; ++i) buttons_[i].seq.store(i, std::memory_order_relaxed);
}

Engine::~Engine() {
    {
        std::lock_guard<std::mutex> lk(idleMtx_);     // the idle runner first: it must never see the machine go
        idleStop_ = true;
    }
    if (idle_.joinable()) idle_.join();
    stopJob();
    std::lock_guard<std::mutex> lk(mtx_);
    live_.reset();
}

// ------------------------------------------------------------------ message thread

void Engine::setRoms(const std::string& osPath, const std::string& wavePath, const std::string& osMd5, const std::string& waveMd5) {
    const bool same = osMd5 == osMd5_ && waveMd5 == waveMd5_ && state_.load() != State::NoRoms;
    osPath_ = osPath; wavePath_ = wavePath; osMd5_ = osMd5; waveMd5_ = waveMd5;
    if (same) return;
    std::vector<uint8_t> machine;
    if (!pendingState_.empty()) {
        Blob b;
        if (unpackBlob(pendingState_, b) && b.osMd5 == osMd5 && b.waveMd5 == waveMd5) machine = std::move(b.machine);
        else setMessage("The saved state was made with other ROM files, so the synth starts fresh.");
        pendingState_.clear();
    }
    startJob(std::move(machine));
}

std::vector<uint8_t> Engine::getState() {
    std::lock_guard<std::mutex> gl(getStateMtx_);
    if (state_.load() != State::Running)             // still restoring (or stopped): the state it restores
        return jobState_.empty() ? pendingState_ : packBlob({osMd5_, waveMd5_, jobState_});
    auto nowMs = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    // Audio running: ask the audio thread for a snapshot at its next block boundary.
    if (nowMs() - lastProcessMs_.load() < 200) {
        const uint32_t want = snapRequest_.load() + 1;
        snapRequest_ = want;
        for (int i = 0; i < 500 && snapDone_.load(std::memory_order_acquire) != want; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (snapDone_.load(std::memory_order_acquire) == want) {
            // The audio thread leaves snap_ alone until the next request (only made here, under getStateMtx_).
            std::vector<uint8_t> blob = packBlob({osMd5_, waveMd5_, snap_});
            if (snap_.size() * 2 > snap_.capacity()) snap_.reserve(snap_.size() * 2);   // keep the audio side allocation-free
            return blob;
        }
    }
    // Audio not running (stopped host, offline): save directly.
    std::lock_guard<std::mutex> lk(mtx_);
    if (live_ && state_.load() == State::Running) return packBlob({osMd5_, waveMd5_, live_->saveState()});
    return jobState_.empty() ? pendingState_ : packBlob({osMd5_, waveMd5_, jobState_});
}

void Engine::setState(const std::vector<uint8_t>& blob) {
    Blob b;
    if (!unpackBlob(blob, b)) { setMessage("The saved state could not be read, so the synth keeps its current state."); return; }
    if (state_.load() == State::NoRoms || osMd5_.empty()) { pendingState_ = blob; return; }
    if (b.osMd5 != osMd5_ || b.waveMd5 != waveMd5_) {
        setMessage("The saved state was made with other ROM files, so the synth keeps its current state.");
        return;
    }
    startJob(std::move(b.machine));
}

// Any thread, no lock, no allocation (a bounded multi-producer queue: each cell's sequence number says whether it is
// free for the producer at `pos` (== pos), written and waiting for the consumer (== pos + 1), or not yet consumed
// from the previous lap (< pos: full)).
bool Engine::push(uint32_t entry) {
    uint32_t pos = btnEnqueue_.load(std::memory_order_relaxed);
    for (;;) {
        ButtonCell& c = buttons_[pos & (kButtonRing - 1)];
        const int32_t dif = int32_t(c.seq.load(std::memory_order_acquire) - pos);
        if (dif == 0) {
            if (btnEnqueue_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                c.value = entry;
                c.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (dif < 0) {
            return false;                            // full: dropped
        } else {
            pos = btnEnqueue_.load(std::memory_order_relaxed);
        }
    }
}

void Engine::pressButton(int osButton, bool down) {
    const uint32_t bit = 1u << (osButton & 31);
    if (down) heldOs_.fetch_or(bit); else heldOs_.fetch_and(~bit);
    ++panelGestures_;
    push(uint32_t((osButton << 1) | (down ? 1 : 0)));
}

void Engine::pressRaw(int raw, bool down) {
    if (raw < 0 || raw > 0x7F) return;
    const uint32_t bit = 1u << (raw & 31);
    if (down) heldRaw_[size_t(raw >> 5)].fetch_or(bit); else heldRaw_[size_t(raw >> 5)].fetch_and(~bit);
    ++panelGestures_;
    push((2u << 24) | (down ? 0x100u : 0u) | uint32_t(raw));
}

bool Engine::anyButtonHeld() const {
    for (const auto& w : heldRaw_) if (w.load()) return true;
    return heldOs_.load() != 0;
}

Engine::LedInfo Engine::led(int code) const {
    if (code < 0 || code > 255) return {LedOff, 0, 0};
    const size_t c = size_t(code);
    return {LedMode(ledMode_[c].load()), beats_[c].load(), beatIntervalMs_[c].load()};
}

std::array<uint8_t, 4> Engine::segments() const {
    const uint32_t v = segs_.load();
    return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
}

// LED messages from the machine's panel (worker thread while booting, then the audio thread).
void Engine::attachLeds(Machine& m) {
    auto lastBeat = std::make_shared<std::array<double, 256>>();
    lastBeat->fill(-1);
    const double msPerCycle = 1000.0 / m.config().cpuHz;
    m.panel.onLed = [this, lastBeat, msPerCycle](uint8_t st, uint8_t code, uint8_t, uint64_t cycle) {
        if (st == 0x9d) {
            const double now = double(cycle) * msPerCycle;
            double& prev = (*lastBeat)[code];
            beatIntervalMs_[code] = prev >= 0 ? float(now - prev) : 0.0f;
            prev = now;
            ledMode_[code] = LedBeat;
            beats_[code] = beats_[code].load() + 1;
            ++uiSerial_;
        } else if (st >= 0x90 && st <= 0x92) {
            if (ledMode_[code].load() != uint8_t(st - 0x90)) { ledMode_[code] = uint8_t(st - 0x90); ++uiSerial_; }
            ++ledStateMessages_;                     // the editor's preset-load trigger (never the beat flash)
        }
        ++panelMessages_;
    };
}

// After a restore the LEDs come from the saved panel state (a beat-flashing LED stays a beat LED if it was one).
void Engine::publishLeds(const Machine& m) {
    for (int c = 0; c < 256; ++c) {
        auto it = m.panel.ledState.find(uint8_t(c));
        const int s = it == m.panel.ledState.end() ? 0 : it->second;
        const uint8_t cur = ledMode_[size_t(c)].load();
        if (s == 2 && cur == LedBeat) continue;
        ledMode_[size_t(c)] = uint8_t(s >= 0 && s <= 2 ? s : 0);
    }
    ++uiSerial_;
}

void Engine::setControl(int cc, int raw) {
    if (cc < 0 || cc >= 26) return;
    raw = std::clamp(raw, 0, 1023);
    controls_[size_t(cc)].store(uint16_t(raw), std::memory_order_relaxed);
    positions_[size_t(cc)] = uint16_t(raw);
    pendingControl_[size_t(cc)].store(uint16_t(0x8000 | raw), std::memory_order_release);   // latest value wins
    lastPanelControl_ = cc;                          // before the counters: a reader that sees the move sees the control
    ++panelControlMoves_;
    ++panelGestures_;
}

// Audio thread, block start: one message per control that moved, at most one per kControlIntervalMs each, and
// only while the panel link keeps up (the serial port itself delivers bytes at the link's real rate).
void Engine::sendControls(Machine& m) {
    const uint64_t now = m.cycles();
    const uint64_t interval = m.cyclesFromMs(kControlIntervalMs);
    for (size_t cc = 0; cc < pendingControl_.size(); ++cc) {
        if (!(pendingControl_[cc].load(std::memory_order_relaxed) & 0x8000)) continue;
        if (controlSentAt_[cc] && now - controlSentAt_[cc] < interval) continue;
        if (m.serial.rxBacklog(0) > kPanelBacklogBytes) return;
        const uint16_t v = pendingControl_[cc].exchange(0, std::memory_order_acq_rel);
        if (!(v & 0x8000)) continue;
        m.panel.moveControl(int(cc), int(v & 0x3FF), now);
        positions_[cc] = m.panel.controls[cc];
        controls_[cc].store(m.panel.controls[cc], std::memory_order_relaxed);   // what the panel now answers to F4
        controlSentAt_[cc] = now ? now : 1;
        ++controlMessages_;
    }
}

// Audio thread, block end: serve a pending getState request.
void Engine::serveSnapshot(Machine& m) {
    const uint32_t want = snapRequest_.load(std::memory_order_acquire);
    if (want == snapDone_.load(std::memory_order_relaxed)) return;
    m.saveStateInto(snap_);
    snapDone_.store(want, std::memory_order_release);
}

void Engine::publishPositions(const Machine& m) {
    for (int i = 0; i < 26; ++i) positions_[size_t(i)] = m.panel.controls[size_t(i)];
}

void Engine::service() {
    if (!rebootRequested_.exchange(false)) return;
    // The OS restarted itself: boot again with the user flash it had (flash keeps its contents through a restart).
    UserFlash keep;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (live_) keep = std::make_shared<const std::vector<uint8_t>>(live_->userFlash(), live_->userFlash() + Machine::kUserFlashSize);
    }
    startJob({}, keep);
}

void Engine::setFactoryUserFlash(bool on, const std::string& cacheDir) { factoryOn_ = on; factoryDir_ = cacheDir; }

void Engine::setFactoryUserFlashForTest(const std::string& osMd5, UserFlash data) {
    std::lock_guard<std::mutex> lk(g_factoryMtx);
    g_factory[osMd5] = std::move(data);
}
uint64_t Engine::factoryRunsForTest() { return g_factoryRuns.load(); }

Engine::UserFlash Engine::factoryUserFlashFor(const std::string& osMd5) {
    std::lock_guard<std::mutex> lk(g_factoryMtx);
    const auto it = g_factory.find(osMd5);
    return it == g_factory.end() ? nullptr : it->second;
}

std::vector<uint8_t> Engine::machineState() {
    if (state_.load() != State::Running) return {};
    Blob b;
    return unpackBlob(getState(), b) ? std::move(b.machine) : std::vector<uint8_t>();
}

std::string Engine::message() const { std::lock_guard<std::mutex> lk(msgMtx_); return message_; }
void Engine::setMessage(const std::string& s) { std::lock_guard<std::mutex> lk(msgMtx_); message_ = s; }

std::array<char, 4> Engine::display() const {
    const uint32_t c = chars_.load();
    return {char(c >> 24), char(c >> 16), char(c >> 8), char(c)};
}

// ------------------------------------------------------------------ worker: boot or restore a machine

void Engine::stopJob() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
}

void Engine::startJob(std::vector<uint8_t> state, UserFlash keep) {
    stopJob();
    { std::lock_guard<std::mutex> gl(getStateMtx_); jobState_ = state; }
    const uint64_t gen = ++generation_;
    state_ = State::Booting;
    Job job{osPath_, wavePath_, osMd5_, factoryOn_, factoryDir_, std::move(keep)};
    worker_ = std::thread(&Engine::runJob, this, std::move(job), std::move(state), gen);
}

void Engine::fail(const std::string& reason) {
    setMessage(reason);
    state_ = State::Stopped;
}

// Cold boot to the preset display, then let it settle. False if the OS stopped (err says why) or on cancel.
bool Engine::coldBoot(Machine& m, bool publish, std::string& err) {
    const uint64_t limit = m.cyclesFromMs(kBootLimitMs);
    bool shown = false;
    while (!cancel_ && m.cycles() < limit && !shown) {
        Machine::Stop st = m.runUntil(m.cycles() + m.cyclesFromMs(20), [&] { return m.panel.text() == kBootDisplay; });
        m.audio.clear(); m.wet.clear();
        if (publish) publishDisplay(m);
        if (st == Machine::Stop::Trap || st == Machine::Stop::HardStall) {
            err = st == Machine::Stop::Trap ? m.trapReason : std::string("error loop");
            return false;
        }
        shown = st == Machine::Stop::Predicate;
    }
    const uint64_t settleEnd = m.cycles() + m.cyclesFromMs(kSettleMs);
    while (!cancel_ && m.cycles() < settleEnd) {
        m.runUntil(std::min(settleEnd, m.cycles() + m.cyclesFromMs(20)));
        m.audio.clear(); m.wet.clear();
        if (publish) publishDisplay(m);
    }
    if (!shown && !cancel_) err = std::string("the display did not show ") + kBootDisplay;
    return !cancel_;
}

// The factory user flash for this OS image: from this process, else from the cache file, else made once in a hidden
// machine (a freshly booted machine running the OS's own factory restore) and cached. Null if it cannot be made.
Engine::UserFlash Engine::factoryUserFlash(const Job& job, const OsImage& os) {
    auto known = [&](UserFlash& f) {
        std::lock_guard<std::mutex> lk(g_factoryMtx);
        auto it = g_factory.find(job.osMd5);
        if (it == g_factory.end()) return false;
        f = it->second;
        return true;
    };
    UserFlash f;
    if (known(f)) return f;
    std::unique_lock<std::mutex> make(g_factoryMakeMtx, std::defer_lock);
    while (!make.try_lock()) {                       // another engine is making it: wait (cancellable)
        if (cancel_) return nullptr;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (known(f)) return f;
    auto keep = [&](UserFlash v) { std::lock_guard<std::mutex> lk(g_factoryMtx); g_factory[job.osMd5] = v; return v; };
    const std::string path = job.factoryDir.empty() ? std::string() : userflash::cachePath(job.factoryDir, job.osMd5);
    std::vector<uint8_t> data;
    if (!path.empty() && userflash::readCache(path, job.osMd5, data))
        return keep(std::make_shared<const std::vector<uint8_t>>(std::move(data)));

    const std::string making = "Making the factory presets from your OS (only the first time).", before = message();
    setMessage(making);
    ++g_factoryRuns;
    std::string err;
    auto h = std::make_unique<Machine>();
    Machine::Config cfg;
    bool ok = h->init(os, cfg, err) && h->loadWaveMemory(job.wavePath, err);
    if (ok) {
        h->panel.keepLog = false;
        ok = coldBoot(*h, false, err) && err.empty();
    }
    if (ok) {
        userflash::MachinePanel panel(*h, &cancel_);
        ok = userflash::factoryRestore(panel, err);
    }
    if (cancel_) return nullptr;                     // not made, not known: the next job tries again
    if (!ok) {
        setMessage("The factory presets could not be made (" + err + "), so the synth starts with empty user memory.");
        return keep(nullptr);
    }
    f = std::make_shared<const std::vector<uint8_t>>(h->userFlash(), h->userFlash() + Machine::kUserFlashSize);
    if (!path.empty()) userflash::writeCache(path, job.osMd5, *f);
    if (message() == making) setMessage(before);
    return keep(f);
}

void Engine::runJob(Job job, std::vector<uint8_t> state, uint64_t generation) {
    OsImage os; std::string err;
    if (!os.load(job.osPath, err)) { fail("Cannot read the OS image: " + err); return; }
    auto make = [&](std::unique_ptr<Machine>& m) {
        m = std::make_unique<Machine>();
        Machine::Config cfg;
        if (!m->init(os, cfg, err) || !m->loadWaveMemory(job.wavePath, err)) return false;
        m->panel.keepLog = false;
        m->esp2.audit = false;                       // the tools' audit bookkeeping: not needed here (output unchanged)
        attachLeds(*m);
        for (size_t i = 0; i < controls_.size(); ++i) m->panel.controls[i] = controls_[i].load(std::memory_order_relaxed);   // answered to the OS's F4
        m->captureAudio = true;
        m->audio.reserve(1 << 16); m->wet.reserve(1 << 16);
        return true;
    };
    std::unique_ptr<Machine> m;
    if (!make(m)) { fail("The emulator could not start: " + err); return; }

    bool restored = false;
    if (!state.empty()) {
        if (m->loadState(state, err)) restored = true;
        else {
            setMessage("The saved state could not be restored (" + err + "), so the synth starts fresh.");
            if (!make(m)) { fail("The emulator could not start: " + err); return; }
        }
    }
    if (cancel_) return;
    // A restored state keeps its own user flash (a state saved before v0.8.3 has none and stays erased, as then: its
    // OS read the flash-write service pointer at boot, when it was null, so that machine never saves). A new machine
    // gets the user flash before it boots: after an OS restart the one it had, else the factory's.
    if (!restored) {                     // seed the user flash, cold boot to the preset display, then let it settle
        UserFlash seed = job.keep;
        if (!seed && job.factoryOn) seed = factoryUserFlash(job, os);
        if (seed) m->setUserFlash(seed->data(), seed->size());
        if (cancel_) return;
        std::string why;
        if (!coldBoot(*m, true, why) && !cancel_) { fail("The synth's OS stopped while booting: " + why); return; }
        if (cancel_) return;
    }
    // Panel ids of the +/Yes (OS button 1) and -/No (OS button 0) buttons, from the image's own table.
    uint8_t raw[2] = {0, 0}; bool found[2] = {false, false};
    for (uint32_t i = 0; i < profile::kButtonMapLen; ++i) {
        const uint8_t id = os.at(profile::kButtonMap + i);
        if (id < 2 && !found[id]) { raw[id] = uint8_t(i); found[id] = true; }
    }
    publishDisplay(*m);
    install(std::move(m), generation, raw[1], raw[0]);
}

void Engine::install(std::unique_ptr<Machine> m, uint64_t generation, uint8_t rawPlus, uint8_t rawMinus) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (generation != generation_.load()) return;           // a newer job replaced this one
        live_.swap(m);
        libInFlight_ = false;                            // a library message in the old machine's port went with it
        rawPlus_ = rawPlus; rawMinus_ = rawMinus;
        rawPlusPub_ = rawPlus; rawMinusPub_ = rawMinus;
        publishPositions(*live_);
        publishLeds(*live_);
        controlSentAt_.fill(0);
        for (size_t i = 0; i < controls_.size(); ++i) controls_[i].store(live_->panel.controls[i], std::memory_order_relaxed);   // a restored state's knobs
        retime_ = true;
        state_ = State::Running;
    }
    // the previous machine (if any) is destroyed here, outside the audio lock
}

void Engine::publishDisplay(Machine& m) {
    const std::string t = m.panel.text();
    uint32_t c = 0;
    for (int i = 0; i < 4; ++i) c = (c << 8) | uint8_t(i < int(t.size()) ? t[size_t(i)] : ' ');
    chars_ = c;
    const uint32_t dots = m.panel.dots();
    const auto& r = m.panel.rawDigits();
    const uint32_t segs = uint32_t(r[0]) << 24 | uint32_t(r[1]) << 16 | uint32_t(r[2]) << 8 | r[3];
    if (segs != segs_.load() || dots != dots_.load()) { segs_ = segs; dots_ = dots; ++uiSerial_; ++panelMessages_; }
}

// ------------------------------------------------------------------ audio thread

void Engine::prepare(double hostRate, int maxBlock, bool audioIn) {
    std::lock_guard<std::mutex> lk(mtx_);
    maxLockWaitUs_ = 0;
    hostRate_ = hostRate;
    resampler_.setup(44100, int(std::lround(hostRate)));
    retime_ = true;
    const double r = 44100.0 / hostRate;
    // MIDI (and Audio In) reach the machine lookahead + margin samples after their host time. With the input on, the
    // margin must also cover the input converter's look-ahead (64 machine samples at 48 kHz) so the input is never
    // late: then latency grows by the difference. Without it, nothing changes.
    inputActive_ = audioIn;
    inRes_.setup(int(std::lround(hostRate)), 44100);
    marginIn_ = audioIn ? std::max(48, int(std::ceil(inRes_.lookahead() * r)) + 3) : 48;
    prefill_ = resampler_.lookahead() + marginIn_;
    inConvL_.assign(size_t(std::ceil(maxBlock * r)) + 4096, 0.0f); inConvR_.assign(inConvL_.size(), 0.0f);
    const size_t cap = size_t(std::ceil(maxBlock * r)) + size_t(resampler_.lookahead()) + 4096;
    inL_.assign(cap, 0.0f); inR_.assign(cap, 0.0f);
    latency_ = int(std::ceil((resampler_.lookahead() + marginIn_) / r));
}

// Audio thread: every button edge queued so far, in order (a cell a producer is still writing waits for the next
// block, and so do the ones after it).
void Engine::drainButtons(Machine& m) {
    for (;;) {
        ButtonCell& c = buttons_[btnDequeue_ & (kButtonRing - 1)];
        if (int32_t(c.seq.load(std::memory_order_acquire) - (btnDequeue_ + 1)) < 0) break;
        const uint32_t v = c.value;
        c.seq.store(btnDequeue_ + kButtonRing, std::memory_order_release);
        ++btnDequeue_;
        ++buttonMessages_;
        uint8_t msg[2];
        if ((v >> 24) == 2) {
            msg[0] = uint8_t((v & 0x100) ? 0x81 : 0x80); msg[1] = uint8_t(v & 0x7F);
        } else {
            msg[0] = uint8_t((v & 1) ? 0x81 : 0x80); msg[1] = ((v >> 1) & 0xFF) == 1 ? rawPlus_ : rawMinus_;
        }
        m.panel.inject(msg, 2, m.cycles(), "");
    }
}

void Engine::process(float* left, float* right, int n, const MidiEvent* events, int numEvents, const float* inLeft, const float* inRight) {
    const auto t0 = std::chrono::steady_clock::now();
    // Before the lock: an idle slice holding it sees the host is back and gives the lock up after that slice.
    lastProcessMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(t0.time_since_epoch()).count();
    std::unique_lock<std::mutex> lk(mtx_);
    const auto t1 = std::chrono::steady_clock::now();
    const double waitUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
    if (waitUs > maxLockWaitUs_.load(std::memory_order_relaxed)) maxLockWaitUs_ = waitUs;
    if (!live_ || state_.load() != State::Running || hostRate_ <= 0) {
        std::fill(left, left + n, 0.0f); std::fill(right, right + n, 0.0f);
        return;
    }
    Machine& m = *live_;
    hostEvents_.clear(); hostUsed_ = 0;
    if (retime_) {
        outParser_.reset();
        if (inputActive_) { inRes_.reset(); fifo_->reset(prefill_); }
        resampler_.reset();
        m.audio.clear(); m.wet.clear();
        anchorSample_ = m.samplesProduced();
        hostPos_ = 0;
        retime_ = false;
    }
    using Clock = std::chrono::steady_clock;
    const bool prof = profiling_.load(std::memory_order_relaxed);
    m.profile = prof;
    const auto tq0 = Clock::now();
    drainButtons(m);
    sendControls(m);

    // MIDI: each event goes to the MIDI port at its exact time plus the constant latency.
    const double r = 44100.0 / hostRate_, cyclesPerSample = m.config().cpuHz / 44100.0;
    // Virtual port first: whole messages at the block's start, while the port is not backed up (a message larger
    // than the backlog limit goes alone into an empty port).
    const bool editor = editorOpen_.load(std::memory_order_relaxed);
    auto blockStart = [&] {
        const double s = double(anchorSample_) + double(hostPos_) * r + resampler_.lookahead() + marginIn_;
        const uint64_t at = uint64_t(s * cyclesPerSample);
        return at < m.cycles() ? m.cycles() : at;
    };
    if (vin_.peekSize()) takeVirtualIn(m, blockStart(), editor);
    if (libIn_.peekSize() || libInFlight_.load(std::memory_order_relaxed)) takeLibraryIn(m, blockStart(), editor);
    // Editor: whole messages after the virtual port's, paced like them (nothing while it is closed).
    if (edOut_.peekSize()) {
        if (!editor) {
            while (edOut_.peekSize()) edOut_.pop([](uint8_t) {});
        } else {
            takeEditorOut(m, blockStart());
        }
    }
    expireRequests(m);
    for (int i = 0; i < numEvents; ++i) {
        const double s = double(anchorSample_) + double(hostPos_ + events[i].offset) * r + resampler_.lookahead() + marginIn_;
        uint64_t at = uint64_t(s * cyclesPerSample);
        if (at < m.cycles()) { at = m.cycles(); ++lateMidi_; }
        midiTimes_[midiCount_++ % midiTimes_.size()] = at;
        m.serial.queueRx(1, events[i].data, size_t(events[i].size), at);
        if (!isClock(events[i].data, size_t(events[i].size))) externalMidi_.fetch_add(1, std::memory_order_relaxed);
        if ((editor || pendingCount_) && edproto::isRequest(events[i].data, size_t(events[i].size)))
            noteRequest(false, events[i].data, size_t(events[i].size), m.samplesProduced());
        if (edproto::isPresetTrigger(events[i].data, size_t(events[i].size))) {
            ++presetTriggers_;
            if (editor) queueStandingRead(m, at);
        }
    }

    // Audio In: this block's input, converted to 44.1 kHz, queued for the ESP2's serial input.
    if (inputActive_) {
        if (inLeft && inRight) inRes_.push(inLeft, inRight, n);
        else {
            std::fill(inConvL_.begin(), inConvL_.end(), 0.0f);
            for (int done = 0; done < n;) { const int c = std::min(n - done, int(inConvL_.size())); inRes_.push(inConvL_.data(), inConvL_.data(), c); done += c; }
        }
        for (int64_t k = inRes_.available(); k > 0;) {
            const int c = int(std::min<int64_t>(k, int64_t(inConvL_.size())));
            inRes_.produce(inConvL_.data(), inConvR_.data(), c);
            for (int i = 0; i < c; ++i) fifo_->push(inConvL_[size_t(i)], inConvR_[size_t(i)]);
            k -= c;
        }
    }
    m.liveIn = inputActive_ ? fifo_.get() : nullptr;
    const auto tq1 = Clock::now();
    const double host0 = m.hostSeconds, voice0 = m.profVoice, esp20 = m.profEsp2;

    // Run the machine until the converter has the input it needs for this block.
    const int64_t need = resampler_.inputNeeded(n);
    while (int64_t(m.wet.size() / 2) < need) {
        const uint64_t target = m.samplesProduced() + uint64_t(need - int64_t(m.wet.size() / 2));
        const Machine::Stop st = m.runUntil(std::max(m.cycles() + 1, m.cycleOfSample(target)));
        if (st == Machine::Stop::Trap || st == Machine::Stop::HardStall) {
            machineStopped(m, int(st));
            std::fill(left, left + n, 0.0f); std::fill(right, right + n, 0.0f);
            return;
        }
    }
    const auto tr0 = Clock::now();
    const size_t got = m.wet.size() / 2;
    if (inL_.size() < got) { inL_.resize(got); inR_.resize(got); }
    for (size_t i = 0; i < got; ++i) {
        inL_[i] = float(m.wet[2 * i]) * (1.0f / 8388608.0f);      // 24-bit DAC words, 1.0 = full scale
        inR_[i] = float(m.wet[2 * i + 1]) * (1.0f / 8388608.0f);
    }
    resampler_.push(inL_.data(), inR_.data(), int(got));
    midiOut(m, n);
    const bool clip = inputActive_ && fifo_->clipLit();
    if (clip != inputClip_.load(std::memory_order_relaxed)) { inputClip_.store(clip, std::memory_order_relaxed); ++uiSerial_; }
    m.wet.clear(); m.audio.clear(); m.midiOut.clear();
    resampler_.produce(left, right, n);
    if (prof) {
        const double secs = [](Clock::duration d) { return std::chrono::duration<double>(d).count(); }(Clock::now() - tr0);
        times_.resample = secs;
        times_.queue = std::chrono::duration<double>(tq1 - tq0).count();
        times_.voice = m.profVoice - voice0;
        times_.esp2 = m.profEsp2 - esp20;
        times_.cpu = std::max(0.0, (m.hostSeconds - host0) - times_.voice - times_.esp2);
    } else {
        times_ = BlockTimes();
    }
    hostPos_ += n;
    publishDisplay(m);
    serveSnapshot(m);
}

// Under mtx_: the virtual port's waiting messages, whole, at machine cycle `at`, while the MIDI port keeps up (a
// message larger than the backlog limit goes alone into an empty port).
void Engine::takeVirtualIn(Machine& m, uint64_t at, bool editor) {
    while (const size_t sz = vin_.peekSize()) {
        const size_t backlog = m.serial.rxBacklog(1);
        if (backlog > 0 && backlog + sz > kVirtualBacklog) break;
        size_t k = 0;
        vin_.pop([&](uint8_t b) { if (k < scratch_.size()) scratch_[k++] = b; });
        m.serial.queueRx(1, scratch_.data(), k, at);
        if (!isClock(scratch_.data(), k)) externalMidi_.fetch_add(1, std::memory_order_relaxed);
        if ((editor || pendingCount_) && edproto::isRequest(scratch_.data(), k)) noteRequest(false, scratch_.data(), k, m.samplesProduced());
        if (edproto::isPresetTrigger(scratch_.data(), k)) {
            ++presetTriggers_;
            if (editor) queueStandingRead(m, at);        // directly behind it: the read sees the new preset
        }
    }
}

// Under mtx_: the window's messages, each whole, once the MIDI port's input queue has room for all of it (made
// beforehand: no allocation); then watches the OS read the last one's last byte.
void Engine::takeLibraryIn(Machine& m, uint64_t at, bool editor) {
    const SerialChannel& port = m.serial.ch_[1];
    while (const size_t sz = libIn_.peekSize()) {
        const size_t backlog = m.serial.rxBacklog(1);
        if (backlog + sz > Serial::kMidiRxRoom) break;
        size_t k = 0;
        libIn_.pop([&](uint8_t b) { if (k < scratch_.size()) scratch_[k++] = b; });
        m.serial.queueRx(1, scratch_.data(), k, at);
        libDoneAt_ = port.rxBytes + backlog + k;
        libInFlight_.store(true, std::memory_order_release);
        libTaken_.fetch_add(1, std::memory_order_release);
        if (edproto::isPresetTrigger(scratch_.data(), k)) {
            ++presetTriggers_;
            if (editor) queueStandingRead(m, at);    // directly behind it: the editor's read sees the new preset
        }
    }
    if (libInFlight_.load(std::memory_order_relaxed) && port.rxBytes >= libDoneAt_) libInFlight_.store(false, std::memory_order_release);
}

bool Engine::libraryInput(const uint8_t* m, size_t n) {
    if (n == 0 || n > kLibraryMessageMax) return false;
    std::lock_guard<std::mutex> lk(libInMtx_);
    if (!libIn_.push(m, n)) return false;
    libPushed_.fetch_add(1, std::memory_order_release);
    return true;
}

// Under mtx_, editor open: its waiting messages after the virtual port's, paced like them.
void Engine::takeEditorOut(Machine& m, uint64_t at) {
    while (const size_t sz = edOut_.peekSize()) {
        const size_t backlog = m.serial.rxBacklog(1);
        if (backlog > 0 && backlog + sz > kVirtualBacklog) break;
        size_t k = 0;
        edOut_.pop([&](uint8_t b) { if (k < scratch_.size()) scratch_[k++] = b; });
        queueEditor(m, scratch_.data(), k, at);
    }
}

// Requests the OS never answered expire one by one (they are in the order they were sent).
void Engine::expireRequests(const Machine& m) {
    size_t expired = 0;
    while (expired < pendingCount_ && m.samplesProduced() - pending_[expired].sample > kEditorReplyTimeoutSamples) ++expired;
    if (expired) {
        std::copy(pending_.begin() + long(expired), pending_.begin() + long(pendingCount_), pending_.begin());
        pendingCount_ -= expired;
    }
}

// The OS stopped (a trap or an error loop): say why; a restart it asked for boots again (service()).
void Engine::machineStopped(Machine& m, int stop) {
    const auto st = Machine::Stop(stop);
    if (st == Machine::Stop::Trap && m.trapReason.rfind("reboot", 0) == 0) {
        setMessage("The synth's OS restarted itself; booting again.");
        rebootRequested_ = true;
    } else {
        std::string why = st == Machine::Stop::Trap ? m.trapReason : std::string("error loop");
        if (st == Machine::Stop::HardStall)
            for (const auto& e : profile::kErrorLoops) if (e.pc == m.stallPc) why = e.meaning;
        setMessage("The synth's OS stopped: " + why);
    }
    state_ = State::Stopped;
}

// ------------------------------------------------------------------ idle runner (F-63)

void Engine::setIdleRun(bool on) {
    idleOn_ = on;
    if (!on) return;
    std::lock_guard<std::mutex> lk(idleMtx_);
    if (!idle_.joinable() && !idleStop_.load()) idle_ = std::thread(&Engine::idleLoop, this);
}

// Every kIdlePollMs: when on, a machine runs and no host block came for kIdleQuietMs, run it for the real time that
// passed (kIdlePollMs on the first step, at most kIdleMaxStepMs), in slices of at most kIdleSliceMs of machine time,
// each under the lock taken with try_lock (a step ends when it is busy).
void Engine::idleLoop() {
    int64_t last = 0;                                // when the previous step ran (0: not idling)
    while (!idleStop_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
        const int64_t now = steadyMs();
        if (!idleOn_.load() || now - lastProcessMs_.load() < kIdleQuietMs || state_.load() != State::Running) {
            idling_ = false;
            last = 0;
            continue;
        }
        int64_t ms = last == 0 ? kIdlePollMs : std::min(kIdleMaxStepMs, std::max<int64_t>(0, now - last));
        last = now;
        while (ms > 0 && !idleStop_.load()) {
            const int64_t slice = std::min(ms, kIdleSliceMs);
            std::unique_lock<std::mutex> lk(mtx_, std::try_to_lock);
            if (!lk.owns_lock() || !idleSlice(slice)) break;
            ms -= slice;
        }
    }
    idling_ = false;
}

// Under mtx_: one slice of machine time with the panel, the virtual port and the editor; audio discarded, Audio In
// silent, nothing for the host.
bool Engine::idleSlice(int64_t ms) {
    if (!live_ || state_.load() != State::Running || hostRate_ <= 0) { idling_ = false; return false; }
    if (steadyMs() - lastProcessMs_.load() < kIdleQuietMs) { idling_ = false; return false; }   // the host is back
    Machine& m = *live_;
    if (!idling_.exchange(true)) outParser_.reset();  // starting: no partial output message carried over
    retime_ = true;                                  // the next host block starts afresh
    m.liveIn = &g_silence;
    m.profile = false;
    drainButtons(m);
    sendControls(m);
    const bool editor = editorOpen_.load(std::memory_order_relaxed);
    if (vin_.peekSize()) takeVirtualIn(m, m.cycles(), editor);
    if (libIn_.peekSize() || libInFlight_.load(std::memory_order_relaxed)) takeLibraryIn(m, m.cycles(), editor);
    if (edOut_.peekSize()) {
        if (!editor) { while (edOut_.peekSize()) edOut_.pop([](uint8_t) {}); }
        else takeEditorOut(m, m.cycles());
    }
    expireRequests(m);
    const Machine::Stop st = m.runUntil(m.cycles() + m.cyclesFromMs(double(ms)));
    m.wet.clear(); m.audio.clear();
    midiOut(m, 1, false);                            // the virtual port and the editor's replies; never the host
    m.midiOut.clear();
    ++idleSlices_;
    if (st == Machine::Stop::Trap || st == Machine::Stop::HardStall) { machineStopped(m, int(st)); return false; }
    publishDisplay(m);
    serveSnapshot(m);                                // a getState asked just before the host stopped
    return true;
}

// ------------------------------------------------------------------ MIDI ports

void Engine::virtualMidiIn(const uint8_t* bytes, size_t n) {
    const uint64_t parserDrops = vinParser_.dropped();
    for (size_t i = 0; i < n; ++i)
        if (vinParser_.feed(bytes[i]) && !vin_.push(vinParser_.data(), vinParser_.size())) ++vinDropped_;
    vinDropped_ += vinParser_.dropped() - parserDrops;
}

// The OS's MIDI output of this block: whole messages to the virtual port and, at the time their audio plays, to the
// host. A machine sample T sounds at host sample (T - anchor + lookahead) / r.
void Engine::midiOut(Machine& m, int n, bool toHost) {
    if (m.midiOut.empty()) return;
    const double r = 44100.0 / hostRate_, cyclesPerSample = m.config().cpuHz / 44100.0;
    for (const auto& e : m.midiOut) {
        if (!outParser_.feed(e.second)) continue;
        // A reply to the editor's request: to the editor, not to the host or the virtual port. Replies to other
        // senders' requests pass unchanged.
        if (pendingCount_ && edproto::isReply(outParser_.data(), outParser_.size()) &&
            replyOwner(outParser_.data(), outParser_.size()) == 1) {
            edReplies_.push(outParser_.data(), outParser_.size());
            continue;
        }
        if (!toHost) { emitMidiOut(outParser_.data(), outParser_.size(), 0, false); continue; }
        const double h = (double(e.first) / cyclesPerSample - double(anchorSample_) + resampler_.lookahead()) / r - double(hostPos_);
        emitMidiOut(outParser_.data(), outParser_.size(), std::clamp(int(h), 0, n - 1));
    }
}

void Engine::emitMidiOut(const uint8_t* d, size_t size, int offset, bool toHost) {
    if (virtualOn_.load(std::memory_order_relaxed) && !vout_.push(d, size)) ++voutDropped_;
    if (!toHost || !hostOutOn_.load(std::memory_order_relaxed)) return;
    if (hostUsed_ + size > hostBytes_.size() || hostEvents_.size() == hostEvents_.capacity()) { ++hostOutDropped_; return; }
    std::memcpy(hostBytes_.data() + hostUsed_, d, size);
    hostEvents_.push_back({offset, uint32_t(hostUsed_), uint32_t(size)});
    hostUsed_ += size;
}

// ------------------------------------------------------------------ editor

void Engine::setEditorOpen(bool on) {
    editorOpen_ = on;
    if (!on) setEditorStandingRead({});
}

bool Engine::editorSend(const uint8_t* m, size_t n) {
    if (!editorOpen_.load()) return false;
    std::lock_guard<std::mutex> lk(edSendMtx_);
    return edOut_.push(m, n);
}

// Seqlock: the sequence is odd while the bytes change; the audio thread copies and retries if it changed meanwhile.
void Engine::setEditorStandingRead(const std::vector<std::vector<uint8_t>>& msgs) {
    std::lock_guard<std::mutex> lk(standingMtx_);
    standingSeq_.fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    size_t p = 0;
    for (const auto& m : msgs) {
        if (m.empty() || p + 2 + m.size() + 2 > standing_.size()) break;   // whole messages only
        standing_[p++].store(uint8_t(m.size() >> 8), std::memory_order_relaxed);
        standing_[p++].store(uint8_t(m.size()), std::memory_order_relaxed);
        for (uint8_t b : m) standing_[p++].store(b, std::memory_order_relaxed);
    }
    standing_[p].store(0, std::memory_order_relaxed);
    standing_[p + 1].store(0, std::memory_order_relaxed);
    standingSeq_.fetch_add(1, std::memory_order_release);
}

void Engine::queueStandingRead(Machine& m, uint64_t at) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t s1 = standingSeq_.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        size_t p = 0;
        for (;;) {
            if (p + 2 > standing_.size()) break;
            const size_t len = size_t(standing_[p].load(std::memory_order_relaxed)) << 8 | standing_[p + 1].load(std::memory_order_relaxed);
            standingCopy_[p] = uint8_t(len >> 8);
            standingCopy_[p + 1] = uint8_t(len);
            if (len == 0 || p + 2 + len > standing_.size()) break;
            for (size_t i = 0; i < len; ++i) standingCopy_[p + 2 + i] = standing_[p + 2 + i].load(std::memory_order_relaxed);
            p += 2 + len;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (standingSeq_.load(std::memory_order_relaxed) != s1) continue;
        for (size_t q = 0; q + 2 <= standingCopy_.size();) {
            const size_t len = size_t(standingCopy_[q]) << 8 | standingCopy_[q + 1];
            if (len == 0 || q + 2 + len > standingCopy_.size()) break;
            queueEditor(m, standingCopy_.data() + q + 2, len, at);
            q += 2 + len;
        }
        return;
    }
}

void Engine::queueEditor(Machine& m, const uint8_t* d, size_t n, uint64_t at) {
    m.serial.queueRx(1, d, n, at);
    if (edproto::isRequest(d, n)) {
        noteRequest(true, d, n, m.samplesProduced());
        ++editorRequests_;
    }
}

void Engine::noteRequest(bool editor, const uint8_t* d, size_t n, uint64_t sample) {
    if (pendingCount_ == pending_.size()) {          // full: the oldest gives way
        std::copy(pending_.begin() + 1, pending_.end(), pending_.begin());
        --pendingCount_;
    }
    PendingRequest& p = pending_[pendingCount_++];
    p.sample = sample;
    p.editor = editor;
    p.command = d[5];
    edproto::addressOf(d, n, p.address);
}

int Engine::replyOwner(const uint8_t* d, size_t n) {
    size_t i = 0;
    if (d[5] == edproto::kError) {                   // the oldest request of any kind
    } else if (d[5] == edproto::kWholeReply) {       // in order: the oldest whole read
        while (i < pendingCount_ && pending_[i].command != edproto::kWholeRead) ++i;
    } else {                                         // the oldest partial read with the address the reply echoes
        edproto::Address a;
        if (!edproto::addressOf(d, n, a)) return -1;
        while (i < pendingCount_ && !(pending_[i].command == edproto::kPartialRead && pending_[i].address == a)) ++i;
    }
    if (i >= pendingCount_) return -1;
    const int owner = pending_[i].editor ? 1 : 0;
    std::copy(pending_.begin() + long(i) + 1, pending_.begin() + long(pendingCount_), pending_.begin() + long(i));
    --pendingCount_;
    return owner;
}

// ------------------------------------------------------------------ tests

std::vector<uint8_t> Engine::machineStateForTest() {
    std::lock_guard<std::mutex> lk(mtx_);
    return live_ ? live_->saveState() : std::vector<uint8_t>();
}

uint32_t Engine::peekForTest(uint32_t addr, int size) {
    std::lock_guard<std::mutex> lk(mtx_);
    return live_ ? live_->peek(addr, size) : 0;
}

std::vector<uint64_t> Engine::midiQueueForTest() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<uint64_t> t;
    for (uint64_t i = midiCount_ > midiTimes_.size() ? midiCount_ - midiTimes_.size() : 0; i < midiCount_; ++i)
        t.push_back(midiTimes_[i % midiTimes_.size()]);
    return t;
}
void Engine::withMachineForTest(const std::function<void(Machine&)>& f) { std::lock_guard<std::mutex> lk(mtx_); if (live_) f(*live_); }
uint64_t Engine::inputUnderrunsForTest() const { return fifo_->underruns; }
uint64_t Engine::anchorForTest() { std::lock_guard<std::mutex> lk(mtx_); return anchorSample_; }
double Engine::cpuHzForTest() { std::lock_guard<std::mutex> lk(mtx_); return live_ ? live_->config().cpuHz : 0; }
