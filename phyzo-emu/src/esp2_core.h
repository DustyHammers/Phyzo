// ESP2 core: an emulation of the ESP2 effects processor, written for this project from the public
// documents: "ESP2 Part I: Instruction and Hardware Specification" (Andreas, Mauchly,
// Dattorro), the "ESP2 Object Format Specification" (microinstruction format and opcode tables) and US
// patent 5,517,436. No manufacturer code or data is contained here: the microcode is whatever the host (the user's
// OS image) loads through the host port at run time.
//
// Modelled:
//  - 300 x 96-bit instruction memory (10-bit PC), 1,024 x 24-bit operand space (GPR/AOR/SPR), host port.
//  - MAC: 24x24 signed multiply with the fixed normalising left shift, 52-bit accumulator, MAC/MACP/MACZERO seeds,
//    60-bit barrel shift (+8..-7), 48-bit output saturation (13-MSB check), MACRL, MAC latch writes per opcode.
//  - ALU: all 32 opcodes, saturating/non-saturating arithmetic, CCR (IOZ IFLG NB NA N C V LT Z), CMR, skip bits.
//  - Program control: delayed Jcc/JScc/RScc (1 slot), 4-deep PC stack, REPT, BIOZ suspension with its 1-cycle latency.
//  - AGEN: 8 regions (BASE/SIZEM1/END), modulo addressing, plus-one mode, BASE update, 16 DIL/DOL latches,
//    memory access one cycle after the request, DIL usable two cycles after the request.
//  - Inter-unit latencies by evaluation order (per instruction n): memory op of n-1, MAC/AGEN operand fetch,
//    commit of ALU(n-1) result, ALU(n), MAC(n) write, DIL load. This reproduces every rule of Part I section 1.3.
// SPR addresses are not in the parts of the spec available to us; the ones used here are derived from the programs
// (see ESP2_REPORT.md) and named in esp2_core.cpp.
#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

class StateWriter;
class StateReader;

class Esp2Core {
public:
    static constexpr int kInstr = 300, kRegs = 1024;
    Esp2Core();
    bool mailboxAutoClear = false;             // placeholder option; the real core never fakes the mailbox

    // ---- host port (byte offsets 0x00-0x1F from the chip base) ----
    uint8_t read8(uint32_t off);
    void write8(uint32_t off, uint8_t v);

    // ---- clocking ----
    // Instruction cycles per sample period (Assumed 192: 33.8688 MHz / 4 at 44.1 kHz).
    int instrPerSample = 192;
    // Run instructions up to an absolute instruction-cycle count.
    void runTo(uint64_t instrCycle);
    // Sample-period edge (LRCLK / IOZ): latch serial outputs, load serial inputs, raise IOZ.
    void sampleTick();
    uint64_t cycle() const { return cycle_; }

    // Chip state: registers, instruction memory, delay RAM, host port, pipelines, program control, outputs and the
    // run statistics. Options (fastPath, specFixes, ...) and logs are not included.
    void save(StateWriter& w) const;
    void load(StateReader& r);

    // ---- board wiring ----
    // External memory: delay RAM at 0x600000 (size kRamWords), voice-chip channel port at 0xC00000.
    static constexpr uint32_t kRamBase = 0x600000, kRamWords = 0x20000;
    static constexpr uint32_t kVoicePort = 0xC00000;
    std::array<int32_t, 32> voicePort{};       // 24-bit words the ESP2 reads at 0xC00000 + n (n = 2*channel + side)
    int32_t serialIn[2] = {0, 0};              // ADC left/right for the next tick (24-bit)
    int32_t dacOut[2] = {0, 0};                // latched at the last tick (24-bit)
    int32_t auxOut[8] = {0};                   // SER 0x3E4-0x3EB at the last tick
    // audit instrumentation
    // Option: keep the audit bookkeeping on delay-RAM accesses and AGEN (ramLow / ramHigh, the write epochs wEpoch, the
    // stale-read window staleReads / staleLow / staleHigh, regionViolations) for the development tools. Off: those are
    // skipped (they never affect the output); every other statistic is kept. Not state. The plugin's engine runs with
    // it off.
    bool audit = true;
    bool ramAlias = false;                     // option: decode unmapped addresses into RAM by the low 16 bits (64K alias)
    FILE* extLog = nullptr;                    // every external write outside RAM and the voice port (t, pc, addr, value)
    uint32_t staleWinLo = 0, staleWinHi = 0xFFFFFF;   // stale reads are counted only inside this address window
    uint32_t epoch = 1;                        // write epoch; reads of words last written before the current epoch are "stale"
    uint64_t staleReads = 0, regionViolations = 0, extWrites = 0;
    uint32_t staleLow = 0xFFFFFFFF, staleHigh = 0;
    std::vector<uint32_t> wEpoch = std::vector<uint32_t>(kRamWords, 0);   // epoch of the last write per RAM word (0 = never)
    uint32_t ramWord(uint32_t i) const { return i < kRamWords ? ram_[i] : 0; }
    const std::array<std::array<uint8_t, 12>, kInstr>& imem() const { return imem_; }
    bool ram16 = false;
    int watch = 0;                             // debug: print up to this many anomalies (env ESP2_WATCH)                        // option: 16-bit delay RAM (truncate written words); default 24-bit (Open)

