// The emulated machine: MC68340 (Musashi in 68020 mode + module models),
// flash, RAM, the voice-chip core, the ESP2 placeholder, and the panel model.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include "esp2_core.h"
#include "esp2_stub.h"
#include "mc68340.h"
#include "os_image.h"
#include "panel_model.h"
#include "voice_core.h"

class Machine : public ModuleHost {
public:
    struct Config {
        double cpuHz = 16.0e6;          // Derived: 8,000 timer counts per 1 ms tick at fsys/2
        uint32_t maxSlice = 4096;       // longest CPU slice between device checks (cycles)
        double panelByteUs = 320.0;     // channel A character time (rate Open)
        double midiByteUs = 320.0;      // channel B character time (31.25 kbaud)
        double stallWindowMs = 200.0;   // hard-stall detector window
        bool esp2Stub = false;          // true: the boot-milestone placeholder instead of the ESP2 core
        int esp2InstrPerSample = 192;   // ESP2 instruction cycles per 44.1 kHz period (Assumed: 33.8688 MHz / 4)
    };

    // Memory map (BOOT_CHECKLIST.md section A)
    static constexpr uint32_t kFlashSize = 0x100000;
    static constexpr uint32_t kRamBase = 0x0BE00000, kRamSize = 0x40000;
    static constexpr uint32_t kVoiceBase = 0x00400000, kVoiceSize = 0x80;
    static constexpr uint32_t kEsp2Base = 0x00800000, kEsp2Size = 0x20;
    static constexpr uint32_t kModuleBase = 0x00F00000, kModuleSize = 0x800;
    // Synthetic boot block: trap stubs (STOP #$2700) the harness recognises.
    static constexpr uint32_t kVectorStubs = 0x1000;   // + vector*4: unexpected exception
    static constexpr uint32_t kRebootStub = 0x2000, kOsUpdateStub = 0x2008, kMainReturnStub = 0x2010;

    Machine();
    ~Machine() override;
    bool init(const OsImage& os, const Config& cfg, std::string& err);   // section A

    // Complete machine state for plugin projects: CPU, RAM, every device, timing. Flash and wave memory are not
    // included (they come from the ROM files, which must be the same ones). load() needs init() and the wave memory
    // first; on failure the machine is left as it was before the call only if the data was rejected up front.
    std::vector<uint8_t> saveState();
    void saveStateInto(std::vector<uint8_t>& out);
    bool loadState(const std::vector<uint8_t>& data, std::string& err);

    // Audio timing: voice-chip samples produced so far, and the CPU cycle at which sample n is produced.
    uint64_t samplesProduced() const { return voiceSamples_; }
    uint64_t cycleOfSample(uint64_t n) const { return sampleCycle(n); }
    void setTraceDir(const std::string& dir);

    enum class Stop { TimeLimit, Trap, HardStall, Predicate };
    Stop runUntil(uint64_t cycleLimit, const std::function<bool()>& pred = {});

    // ModuleHost
    uint64_t now() override;
    void reschedule() override;
    void irqChanged() override;
    uint8_t dmaRead8(uint32_t addr) override;
    void dmaWrite8(uint32_t addr, uint8_t v) override;
    void serialTx(int ch, uint8_t b, uint64_t when) override;
    void serialOutputPort(uint8_t op, uint64_t when) override;

    // CPU bus (called from the Musashi glue)
    uint32_t read(uint32_t a, int size);
    void write(uint32_t a, uint32_t v, int size);
    uint32_t peek(uint32_t a, int size) const;    // side-effect free (disassembly, reports)
    int intAck(int level);
    void instrHook(uint32_t pc);

    double ms(uint64_t cycle) const { return double(cycle) * 1000.0 / cfg_.cpuHz; }
    uint64_t cyclesFromMs(double m) const { return uint64_t(m * cfg_.cpuHz / 1000.0); }
    uint64_t cycles() const { return cycles_; }
    const Config& config() const { return cfg_; }

