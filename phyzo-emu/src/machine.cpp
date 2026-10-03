#include "machine.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <atomic>
#include <thread>
#include "os_profile.h"
#include "state_io.h"

extern "C" {
#include "m68k.h"
unsigned long phyzo_cpu_state_size(void);
void phyzo_cpu_save(void* dst);
void phyzo_cpu_save_portable(void* dst);
void phyzo_cpu_load(const void* src);
}

Machine* g_machine = nullptr;

// Musashi has one global CPU. Several machines (plugin instances) take turns on it: whoever runs 68k code holds the
// lock, and the CPU state of the previous owner is parked in that machine until it runs again. Only the 68k needs it:
// each machine's devices (timers, serial, voice chip, ESP2) are its own and run outside the lock, so instances run
// their voice chips and ESP2s in parallel. The lock is held for one 68k slice (at most one sample period) at a time;
// a waiter spins briefly (the holder releases within microseconds) before yielding, as sleeping on a mutex would cost
// a wake-up latency per slice.
namespace {
std::atomic<bool> g_cpuBusy{false};
Machine* g_cpuOwner = nullptr;

inline void cpuRelax() {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

void lockCpu() {
    for (int spins = 0;;) {
        if (!g_cpuBusy.exchange(true, std::memory_order_acquire)) return;
        while (g_cpuBusy.load(std::memory_order_relaxed)) {
            if (++spins < 20000) cpuRelax(); else std::this_thread::yield();
        }
    }
}
void unlockCpu() { g_cpuBusy.store(false, std::memory_order_release); }
}

struct CpuLock {
    Machine& m;
    explicit CpuLock(Machine& mm) : m(mm) { lockCpu(); m.becomeCpuOwner(); m.cpuHeld_ = true; }
    ~CpuLock() { m.cpuHeld_ = false; unlockCpu(); }
};

void Machine::becomeCpuOwner() {
    if (g_cpuOwner != this) {
        if (g_cpuOwner) {
            g_cpuOwner->cpuState_.resize(phyzo_cpu_state_size());
            phyzo_cpu_save(g_cpuOwner->cpuState_.data());
        }
        if (cpuState_.size() == phyzo_cpu_state_size()) phyzo_cpu_load(cpuState_.data());
        g_cpuOwner = this;
    }
    g_machine = this;
}

namespace {
inline uint32_t be(const uint8_t* p, int size) {
    switch (size) {
    case 1: return p[0];
    case 2: return uint32_t(p[0]) << 8 | p[1];
    default: return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    }
}
inline void putBe(uint8_t* p, uint32_t v, int size) {
    for (int i = 0; i < size; ++i) p[i] = uint8_t(v >> (8 * (size - 1 - i)));
}
}

Machine::Machine() : flash_(kFlashSize, 0xff), ram_(kRamSize, 0) {}
Machine::~Machine() {
    lockCpu();
    if (g_cpuOwner == this) g_cpuOwner = nullptr;
    if (g_machine == this) g_machine = nullptr;
    unlockCpu();
    if (voice.log) std::fclose(voice.log);
    if (voice.resonanceLog) std::fclose(voice.resonanceLog);
    if (esp2.log) std::fclose(esp2.log);
    if (esp2stub.log) std::fclose(esp2stub.log);
}

bool Machine::init(const OsImage& os, const Config& cfg, std::string& err) {
    CpuLock cpu(*this);
    cfg_ = cfg;
    // A3: flash 0x0-0xFFFFF erased (0xFF), A2: OS image at 0x4000 (read-only).
    std::fill(flash_.begin(), flash_.end(), 0xff);
    std::fill(ram_.begin(), ram_.end(), 0);                     // A5: RAM zero-filled
    if (OsImage::kLoadAddress + os.bytes.size() > 0xF0000) { err = "OS image overlaps user flash"; return false; }
    std::memcpy(&flash_[OsImage::kLoadAddress], os.bytes.data(), os.bytes.size());

    // A4: synthetic 1 KB vector table and trap stubs in the boot block.
    std::fill(flash_.begin(), flash_.begin() + OsImage::kLoadAddress, 0x00);
    auto put32 = [&](uint32_t a, uint32_t v) { putBe(&flash_[a], v, 4); };
    auto stop = [&](uint32_t a) { putBe(&flash_[a], 0x4E722700, 4); };   // STOP #$2700
    for (uint32_t v = 2; v < 256; ++v) { put32(v * 4, kVectorStubs + v * 4); stop(kVectorStubs + v * 4); }
    put32(0x000, profile::kInitialSsp);   // slot 0: initial SSP (informational)
    put32(0x004, kRebootStub);            // slot 1: reboot service -> host trap
    put32(0x3EC, 0);                      // slot 251: no flash-write service (saves skipped)
    put32(0x3F8, kOsUpdateStub);          // slot 254: OS update service -> host trap
    stop(kRebootStub); stop(kOsUpdateStub); stop(kMainReturnStub);

    // Serial character times.
    serial.ch_[0].byteCycles = uint64_t(cfg_.panelByteUs * cfg_.cpuHz / 1e6);
    serial.ch_[1].byteCycles = uint64_t(cfg_.midiByteUs * cfg_.cpuHz / 1e6);

    panel.setFont(&flash_[profile::kSegmentFont]);
    panel.cyclesPerMs = uint64_t(cfg_.cpuHz / 1000.0);
    panel.sendToOs = [this](const std::vector<uint8_t>& b, uint64_t when) { serial.queueRx(0, b, when); };

    // A1/A6: 68020-mode core, SR = 0x2700, VBR = 0, SSP = 0x0BE30000, call main. The core starts from the cleared
    // state the first machine in a process finds (reset leaves D/A registers alone; they must not carry over from
    // whichever machine used the shared core last).
    cpuSnap_.assign(phyzo_cpu_state_size(), 0);
    phyzo_cpu_load(cpuSnap_.data());
    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68020);
    m68k_pulse_reset();
    m68k_set_reg(M68K_REG_SR, 0x2700);
    m68k_set_reg(M68K_REG_VBR, 0);
    uint32_t sp = profile::kInitialSsp - 4;
    putBe(&ram_[sp - kRamBase], kMainReturnStub, 4);    // main() is a C function: return traps
    m68k_set_reg(M68K_REG_SP, sp);
    m68k_set_reg(M68K_REG_PC, profile::kMainEntry);
    cycles_ = 0;
    voiceSamples_ = 0; nextSample_ = sampleCycle(1);
    return true;
}