    // ---- logging / statistics ----
    FILE* log = nullptr;                       // same format as the placeholder's esp2_commands.csv
    double timeMs = 0; std::string source;
    uint64_t regWrites = 0, regReads = 0, instrWrites = 0, instrReads = 0, controlWrites = 0, readoutReads = 0;
    uint64_t mailboxReadsWhileRunning = 0, mailboxReadsNonZero = 0, badInstrAddr = 0;
    uint64_t executed = 0, suspendedCycles = 0, haltedCycles = 0, biozPasses = 0, biozSuspends = 0, overruns = 0;
    uint64_t macSat = 0, aluSat = 0, dacSatSamples = 0, ticks = 0;
    uint64_t memReads = 0, memWrites = 0, memUnmapped = 0, voicePortReads = 0, voicePortWrites = 0;
    uint64_t reservedMacOps = 0, unknownSprReads = 0, unknownSprWrites = 0;
    uint32_t ramLow = 0xFFFFFFFF, ramHigh = 0;
    std::map<uint32_t, uint64_t> unmappedAddr;   // external addresses outside RAM and the voice port (first 64 kept)
    static constexpr uint32_t kSatKeys = 0x1400;
    // Saturated MAC/ALU results by destination: index = register (ALU), register | 0x1000 (MAC).
    std::array<uint64_t, kSatKeys> satDest{};
    std::array<double, kSatKeys> satOver{};      // largest overshoot beyond full scale per destination, in 24-bit output LSBs
    std::map<uint32_t, uint64_t> unknownSpr;     // accesses to SPR addresses not identified (address -> count)
    uint64_t satToDac = 0;                     // saturated MAC/ALU results written to SER 0x3EC/0x3ED
    int lastControl = -1;
    uint8_t control = 0x04;                    // reset: halted
    bool specFixes = true;                     // false: checkpoint behaviour (pre-spec SPR semantics), for A/B
    bool running() const { return specFixes ? !((control & 0x04) || ((control & 0x03) == 0x03)) : !(control & 0x04); }
    uint32_t pc() const { return pc_; }
    uint32_t reg(int a) const { return r_[a & 0x3FF]; }