    // devices
    Timer timer1{*this};
    Serial serial{*this};
    Dma dma1{*this};
    Esp2Core esp2;                       // the ESP2 core (default)
    Esp2Stub esp2stub;                   // boot-milestone placeholder (--esp2-stub)
    // ESP2 effect path: DAC samples (24-bit, interleaved L,R) captured alongside the dry voice sum
    std::vector<int32_t> wet;
    // Live audio input (the plugin's Audio In): one 24-bit word pair per sample period, in order, for the ESP2's
    // serial input. When set, it replaces audioIn. Not part of the machine state.
    struct LiveInput { virtual ~LiveInput() = default; virtual void next(int32_t& l, int32_t& r) = 0; };
    LiveInput* liveIn = nullptr;
    std::vector<float> audioIn;          // optional mic/line test signal (interleaved L,R, +/-1.0), looped
    size_t audioInPos = 0;
    uint64_t voicePortClips = 0;         // channel words clamped to the 20-bit range at the voice-chip output
    void syncEsp2();                     // run the ESP2 up to the current CPU time
    bool profile = false;                // time the voice core and the ESP2 separately (host seconds)
    double profVoice = 0, profEsp2 = 0;
    double profCpuWait = 0;              // waiting for the shared CPU lock (other instances running 68k code)
    VoiceCore voice;
    // audio: one stereo frame per voice-chip sample (44.1 kHz), 20-bit values
    bool captureAudio = false;
    std::vector<int32_t> audio;          // interleaved L,R
    bool loadWaveMemory(const std::string& path, std::string& err);   // native image -> bank 2
    PanelModel panel;
    std::vector<std::pair<uint64_t, uint8_t>> midiOut;

    // diagnostics
    struct AccessStat { uint64_t count = 0; uint32_t firstPc = 0; uint64_t firstCycle = 0; uint32_t lastValue = 0; };
    std::map<std::tuple<uint32_t, char, int>, AccessStat> unmapped;   // (addr, 'R'/'W', size)
    uint64_t flashWrites = 0;
    std::string trapReason;
    uint32_t trapPc = 0;
    uint32_t stallPc = 0;
    std::map<uint32_t, uint64_t> pcSamples;      // PC histogram (enabled by sampling)
    bool sampling = false;
    std::vector<std::pair<uint32_t, uint64_t>> checkpointHits;   // trace build
    uint32_t pcRing[256] = {0};
    uint32_t pcRingPos = 0;
    uint64_t instructions = 0;                   // trace build only
    double hostSeconds = 0;                      // wall time spent in runUntil
    uint64_t irqTaken[8] = {0};
    uint64_t spuriousAcks = 0;
    struct DevAccess { uint64_t cycle; uint32_t pc; char dev; char rw; int size; uint32_t addr; uint32_t value; };
    static constexpr size_t kDevRing = 256;
    DevAccess devRing[kDevRing] = {};
    uint64_t devRingCount = 0;                   // recent device accesses, for stall reports
    std::vector<DevAccess> recentDeviceAccesses(size_t n) const;

private:
    friend struct CpuLock;
    void becomeCpuOwner();               // with the CPU lock held: load this machine's CPU state into Musashi
    std::vector<uint8_t> cpuState_;      // this machine's CPU while another machine owns Musashi
    std::vector<uint8_t> cpuSnap_;       // CPU section buffer for saveStateInto
    uint32_t moduleRead8(uint32_t off, bool& ok);
    void moduleWrite8(uint32_t off, uint8_t v, bool& ok);
    void logUnmapped(uint32_t a, char rw, int size, uint32_t v);
    void noteDevice(const char* dev, char rw, uint32_t a, int size, uint32_t v);
    void updateIrq();
    void advanceDevices();
    uint64_t nextEvent() const;
    uint32_t currentPc() const;

    Config cfg_;
    std::vector<uint8_t> flash_;
    std::vector<uint8_t> ram_;
    uint64_t cycles_ = 0, sliceStart_ = 0;
    uint64_t voiceSamples_ = 0, nextSample_ = 0;
    uint64_t sampleCycle(uint64_t n) const { return uint64_t(double(n) * cfg_.cpuHz / VoiceCore::kOutputRate); }
    bool inExecute_ = false, inDma_ = false;
    bool cpuHeld_ = false;               // this machine holds the CPU lock (set by CpuLock)
    friend struct CpuLock;
    int irqLevel_ = 0;
    // hard-stall detector
    uint64_t windowStart_ = 0, windowSamples_ = 0, windowAtBra_ = 0;
    uint32_t windowBraPc_ = 0;
};

extern Machine* g_machine;