bool Machine::loadWaveMemory(const std::string& path, std::string& err) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { err = "cannot open wave image " + path; return false; }
    std::vector<uint8_t> b; uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    if (b.size() != 0x400000) { err = "wave image must be 4 MB (2,097,152 16-bit samples)"; return false; }
    std::vector<int16_t> s(b.size() / 2);
    for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(b[2 * i] | (b[2 * i + 1] << 8));   // little-endian
    voice.setBank(2, std::move(s));     // internal wave memory: chip address base 0x20000000
    return true;
}

void Machine::syncEsp2() {
    if (cfg_.esp2Stub) return;
    const uint64_t ips = uint64_t(cfg_.esp2InstrPerSample);
    const uint64_t c = now(), c0 = sampleCycle(voiceSamples_);
    uint64_t frac = c > c0 ? uint64_t(double(c - c0) * double(ips) * VoiceCore::kOutputRate / cfg_.cpuHz) : 0;
    if (frac >= ips) frac = ips - 1;
    if (profile) {
        auto t0 = std::chrono::steady_clock::now();
        esp2.runTo(voiceSamples_ * ips + frac);
        profEsp2 += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    } else esp2.runTo(voiceSamples_ * ips + frac);
}

void Machine::setTraceDir(const std::string& dir) {
    voice.log = std::fopen((dir + "/voice_writes.csv").c_str(), "w");
    if (voice.log) std::fprintf(voice.log, "t_ms,pc,page,reg,size,value\n");
    voice.resonanceLog = std::fopen((dir + "/resonance_register_writes.csv").c_str(), "w");
    if (voice.resonanceLog) std::fprintf(voice.resonanceLog, "t_ms,pc,voice,reg,value,cr_at_write\n");
    FILE* el = std::fopen((dir + "/esp2_commands.csv").c_str(), "w");
    if (el) std::fprintf(el, "t_ms,source,op,addr,value\n");
    if (cfg_.esp2Stub) esp2stub.log = el; else esp2.log = el;
}