    struct Decoded {
        uint16_t A, B, C, D, E, F, G;
        uint8_t alu, mac, sh, ag, rgn, dl;
        bool aluSkip, macSkip, agSkip;
        // translation (computed once when the instruction is written)
        bool pureNop = false;      // ALU MOV REF>REF, MAC NOP pattern, AGEN NOP, no skip bits: only REF advances
        bool macNop = false;       // MAC NOP pattern (MACRL x ONE >>1 > ZERO): no state change at all
        bool macReserved = false, ccClass = false, anySkip = false, aluReadsA = false, aluReadsB = false;
        bool aPlain = false, bPlain = false, cPlain = false, dPlain = false, ePlain = false, fPlain = false;
        uint8_t aluKind = 0;       // 0 general, 1 MOV-class with plain B (result = B), 2 MOV-class with plain B and C = ZERO (no effect)
        uint8_t macSeed = 0;       // 0 MACZERO, 1 MACP, 2 MAC latch
        bool macSub = false, macSeedShift = false, macLatchWrite = false;
        int8_t macShift = 0;       // + = left
        bool hasIndirect = false;  // an operand field names INDIRECT/INDIRINC/INDIRDEC
        uint8_t fKind = 0;         // MAC destination: 0 ZERO (discarded), 1 plain, 2 side effects
        // bind-time constants for stepT: MAC product factor (+2 / -2 for subtract), seed and output shifts as
        // (left, right) pairs, AGEN region END register, +1 mode, BASE update, memory request
        int8_t macMul = 2; uint8_t seedL = 0, seedR = 0, outL = 0, outR = 0;
        uint16_t agEnd = 0; uint8_t agPlus = 0; bool agBase = false, agMem = false;
        // specialised routine for this line (chosen in bind(); see stepT)
        void (*run)(Esp2Core&, const Decoded&) = nullptr;
        uint32_t (*aluFn)(Esp2Core&, const Decoded&, uint32_t, uint32_t, bool, bool&) = nullptr;
    };
    bool fastPath = true;                      // false: reference interpreter (stepRef), for A/B checks

private:
    // registers
    uint32_t readReg(uint32_t a);              // program-side operand read
    void writeReg(uint32_t a, uint32_t v);     // program-side operand write
    void hostRegCommand(uint8_t cmd);
    void hostInstrCommand(uint8_t cmd);
    void decode(int addr);
    static void translate(Decoded& d);
    static void bind(Decoded& d);
    inline void step();
    template <int SK, int AG, int MAC, int ALU, int OP = -1> inline void stepT(const Decoded& d);
    // The line routines: each runs its line, counts the cycle and continues with the next line's routine as a
    // guaranteed tail call (ESP2_NEXT_LINE in esp2_core.cpp).
    template <int SK, int AG, int MAC, int ALU, int OP = -1> static void runT(Esp2Core& c, const Decoded& d);
    static void runStep(Esp2Core& c, const Decoded&);
    static void runNop(Esp2Core& c, const Decoded&);
    uint64_t stop_ = 0;                        // runTo: the current chain of lines ends here (not state)
    template <int OP> inline uint32_t aluFast(const Decoded& d, uint32_t ua, uint32_t ub, bool setFlags);
    void aluSatCold(const Decoded& d, int64_t exact);
    template <int OP> uint32_t aluT(const Decoded& d, uint32_t a, uint32_t b, bool setFlags, bool& write);
    template <int OP> static uint32_t aluS(Esp2Core& c, const Decoded& d, uint32_t a, uint32_t b, bool f, bool& w) { return c.aluT<OP>(d, a, b, f, w); }
    struct Binder;
    void stepRef();
    bool skip() const;
    uint32_t alu(const Decoded& d, uint32_t a, uint32_t b, bool setFlags, bool& write);

    std::array<uint32_t, kRegs> r_{};
    std::array<std::array<uint8_t, 12>, kInstr> imem_{};
    std::array<Decoded, kInstr> dec_{};
    std::vector<uint32_t> ram_ = std::vector<uint32_t>(kRamWords, 0);
    uint8_t port_[0x20] = {0};

    // machine state
    uint64_t cycle_ = 0;
    uint32_t pc_ = 0, npc_ = 1;
    bool biozArmed_ = false, suspended_ = false, iozStatus_ = false;
    uint32_t ccr_ = 0, cmr_ = 0, refpt_ = 0, aluShift_ = 0;
    int64_t macLatch_ = 0, macp_ = 0;          // 52-bit, sign-extended in int64
    uint32_t macrl_ = 0;
    uint32_t pcStack_[4] = {0}; int sp_ = 0;
    uint32_t reptSt_ = 0, reptEnd_ = 0x3FF, reptCnt_ = 0;
    // pipelines
    bool aluPend_ = false, aluPendPlain_ = false; uint32_t aluPendAddr_ = 0, aluPendVal_ = 0;
    struct MemOp { bool valid = false, write = false; uint32_t addr = 0; int latch = 0; } memPend_;
    bool dilPend_ = false; int dilPendLatch_ = 0; uint32_t dilPendVal_ = 0;
    uint32_t memRead(uint32_t addr);
    inline uint32_t dolTrunc(uint32_t v) const;
    uint32_t peekReg(uint32_t a) const;        // register value as the host or a program would read it (no side effects)
    // indirection: pointer histories implementing the patent's pointer latencies
    struct PtrWrite { uint64_t visible; uint32_t value; };
    std::array<std::array<PtrWrite, 4>, 7> ptrPend_{}; std::array<int, 7> ptrN_{};
    char writer_ = 'H';                        // 'A' ALU commit, 'M' MAC, 'H' host (for pointer latencies)
    uint32_t ptrValue(int i);
    void ptrWrite(int i, uint32_t v);
    Decoded resolveIndirect(const Decoded& d, uint8_t incdec[7]);
    void applyIncDec(const uint8_t incdec[7], bool aluEx, bool macEx, bool agEx);
    void memWrite(uint32_t addr, uint32_t v);
};