uint32_t Machine::currentPc() const { return m68k_get_reg(nullptr, M68K_REG_PPC); }

uint64_t Machine::now() { return inExecute_ ? sliceStart_ + uint64_t(m68k_cycles_run()) : cycles_; }

void Machine::reschedule() { if (inExecute_) m68k_end_timeslice(); }

void Machine::irqChanged() {
    // A device raised or lowered a line while the devices advance outside the CPU lock: Musashi is told now, as it
    // always was (m68k_set_irq may take the interrupt at once), with the lock held for that call.
    if (!cpuHeld_) { CpuLock cpu(*this); updateIrq(); return; }
    int old = irqLevel_;
    updateIrq();
    if (irqLevel_ != old && inExecute_) m68k_end_timeslice();
}

void Machine::updateIrq() {
    int lvl = std::max(timer1.irqLevel(), serial.irqLevel());
    if (voice.irqLine()) lvl = std::max(lvl, 5);   // voice chip: external IRQ5, autovectored
    if (lvl != irqLevel_) { irqLevel_ = lvl; m68k_set_irq(unsigned(lvl)); }
}

int Machine::intAck(int level) {
    if (level >= 0 && level < 8) ++irqTaken[level];
    if (level == 5) return int(M68K_INT_ACK_AUTOVECTOR);       // A7: voice chip, vector 29
    if (timer1.irqLevel() == level) return timer1.vector();
    if (serial.irqLevel() == level) return serial.vector();
    ++spuriousAcks;
    return int(M68K_INT_ACK_SPURIOUS);
}

void Machine::advanceDevices() {
    uint64_t t = now();
    timer1.advance(t);
    serial.advance(t);
    while (nextSample_ <= t) {                     // voice chip: one output sample per 1/44100 s
        int32_t l, r;
        std::chrono::steady_clock::time_point pt0;
        if (profile) pt0 = std::chrono::steady_clock::now();
        voice.tick(l, r);
        std::chrono::steady_clock::time_point pt1;
        if (profile) { pt1 = std::chrono::steady_clock::now(); profVoice += std::chrono::duration<double>(pt1 - pt0).count(); }
        if (captureAudio) { audio.push_back(l); audio.push_back(r); }
        if (!cfg_.esp2Stub) {
            // ESP2: finish this sample period, then the period edge (LRCLK/IOZ). The voice chip's channel sums
            // reach the ESP2 as 24-bit words at external address 0xC00000 + 2*channel (+1 = right): 20-bit chip
            // output, left-justified (Assumed), clamped at the chip's 20-bit output width (Assumed).
            esp2.runTo((voiceSamples_ + 1) * uint64_t(cfg_.esp2InstrPerSample));
            for (int c = 0; c < VoiceCore::kChannels; ++c)
                for (int side = 0; side < 2; ++side) {
                    int64_t v = voice.chan[c][side];
                    if (v > (1 << 19) - 1) { v = (1 << 19) - 1; ++voicePortClips; }
                    if (v < -(1 << 19)) { v = -(1 << 19); ++voicePortClips; }
                    esp2.voicePort[2 * c + side] = int32_t(v) * 16;
                }
            esp2.voicePort[31] = 0;                // status word read by the shell (meaning Open)
            if (liveIn) liveIn->next(esp2.serialIn[0], esp2.serialIn[1]);
            else if (!audioIn.empty()) {
                for (int i = 0; i < 2; ++i) {
                    float x = std::clamp(audioIn[(audioInPos + i) % audioIn.size()], -1.0f, 1.0f);
                    esp2.serialIn[i] = int32_t(x * 8388607.0f);
                }
                audioInPos = (audioInPos + 2) % audioIn.size();
            }
            esp2.sampleTick();
            if (profile) profEsp2 += std::chrono::duration<double>(std::chrono::steady_clock::now() - pt1).count();
            if (captureAudio) { wet.push_back(esp2.dacOut[0]); wet.push_back(esp2.dacOut[1]); }
        }
        nextSample_ = sampleCycle(++voiceSamples_ + 1);
    }
}

uint64_t Machine::nextEvent() const { return std::min({timer1.nextEvent(), serial.nextEvent(), nextSample_}); }

Machine::Stop Machine::runUntil(uint64_t limit, const std::function<bool()>& pred) {
    auto t0 = std::chrono::steady_clock::now();
    Stop result = Stop::TimeLimit;
    const uint64_t window = cyclesFromMs(cfg_.stallWindowMs);
    while (cycles_ < limit) {
        advanceDevices();                          // this machine's own devices: outside the CPU lock
        uint32_t pc;
        {
            std::chrono::steady_clock::time_point w0;
            if (profile) w0 = std::chrono::steady_clock::now();
            CpuLock cpu(*this);
            if (profile) profCpuWait += std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
            updateIrq();
            uint64_t next = std::min(limit, nextEvent());
            uint64_t budget = next > cycles_ ? next - cycles_ : 1;
            if (budget > cfg_.maxSlice) budget = cfg_.maxSlice;
            sliceStart_ = cycles_;
            inExecute_ = true;
            int used = m68k_execute(int(budget));
            inExecute_ = false;
            cycles_ += uint64_t(used > 0 ? used : 1);

            pc = m68k_get_reg(nullptr, M68K_REG_PC);
            // Trap stubs: PC sits just past a STOP in the boot block.
            if (pc > kVectorStubs && pc <= kVectorStubs + 0x400 && ((pc - kVectorStubs) & 3) == 0) {
                uint32_t vec = (pc - kVectorStubs) / 4 - 1;
                char b[96]; std::snprintf(b, sizeof b, "unexpected exception, vector %u", vec);
                trapReason = b; trapPc = m68k_get_reg(nullptr, M68K_REG_PPC);
                result = Stop::Trap; break;
            }
        }
        if (pc == kRebootStub + 4) { trapReason = "reboot service called (jmp via 0x4)"; result = Stop::Trap; break; }
        if (pc == kOsUpdateStub + 4) { trapReason = "OS update service called (jmp via 0x3F8)"; result = Stop::Trap; break; }
        if (pc == kMainReturnStub + 4) { trapReason = "main() returned"; result = Stop::Trap; break; }

        if (sampling) ++pcSamples[pc];
        // Hard stall: most samples in a window sit on one `bra *`.
        ++windowSamples_;
        if (peek(pc, 2) == 0x60FE) {
            if (pc != windowBraPc_) { windowBraPc_ = pc; windowAtBra_ = 0; }
            ++windowAtBra_;
        }
        if (cycles_ - windowStart_ >= window) {
            if (windowSamples_ > 8 && windowAtBra_ * 10 >= windowSamples_ * 9) {
                stallPc = windowBraPc_;
                result = Stop::HardStall;
                windowStart_ = cycles_; windowSamples_ = windowAtBra_ = 0;
                break;
            }
            windowStart_ = cycles_; windowSamples_ = windowAtBra_ = 0;
        }
        if (pred && pred()) { result = Stop::Predicate; break; }
    }
    hostSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return result;
}

// ------------------------------------------------------------------ bus

void Machine::noteDevice(const char* dev, char rw, uint32_t a, int size, uint32_t v) {
    DevAccess& d = devRing[devRingCount % kDevRing];
    d = {now(), inDma_ ? 0xFFFFFFFFu : currentPc(), dev[0], rw, size, a, v};
    ++devRingCount;
}

std::vector<Machine::DevAccess> Machine::recentDeviceAccesses(size_t n) const {
    std::vector<DevAccess> out;
    uint64_t start = devRingCount > n ? devRingCount - n : 0;
    if (devRingCount - start > kDevRing) start = devRingCount - kDevRing;
    for (uint64_t i = start; i < devRingCount; ++i) out.push_back(devRing[i % kDevRing]);
    return out;
}

void Machine::logUnmapped(uint32_t a, char rw, int size, uint32_t v) {
    AccessStat& s = unmapped[std::make_tuple(a, rw, size)];
    if (s.count == 0) { s.firstPc = inDma_ ? 0xFFFFFFFFu : currentPc(); s.firstCycle = now(); }
    ++s.count;
    s.lastValue = v;
    noteDevice("unmapped", rw, a, size, v);
}

uint32_t Machine::moduleRead8(uint32_t off, bool& ok) {
    ok = true;
    if (off >= 0x600 && off < 0x640) return timer1.read8(off - 0x600);
    if (off >= 0x700 && off < 0x740) { bool h; uint8_t v = serial.read8(off - 0x700, h); ok = h; return v; }
    if (off >= 0x780 && off < 0x7c0) return dma1.read8(off - 0x780);
    ok = false;   // SIM40, timer 2 (hardware test only), DMA ch2: not modelled
    return 0;
}

void Machine::moduleWrite8(uint32_t off, uint8_t v, bool& ok) {
    ok = true;
    if (off >= 0x600 && off < 0x640) { timer1.write8(off - 0x600, v); return; }
    if (off >= 0x700 && off < 0x740) { bool h; serial.write8(off - 0x700, v, h); ok = h; return; }
    if (off >= 0x780 && off < 0x7c0) { dma1.write8(off - 0x780, v); return; }
    ok = false;
}

uint32_t Machine::read(uint32_t a, int size) {
    if (a - kRamBase <= kRamSize - size) return be(&ram_[a - kRamBase], size);
    if (a <= kFlashSize - size) return be(&flash_[a], size);
    if (a - kVoiceBase < kVoiceSize) {
        voice.timeMs = ms(now()); voice.pc = currentPc();
        uint32_t v;
        if (((a & 3) + size) <= 4) v = voice.read(a - kVoiceBase, size);
        else { v = 0; for (int i = 0; i < size; ++i) v = (v << 8) | voice.read(a + i - kVoiceBase, 1); }
        noteDevice("voice", 'R', a, size, v);
        irqChanged();                                // an IRQV read acknowledges the voice interrupt
        return v;
    }
    if (a - kEsp2Base < kEsp2Size) {
        uint32_t v = 0;
        if (cfg_.esp2Stub) for (int i = 0; i < size; ++i) v = (v << 8) | esp2stub.read8(a + i - kEsp2Base, now(), cfg_.cpuHz);
        else { syncEsp2(); for (int i = 0; i < size; ++i) v = (v << 8) | esp2.read8(a + i - kEsp2Base); }
        noteDevice("esp2", 'R', a, size, v);
        return v;
    }
    if (a - kModuleBase < kModuleSize) {
        uint32_t v = 0; bool all = true;
        for (int i = 0; i < size; ++i) { bool ok; v = (v << 8) | moduleRead8(a + i - kModuleBase, ok); all &= ok; }
        if (!all) logUnmapped(a, 'R', size, v); else noteDevice("m68340", 'R', a, size, v);
        return v;
    }
    logUnmapped(a, 'R', size, 0);
    return 0;
}

void Machine::write(uint32_t a, uint32_t v, int size) {
    if (a - kRamBase <= kRamSize - size) { putBe(&ram_[a - kRamBase], v, size); return; }
    if (a < kFlashSize) { ++flashWrites; logUnmapped(a, 'W', size, v); return; }   // read-only
    if (a - kVoiceBase < kVoiceSize) {
        voice.timeMs = ms(now()); voice.pc = inDma_ ? 0xFFFFFF : currentPc();
        if (((a & 3) + size) <= 4) voice.write(a - kVoiceBase, v, size);
        else for (int i = 0; i < size; ++i) voice.write(a + i - kVoiceBase, (v >> (8 * (size - 1 - i))) & 0xff, 1);
        noteDevice("voice", 'W', a, size, v);
        return;
    }
    if (a - kEsp2Base < kEsp2Size) {
        std::string src;
        if (inDma_) src = "DMA";
        else { char b[16]; std::snprintf(b, sizeof b, "%06X", currentPc()); src = b; }
        if (cfg_.esp2Stub) {
            esp2stub.timeMs = ms(now()); esp2stub.source = src;
            for (int i = 0; i < size; ++i) esp2stub.write8(a + i - kEsp2Base, (v >> (8 * (size - 1 - i))) & 0xff);
        } else {
            syncEsp2();
            esp2.timeMs = ms(now()); esp2.source = src;
            for (int i = 0; i < size; ++i) esp2.write8(a + i - kEsp2Base, (v >> (8 * (size - 1 - i))) & 0xff);
        }
        noteDevice("esp2", 'W', a, size, v);
        return;
    }
    if (a - kModuleBase < kModuleSize) {
        bool all = true;
        for (int i = 0; i < size; ++i) {
            bool ok; moduleWrite8(a + i - kModuleBase, (v >> (8 * (size - 1 - i))) & 0xff, ok); all &= ok;
        }
        if (!all) logUnmapped(a, 'W', size, v); else noteDevice("m68340", 'W', a, size, v);
        return;
    }
    logUnmapped(a, 'W', size, v);
}

uint32_t Machine::peek(uint32_t a, int size) const {
    if (a - kRamBase <= kRamSize - size) return be(&ram_[a - kRamBase], size);
    if (a <= kFlashSize - size) return be(&flash_[a], size);
    return 0;
}

uint8_t Machine::dmaRead8(uint32_t addr) {
    inDma_ = true; uint8_t v = uint8_t(read(addr, 1)); inDma_ = false; return v;
}
void Machine::dmaWrite8(uint32_t addr, uint8_t v) {
    inDma_ = true; write(addr, v, 1); inDma_ = false;
}

void Machine::serialTx(int ch, uint8_t b, uint64_t when) {
    if (ch == 0) panel.onOsByte(b, when);
    else midiOut.emplace_back(when, b);
}

void Machine::serialOutputPort(uint8_t op, uint64_t when) { panel.onResetPin(op & 1, when); }

void Machine::instrHook(uint32_t pc) {
    pcRing[pcRingPos++ & 255] = pc;
    ++instructions;
    if (pc - 0x4080u < 0x200u) {
        for (const auto& c : profile::kCheckpoints)
            if (c.pc == pc) {
                bool seen = false;
                for (auto& h : checkpointHits) if (h.first == pc) { seen = true; break; }
                if (!seen) checkpointHits.emplace_back(pc, now());
            }
    }
}

// ------------------------------------------------------------------ state (plugin projects)

namespace {
constexpr uint32_t kStateVersion = 1;
}

std::vector<uint8_t> Machine::saveState() {
    std::vector<uint8_t> out;
    saveStateInto(out);
    return out;
}

// Writes into `out`, reusing its capacity (no allocation once it is large enough: used on the audio thread).
void Machine::saveStateInto(std::vector<uint8_t>& out) {
    CpuLock cpu(*this);
    StateWriter w;
    out.clear();
    w.bytes.swap(out);
    w.raw("PHZM", 4);
    w.put(kStateVersion);
    w.section("MACH", [&](StateWriter& s) {
        s.put(cfg_.cpuHz); s.put(cfg_.esp2InstrPerSample); s.put(cfg_.esp2Stub);
        s.put(cycles_); s.put(voiceSamples_); s.put(nextSample_); s.put(irqLevel_);
        s.put(windowStart_); s.put(windowSamples_); s.put(windowAtBra_); s.put(windowBraPc_);
        s.put(audioInPos); s.put(voicePortClips); s.put(flashWrites); s.put(irqTaken); s.put(spuriousAcks);
        s.vec(ram_);
    });
    w.section("CPU ", [&](StateWriter& s) {
        cpuSnap_.resize(phyzo_cpu_state_size());        // kept between saves: no allocation after the first
        phyzo_cpu_save_portable(cpuSnap_.data());
        s.vec(cpuSnap_);
    });
    w.section("TIM1", [&](StateWriter& s) { timer1.save(s); });
    w.section("SER ", [&](StateWriter& s) { serial.save(s); });
    w.section("DMA1", [&](StateWriter& s) { dma1.save(s); });
    w.section("VOIC", [&](StateWriter& s) { voice.save(s); });
    w.section("ESP2", [&](StateWriter& s) { esp2.save(s); });
    w.section("PANL", [&](StateWriter& s) { panel.save(s); });
    out.swap(w.bytes);
}

bool Machine::loadState(const std::vector<uint8_t>& data, std::string& err) {
    CpuLock cpu(*this);
    StateReader r(data.data(), data.size());
    char magic[4] = {};
    r.raw(magic, 4);
    if (!r.ok() || std::memcmp(magic, "PHZM", 4) != 0) { err = "not a machine state"; return false; }
    if (r.get<uint32_t>() != kStateVersion) { err = "machine state from another version"; return false; }
    if (cfg_.esp2Stub) { err = "machine state needs the ESP2 core"; return false; }
    std::string tag; StateReader s(nullptr, 0);
    int seen = 0;
    while (r.nextSection(tag, s)) {
        if (tag == "MACH") {
            double hz = 0; int ips = 0; bool stub = true;
            s.get(hz); s.get(ips); s.get(stub);
            if (hz != cfg_.cpuHz || ips != cfg_.esp2InstrPerSample || stub) { err = "machine state from another configuration"; return false; }
            s.get(cycles_); s.get(voiceSamples_); s.get(nextSample_); s.get(irqLevel_);
            s.get(windowStart_); s.get(windowSamples_); s.get(windowAtBra_); s.get(windowBraPc_);
            s.get(audioInPos); s.get(voicePortClips); s.get(flashWrites); s.get(irqTaken); s.get(spuriousAcks);
            s.vecExact(ram_, kRamSize);
        } else if (tag == "CPU ") {
            std::vector<uint8_t> c;
            s.vec(c);
            if (c.size() != phyzo_cpu_state_size()) { err = "CPU state from another core version"; return false; }
            phyzo_cpu_load(c.data());
        } else if (tag == "TIM1") timer1.load(s);
        else if (tag == "SER ") serial.load(s);
        else if (tag == "DMA1") dma1.load(s);
        else if (tag == "VOIC") voice.load(s);
        else if (tag == "ESP2") esp2.load(s);
        else if (tag == "PANL") panel.load(s);
        else continue;                                           // unknown section from a later version
        if (!s.ok()) { err = "machine state section " + tag + " is damaged"; return false; }
        ++seen;
    }
    if (!r.ok() || seen < 8) { err = "machine state is incomplete"; return false; }
    sliceStart_ = cycles_;
    return true;
}
