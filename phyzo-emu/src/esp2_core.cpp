#include "esp2_core.h"
#include <algorithm>
#include <utility>

// SPR addresses used by this core. The spec sections that list SPR addresses were not available; each address
// below was derived from how the loaded programs use it (ESP2_REPORT.md, "SPR map"). Tags: K = fixed by the spec's
// own encodings (NOP pseudo-instructions in the Object Format appendix), D = derived from program usage.
namespace spr {
constexpr uint32_t PEAK = 0x1C8;      // D  plain register (shell input peak hold)
constexpr uint32_t MINUS1 = 0x1CB;    // D  $800000, E operand of the MAC MOV pseudo
constexpr uint32_t ONE = 0x1CC;       // K  $000001 (MAC NOP = MACRL x ONE >>1 > ZERO)
constexpr uint32_t HALF = 0x1CD;      // D  $400000
constexpr uint32_t MACP_LS = 0x1D0;   // assumed (unused by the programs)
constexpr uint32_t MACP_L = 0x1D1;    // D  ALU-written low half before a MACP-seeded MAC op
constexpr uint32_t MACP_HC = 0x1D2;   // D  "MACP" as destination; consumed by MACP ops two lines after ALU writes
constexpr uint32_t MACP_H = 0x1D3;    // D  MAC-written high half (integer delay offset)
constexpr uint32_t MACRL = 0x1D4;     // K  MAC result low latch
constexpr uint32_t CMR = 0x1DE;       // D  written from a register right before skippable lines (distort2)
constexpr uint32_t DOL0 = 0x1EF;      // D  DOL n = 0x1EF - n (written the line before WR)
constexpr uint32_t DIL0 = 0x1FF;      // D  DIL n = 0x1FF - n (read two lines after RD)
constexpr uint32_t REGION_END0 = 0x3DD; // D region r: END = 0x3DD-3r, SIZEM1 = +1, BASE = +2
constexpr uint32_t SER_ADC_L = 0x3EF, SER_ADC_R = 0x3EE;   // D  serial inputs (mic/line)
constexpr uint32_t SER_DAC_L = 0x3ED, SER_DAC_R = 0x3EC;   // D  serial outputs to the stereo DAC
constexpr uint32_t REF = 0x3F1;       // K  refresh pseudo-register (indirect through REFPT)
constexpr uint32_t ZERO = 0x3FF;      // K  read-only zero
// From the Part I SPR map (section 11), added after the spec became available:
constexpr uint32_t HOST_ESP_FACE = 0x1C8, HOST_CNTL_SPR = 0x1CE, HOST_GPR_DATA = 0x1CF, MACL = 0x1D1, MACH = 0x1D3;
constexpr uint32_t ALU_SHIFT = 0x1D5, REPT_CNT = 0x1D6, REPT_END = 0x1D7, REPT_ST = 0x1D8, PCSTACK3 = 0x1D9, PCSTACK0 = 0x1DC;
constexpr uint32_t PC = 0x1DD, CCR = 0x1DF, HARD_CONF = 0x3F3, SER_CONF = 0x3F2, REFPT = 0x3FD, REFINST = 0x3FE;
constexpr uint32_t INDIRDEC = 0x3F0, INDIRINC = 0x3F8, INDIRECT = 0x3F9;
// pointer register for each operand slot A B C D E F G
constexpr uint32_t INDIR[7] = {0x3FC, 0x3FB, 0x3FA, 0x3C7, 0x3C6, 0x3C5, 0x3C4};
inline int indirIndex(uint32_t a) { for (int i = 0; i < 7; ++i) if (INDIR[i] == a) return i; return -1; }
inline bool sideEffectSpr(uint32_t a) {      // SPRs whose reads or writes are not plain storage (spec semantics)
    return a == HOST_CNTL_SPR || a == MACL || a == MACH || (a >= ALU_SHIFT && a <= CCR) || a == REFPT || a == REFINST || indirIndex(a) >= 0;
}
inline bool identified(uint32_t a) {
    if (a < 0x1C8 || (a >= 0x200 && a < 0x3C4)) return true;
    if (a == PEAK || a == MINUS1 || a == ONE || a == HALF || (a >= MACP_LS && a <= MACRL) || a == CMR) return true;
    if (a == 0x1D6 || a == 0x1DD) return true;          // zeroed by every program's register image (role Open)
    if (a >= 0x1E0 && a <= 0x1FF) return true;          // DOL / DIL
    if (a >= 0x3C8 && a <= 0x3EF) return true;          // regions, serial data
    if (a >= 0x3F1 && a <= 0x3F7) return true;          // REF, SER_CONF, HARD_CONF, SCLK0/1 registers
    if (a >= 0x1C8 && a <= 0x1DF) return true;          // spec SPR map
    if (a >= 0x3C4 && a <= 0x3C7) return true;          // INDIRD..G
    if (a >= 0x3F8 && a <= 0x3FE) return true;          // INDIRINC, INDIRECT, INDIRC..A, REFPT, REFINST
    return a == ZERO || a == 0x3F0;
}
}  // namespace spr

namespace {
constexpr uint32_t F_Z = 1, F_LT = 2, F_V = 4, F_C = 8, F_N = 0x10, F_NA = 0x20, F_NB = 0x40;
inline int32_t sx24(uint32_t v) { return int32_t(v << 8) >> 8; }
inline int64_t wrap52(int64_t v) { return int64_t(uint64_t(v) << 12) >> 12; }
inline int64_t shiftv(int64_t v, int sh) { return sh >= 0 ? int64_t(uint64_t(v) << sh) : (v >> -sh); }
constexpr int64_t kMax48 = (int64_t(1) << 47) - 1, kMin48 = -(int64_t(1) << 47);
}  // namespace

// Power-up state (Part I 1.1.3, 1.1.6): GPR, AOR and instruction memory read as logical 1. Reset leaves the chip
// halted until the host starts it.
Esp2Core::Esp2Core() {
    for (auto& w : imem_) w.fill(0xFF);
    for (int i = 0; i < kInstr; ++i) decode(i);
    for (uint32_t a = 0; a < 0x1C8; ++a) r_[a] = 0xFFFFFF;
    for (uint32_t a = 0x200; a < 0x3C4; ++a) r_[a] = 0xFFFFFF;
    r_[spr::ZERO] = 0; r_[spr::ONE] = 1; r_[spr::MINUS1] = 0x800000; r_[spr::HALF] = 0x400000; r_[spr::MACRL] = 0;
}

// ------------------------------------------------------------------ registers (program side)

uint32_t Esp2Core::readReg(uint32_t a) {
    a &= 0x3FF;
    if (a < 0x1C8 || (a >= 0x200 && a < 0x3C4)) return r_[a];
    switch (a) {
    case spr::ZERO: return 0;
    case spr::ONE: return 1;
    case spr::MINUS1: return 0x800000;
    case spr::HALF: return 0x400000;
    case spr::MACRL: return macrl_;
    case spr::REF: {
        uint32_t ra = ((refpt_ & 1) << 9) | (refpt_ >> 1);
        refpt_ = (refpt_ + 1) & 511;
        return r_[ra];
    }
    default:
        if (specFixes && spr::sideEffectSpr(a)) return peekReg(a);
        if (!spr::identified(a)) { ++unknownSprReads; ++unknownSpr[a]; }
        return r_[a];
    }
}

// Spec SPR semantics (Part I section 11) for registers that are not plain storage.
uint32_t Esp2Core::peekReg(uint32_t a) const {
    switch (a) {
    case spr::ZERO: return 0;
    case spr::ONE: return 1;
    case spr::MINUS1: return 0x800000;
    case spr::HALF: return 0x400000;
    case spr::MACRL: return macrl_;
    default: break;
    }
    if (!specFixes) return r_[a];
    switch (a) {
    case spr::MACH: return uint32_t(macLatch_ >> 24) & 0xFFFFFF;         // unsaturated MAC latch, bits 47:24
    case spr::MACL: return uint32_t(macLatch_) & 0xFFFFFF;
    case spr::HOST_CNTL_SPR: return (control & 0x57) | (iozStatus_ ? 0x08 : 0) | (suspended_ ? 0x20 : 0);
    case spr::ALU_SHIFT: return aluShift_;
    case spr::REPT_CNT: return reptCnt_;
    case spr::REPT_END: return reptEnd_;
    case spr::REPT_ST: return reptSt_;
    case spr::PC: return pc_;
    case spr::CMR: return cmr_;
    case spr::CCR: return (ccr_ & 0x7F) | (iozStatus_ ? 0x100 : 0);
    case spr::REFPT: return refpt_;
    case spr::REFINST: return 0;
    default:
        if (a >= spr::PCSTACK3 && a <= spr::PCSTACK0) return pcStack_[(sp_ + 3 - int(spr::PCSTACK0 - a)) & 3];
        { int i = spr::indirIndex(a); if (i >= 0) return r_[a] & 0x3FF; }
        return r_[a];
    }
}

uint32_t Esp2Core::ptrValue(int i) {
    int k = 0;
    for (int j = 0; j < ptrN_[i]; ++j) {
        if (ptrPend_[i][j].visible <= cycle_) r_[spr::INDIR[i]] = ptrPend_[i][j].value;
        else ptrPend_[i][k++] = ptrPend_[i][j];
    }
    ptrN_[i] = k;
    return r_[spr::INDIR[i]] & 0x3FF;
}

// Pointer-register write latencies (US 5,517,436, "Indirect Register Latencies"): MAC writes to INDIRA-E are usable
// two lines later, to INDIRF one line later; ALU writes to INDIRA,B two, INDIRC one, INDIRD-F three. G (not listed)
// follows the region-register rules: MAC +1, ALU +2.
void Esp2Core::ptrWrite(int i, uint32_t v) {
    static const int macLat[7] = {2, 2, 2, 2, 2, 1, 1}, aluLat[7] = {2, 2, 1, 3, 3, 3, 2};
    if (writer_ == 'H') { r_[spr::INDIR[i]] = v & 0x3FF; ptrN_[i] = 0; return; }
    uint64_t origin = writer_ == 'M' ? cycle_ : cycle_ - 1;      // an ALU result lands one line after its instruction
    uint64_t vis = origin + uint64_t(writer_ == 'M' ? macLat[i] : aluLat[i]);
    if (ptrN_[i] == 4) { r_[spr::INDIR[i]] = ptrPend_[i][0].value; for (int j = 0; j < 3; ++j) ptrPend_[i][j] = ptrPend_[i][j + 1]; ptrN_[i] = 3; }
    ptrPend_[i][ptrN_[i]++] = {vis, v & 0x3FF};
}

void Esp2Core::writeReg(uint32_t a, uint32_t v) {
    a &= 0x3FF; v &= 0xFFFFFF;
    if (a < 0x1C8 || (a >= 0x200 && a < 0x3C4)) { r_[a] = v; return; }
    switch (a) {
    case spr::ZERO: case spr::ONE: case spr::MINUS1: case spr::HALF: case spr::MACRL: case spr::REF: return;
    case spr::MACP_LS: macp_ = sx24(v); break;
    case spr::MACP_L: macp_ = (macp_ & ~int64_t(0xFFFFFF)) | v; break;
    case spr::MACP_HC: macp_ = int64_t(sx24(v)) * (int64_t(1) << 24); break;
    case spr::MACP_H: macp_ = int64_t(sx24(v)) * (int64_t(1) << 24) + (macp_ & 0xFFFFFF); break;
    case spr::CMR: cmr_ = v & 0x3FF; break;
    default:
        if (specFixes) {
            int i = spr::indirIndex(a);
            if (i >= 0) { ptrWrite(i, v); return; }
            switch (a) {
            case spr::HOST_CNTL_SPR: control = uint8_t((control & ~0x02) | (v & 0x02)); iozStatus_ = v & 0x08; break;   // ESP_HALT, IOZ writable
            case spr::ALU_SHIFT: aluShift_ = v; break;
            case spr::REPT_CNT: reptCnt_ = v; break;
            case spr::REPT_END: reptEnd_ = v & 0x3FF; break;
            case spr::REPT_ST: reptSt_ = v & 0x3FF; break;
            case spr::PC: pc_ = v & 0x3FF; npc_ = (pc_ + 1) & 0x3FF; break;                  // "writable with caveat"
            case spr::CCR: ccr_ = v & 0x7F; break;                                           // IFLG, IOZ read-only
            case spr::REFPT: refpt_ = v & 511; break;
            default:
                if (a >= spr::PCSTACK3 && a <= spr::PCSTACK0) pcStack_[(sp_ + 3 - int(spr::PCSTACK0 - a)) & 3] = v & 0x3FF;
                break;
            }
        }
        if (!spr::identified(a)) { ++unknownSprWrites; ++unknownSpr[a | 0x1000]; }
        break;
    }
    r_[a] = v;
}

// ------------------------------------------------------------------ external memory

// Magnitude truncation of WR data (HARD_CONF MAG_TRUNC, TRUNC_WIDTH; patent "Data Interface"): negative values move one
// LSB toward zero, then 16-bit mode clears the 8 LSBs. Both the OS (0x881C) and the programs (0x8C08) select 16 bits.
inline uint32_t Esp2Core::dolTrunc(uint32_t v) const {
    if (ram16 && !specFixes) return v & 0xFFFF00;
    if (!specFixes || !(r_[spr::HARD_CONF] & 0x800)) return v;
    const bool neg = v & 0x800000;
    if (r_[spr::HARD_CONF] & 0x1000) return neg ? (v + 1) & 0xFFFFFF : v;
    return ((neg ? v + 0x100 : v) & 0xFFFF00) & 0xFFFFFF;
}

uint32_t Esp2Core::memRead(uint32_t addr) {
    if (addr - kRamBase < kRamWords) {
        ++memReads; ramLow = std::min(ramLow, addr); ramHigh = std::max(ramHigh, addr);
        if (wEpoch[addr - kRamBase] < epoch && addr >= staleWinLo && addr <= staleWinHi) { ++staleReads; staleLow = std::min(staleLow, addr); staleHigh = std::max(staleHigh, addr); }
        return ram_[addr - kRamBase];
    }
    if ((addr & ~0x1Fu) == kVoicePort) { ++voicePortReads; return uint32_t(voicePort[addr & 31]) & 0xFFFFFF; }
    if (ramAlias) return memRead(kRamBase + (addr & 0xFFFF));
    ++memUnmapped; if (unmappedAddr.size() < 64 || unmappedAddr.count(addr)) ++unmappedAddr[addr];
    return 0;
}

void Esp2Core::memWrite(uint32_t addr, uint32_t v) {
    if (addr - kRamBase < kRamWords) {
        ++memWrites; ramLow = std::min(ramLow, addr); ramHigh = std::max(ramHigh, addr);
        ram_[addr - kRamBase] = dolTrunc(v); wEpoch[addr - kRamBase] = epoch;
        return;
    }
    if ((addr & ~0x1Fu) == kVoicePort) { ++voicePortWrites; voicePort[addr & 31] = sx24(dolTrunc(v)); return; }
    ++extWrites;
    if (extLog) std::fprintf(extLog, "%llu,%03X,%06X,%06X,%s\n", (unsigned long long)cycle_, pc_, addr, v & 0xFFFFFF, ramAlias ? "alias" : "dropped");
    if (ramAlias) { memWrite(kRamBase + (addr & 0xFFFF), v); return; }
    ++memUnmapped; if (unmappedAddr.size() < 64 || unmappedAddr.count(addr)) ++unmappedAddr[addr | 0x80000000u];
}

// ------------------------------------------------------------------ host port

void Esp2Core::decode(int a) {
    const auto& b = imem_[a];
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 4; ++i) hi = (hi << 8) | b[i];
    for (int i = 4; i < 12; ++i) lo = (lo << 8) | b[i];
    Decoded& d = dec_[a];
    d.A = uint16_t((hi >> 22) & 0x3FF); d.B = uint16_t((hi >> 12) & 0x3FF); d.C = uint16_t((hi >> 2) & 0x3FF);
    d.alu = uint8_t(((hi & 3) << 3) | (lo >> 61)); d.aluSkip = (lo >> 60) & 1;
    d.D = uint16_t((lo >> 50) & 0x3FF); d.E = uint16_t((lo >> 40) & 0x3FF); d.F = uint16_t((lo >> 30) & 0x3FF);
    d.mac = uint8_t((lo >> 25) & 0x1F); d.sh = uint8_t((lo >> 21) & 0xF); d.macSkip = (lo >> 20) & 1;
    d.G = uint16_t(((lo >> 11) & 0x1FF) | 0x200); d.ag = uint8_t((lo >> 8) & 7); d.rgn = uint8_t((lo >> 5) & 7);
    d.dl = uint8_t((lo >> 1) & 0xF); d.agSkip = lo & 1;
    translate(d);
    bind(d);
}

// Plain operands read and write r_[] directly with no side effect (everything readReg/writeReg does not special-case).
static bool plainRead(uint32_t a) {
    if (a < 0x1C8 || (a >= 0x200 && a < 0x3C4)) return true;
    if (spr::sideEffectSpr(a) || a == spr::CMR) return false;
    if (a == spr::ZERO || a == spr::ONE || a == spr::MINUS1 || a == spr::HALF || a == spr::MACRL) return true;   // mirrored
    if (a == spr::REF) return false;
    return spr::identified(a);
}
static bool plainWrite(uint32_t a) {
    if (a < 0x1C8 || (a >= 0x200 && a < 0x3C4)) return true;
    if (spr::sideEffectSpr(a)) return false;
    switch (a) { case spr::ZERO: case spr::ONE: case spr::MINUS1: case spr::HALF: case spr::MACRL: case spr::REF:
                 case spr::MACP_LS: case spr::MACP_L: case spr::MACP_HC: case spr::MACP_H: case spr::CMR: return false; }
    return spr::identified(a);
}

Esp2Core::Decoded Esp2Core::resolveIndirect(const Decoded& d0, uint8_t incdec[7]) {
    Decoded d = d0;
    const bool immAB = d.alu == 0x1C || d.alu == 0x1D || d.alu == 0x1E || d.alu == 0x1F;
    uint16_t* f[7] = {&d.A, &d.B, &d.C, &d.D, &d.E, &d.F, &d.G};
    for (int i = 0; i < 7; ++i) {
        incdec[i] = 0;
        if ((i == 0 && (immAB || d.alu == 0x1B)) || (i == 1 && immAB)) continue;
        uint32_t a = *f[i];
        if (a != spr::INDIRECT && a != spr::INDIRINC && a != spr::INDIRDEC) continue;
        *f[i] = uint16_t(ptrValue(i));
        incdec[i] = a == spr::INDIRINC ? 1 : a == spr::INDIRDEC ? 2 : 0;
    }
    d.hasIndirect = false;
    translate(d);
    d.hasIndirect = false;
    return d;
}

void Esp2Core::applyIncDec(const uint8_t incdec[7], bool aluEx, bool macEx, bool agEx) {
    for (int i = 0; i < 7; ++i) {
        if (!incdec[i]) continue;
        const bool ex = i < 3 ? aluEx : i < 6 ? macEx : agEx;
        if (!ex) continue;                                    // a skipped unit does not step its pointer
        uint32_t v = (ptrValue(i) + (incdec[i] == 1 ? 1 : 0x3FF)) & 0x3FF;
        if (ptrN_[i] == 4) { r_[spr::INDIR[i]] = ptrPend_[i][0].value; for (int j = 0; j < 3; ++j) ptrPend_[i][j] = ptrPend_[i][j + 1]; ptrN_[i] = 3; }
        ptrPend_[i][ptrN_[i]++] = {cycle_ + 1, v};            // incremented address usable on the next line
    }
}

void Esp2Core::translate(Decoded& d) {
    d.macReserved = (d.mac >= 0x0C && d.mac < 0x14) || d.mac >= 0x1C;
    d.ccClass = d.alu == 0x1B || d.alu == 0x1C || d.alu == 0x1D || d.alu == 0x1E;
    d.anySkip = d.aluSkip || d.macSkip || d.agSkip;
    const bool isJump = d.alu == 0x1C || d.alu == 0x1D || d.alu == 0x1F;
    d.aluReadsB = !isJump;
    d.aluReadsA = !isJump && d.alu != 0x0B && d.alu != 0x1B && d.alu != 0x1E && d.alu != 0x18 && d.alu != 0x19;
    d.aPlain = plainRead(d.A); d.bPlain = plainRead(d.B); d.dPlain = plainRead(d.D); d.ePlain = plainRead(d.E);
    d.cPlain = plainWrite(d.C); d.fPlain = plainWrite(d.F);
    d.macNop = d.mac == 2 && d.D == spr::MACRL && d.E == spr::ONE && d.sh == 6 && d.F == spr::ZERO;
    d.pureNop = !d.anySkip && d.macNop && d.ag == 6 && d.alu == 0x0B && d.B == spr::REF && d.C == spr::REF;
    const bool movClass = d.alu == 0x0B || d.alu == 0x18 || d.alu == 0x19;
    d.aluKind = (movClass && d.bPlain) ? (d.C == spr::ZERO ? 2 : 1) : 0;
    d.macSeedShift = d.mac >= 0x14;
    d.macSeed = uint8_t(d.macSeedShift ? (d.mac < 0x18 ? 1 : 2) : (d.mac >> 2));
    d.macSub = d.mac & 1; d.macLatchWrite = !(d.mac & 2); d.macShift = int8_t(int(d.sh) - 7);
    d.fKind = d.F == spr::ZERO ? 0 : d.fPlain ? 1 : 2;
    auto ind = [](uint32_t a) { return a == spr::INDIRECT || a == spr::INDIRINC || a == spr::INDIRDEC; };
    const bool immAB = d.alu == 0x1C || d.alu == 0x1D || d.alu == 0x1E || d.alu == 0x1F;   // fields are immediates
    d.hasIndirect = (!immAB && d.alu != 0x1B && ind(d.A)) || (!immAB && ind(d.B)) || ind(d.C) || ind(d.D) || ind(d.E) || ind(d.F) || ind(d.G);
    if (d.hasIndirect) d.pureNop = false;
}

uint8_t Esp2Core::read8(uint32_t off) {
    off &= 0x1F;
    if (off >= 0x1B && off <= 0x1D) {            // HOST_ESP_FACE_B2..B0 = SPR 0x1C8 (spec); checkpoint: PC
        ++readoutReads;
        return uint8_t((specFixes ? r_[spr::HOST_ESP_FACE] : pc_) >> (8 * (0x1D - off)));
    }
    if (off == 0x19) return specFixes ? uint8_t((control & 0x57) | (iozStatus_ ? 0x08 : 0) | (suspended_ ? 0x20 : 0)) : control;
    return port_[off];
}

void Esp2Core::write8(uint32_t off, uint8_t v) {
    off &= 0x1F;
    if (off == 0x0F) { hostInstrCommand(v); return; }
    if (off == 0x17) { hostRegCommand(v); return; }
    if (specFixes && off >= 0x1B && off <= 0x1D) {
        int sh = 8 * int(0x1D - off);
        r_[spr::HOST_ESP_FACE] = (r_[spr::HOST_ESP_FACE] & ~(0xFFu << sh)) | (uint32_t(v) << sh);
        return;
    }
    if (off == 0x19) {
        control = v; ++controlWrites; lastControl = v;
        if (specFixes) { iozStatus_ = v & 0x08; suspended_ = v & 0x20; }   // IOZ status and BIOZ are writable
        if (log) std::fprintf(log, "%.3f,%s,CTRL,,%02X%s\n", timeMs, source.c_str(), v, running() ? " (run)" : "");
        return;
    }
    port_[off] = v;
}

// Host transfers complete at once: the core is only ever observed between instruction cycles, which matches a
// chip that executes HOST (or sits in BIOZ suspension) whenever a transfer is pending.
void Esp2Core::hostRegCommand(uint8_t cmd) {
    uint32_t addr = ((uint32_t(port_[0x14]) << 8) | port_[0x15]) & 0x3FF;
    if (cmd == 0x80) {
        uint32_t val = (uint32_t(port_[0x11]) << 16) | (uint32_t(port_[0x12]) << 8) | port_[0x13];
        writeReg(addr, val);
        ++regWrites;
        if (log) std::fprintf(log, "%.3f,%s,REGW,%03X,%06X\n", timeMs, source.c_str(), addr, val);
    } else if (cmd == 0x81) {
        uint32_t val;
        switch (addr) {
        case spr::ZERO: val = 0; break;
        case spr::ONE: val = 1; break;
        case spr::MINUS1: val = 0x800000; break;
        case spr::HALF: val = 0x400000; break;
        case spr::MACRL: val = macrl_; break;
        default: val = r_[addr]; break;
        }
        if (addr == 0x0F7 && running()) ++mailboxReadsWhileRunning;
        if (addr == 0x0F7 && val) ++mailboxReadsNonZero;
        port_[0x11] = uint8_t(val >> 16); port_[0x12] = uint8_t(val >> 8); port_[0x13] = uint8_t(val);
        ++regReads;
        if (log) std::fprintf(log, "%.3f,%s,REGR,%03X,%06X\n", timeMs, source.c_str(), addr, val);
    } else if (log) {
        std::fprintf(log, "%.3f,%s,REGCMD?,%03X,%02X\n", timeMs, source.c_str(), addr, cmd);
    }
    port_[0x17] = cmd & 0x7F;
}

void Esp2Core::hostInstrCommand(uint8_t cmd) {
    uint32_t addr = (uint32_t(port_[0x0C]) << 8) | port_[0x0D];
    if (addr >= uint32_t(kInstr)) {
        ++badInstrAddr;
        if (log) std::fprintf(log, "%.3f,%s,INSCMD-BADADDR,%03X,%02X\n", timeMs, source.c_str(), addr, cmd);
        port_[0x0F] = cmd & 0x7F;
        return;
    }
    if (cmd == 0x80) {
        for (int i = 0; i < 12; ++i) imem_[addr][i] = port_[i];
        decode(int(addr));
        ++instrWrites;
        if (log) {
            char hex[25];
            for (int i = 0; i < 12; ++i) std::snprintf(hex + 2 * i, 3, "%02X", port_[i]);
            std::fprintf(log, "%.3f,%s,INSW,%03X,%s\n", timeMs, source.c_str(), addr, hex);
        }
    } else if (cmd == 0x81) {
        for (int i = 0; i < 12; ++i) port_[i] = imem_[addr][i];
        ++instrReads;
        if (log) std::fprintf(log, "%.3f,%s,INSR,%03X,\n", timeMs, source.c_str(), addr);
    } else if (log) {
        std::fprintf(log, "%.3f,%s,INSCMD?,%03X,%02X\n", timeMs, source.c_str(), addr, cmd);
    }
    port_[0x0F] = cmd & 0x7F;
}

// ------------------------------------------------------------------ execution

// Skip test (patent, "Conditional Execution"): OR over (CMR & CCR) for the nine flags, inverted by the NOT bit
// (CMR bit 9). True means the condition selects "skip". ALW = 0x000 never skips, NEV = 0x200 always skips.
bool Esp2Core::skip() const {
    uint32_t ccr = (ccr_ & 0x7F) | (iozStatus_ ? 0x100 : 0);
    bool t = (cmr_ & ccr & 0x1FF) != 0;
    return (cmr_ & 0x200) ? !t : t;
}

template <int OP>
uint32_t Esp2Core::aluT(const Decoded& d, uint32_t ua, uint32_t ub, bool setFlags, bool& write) {
    const int32_t a = sx24(ua), b = sx24(ub);
    uint32_t res = 0, fl = ccr_ & 0x7F, keep = 0x7F;     // keep: flags left alone
    bool sat = false;
    auto arith = [&](int64_t exact, uint32_t carry, bool saturate) {   // sets N C V LT Z from an exact result
        bool v = exact > 0x7FFFFF || exact < -0x800000;
        if (v && saturate) { double ov = double(exact > 0 ? exact - 0x7FFFFF : -0x800000 - exact); double& mo = satOver[d.C]; if (ov > mo) mo = ov; }
        uint32_t raw = uint32_t(exact) & 0xFFFFFF;
        bool nprime = raw & 0x800000;
        if (v && saturate) { res = exact > 0 ? 0x7FFFFF : 0x800000; sat = true; } else res = raw;
        fl = (carry ? F_C : 0) | (v ? F_V : 0) | ((nprime ^ v) ? F_LT : 0) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z);
        keep = F_NA | F_NB;
    };
    auto signsAB = [&]() { fl = (fl & ~(F_NA | F_NB)) | (a < 0 ? F_NA : 0) | (b < 0 ? F_NB : 0); };
    const uint32_t cin = (ccr_ & F_C) ? 1 : 0;
    write = true;
    switch (OP) {
    case 0x00: arith(int64_t(a) + b, ((ua + ub) >> 24) & 1, true); signsAB(); break;                    // ADD
    case 0x01: arith(int64_t(a) + b, ((ua + ub) >> 24) & 1, false); signsAB(); break;                   // ADDV
    case 0x02: arith(int64_t(a) + b + cin, ((ua + ub + cin) >> 24) & 1, true); signsAB(); break;        // ADDC
    case 0x03: arith(int64_t(b) - a, ub < ua, true); signsAB(); break;                                  // SUB
    case 0x04: arith(int64_t(b) - a, ub < ua, false); signsAB(); break;                                 // SUBV
    case 0x05: arith(int64_t(b) - a - cin, ub < ua + cin, true); signsAB(); break;                      // SUBB
    case 0x1A: arith(int64_t(a) - b, ua < ub, true); signsAB(); break;                                  // SUBREV
    case 0x06: case 0x07: {                                                                              // MAX, MIN
        arith(int64_t(a) - b, ua < ub, false);
        res = (OP == 0x06) == (a >= b) ? ua : ub;
        fl = (fl & ~F_Z) | (a == b ? F_Z : 0); signsAB(); break;
    }
    case 0x08: case 0x09: case 0x0A:                                                                     // AND OR XOR
        res = OP == 0x08 ? (ua & ub) : OP == 0x09 ? (ua | ub) : (ua ^ ub);
        fl = (fl & (F_C | F_V | F_LT)) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z); signsAB(); break;
    case 0x0B: case 0x18: case 0x19: case 0x1B: case 0x1E: res = ub; setFlags = false; break;          // MOV HOST BIOZ MOVcc RScc
    case 0x0C:                                                                                           // RECT
        if (b < 0) arith(int64_t(a) - b, ua < ub, true);
        else { res = ub; fl = (fl & F_C) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z); }
        signsAB(); break;
    case 0x0D: {                                                                                         // AVG
        int64_t s = int64_t(a) + b; res = uint32_t(s >> 1) & 0xFFFFFF;
        fl = (fl & F_C) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z); signsAB(); break;
    }
    case 0x0E: {                                                                                         // AMDF
        arith(int64_t(b) - a, ub < ua, true);
        bool neg = res & 0x800000;
        if (neg) res ^= 0xFFFFFF;
        fl = (fl & ~(F_N | F_Z)) | (neg ? F_N : 0) | (res ? 0 : F_Z); signsAB(); break;
    }
    case 0x0F: case 0x10: {                                                                              // AS, LS
        int n = std::clamp(a, -8, 8); bool arithShift = OP == 0x0F; uint32_t c;
        if (n > 0) {
            c = (ub >> (24 - n)) & 1;
            res = (ub << n) & 0xFFFFFF;
            if (arithShift && (sx24(res) >> n) != b) { res = b < 0 ? 0x800000 : 0x7FFFFF; sat = true; fl |= F_V; }
            else fl &= ~F_V;
        } else if (n < 0) {
            c = (ub >> (-n - 1)) & 1;
            res = arithShift ? uint32_t(b >> -n) & 0xFFFFFF : (ub >> -n);
            fl &= ~F_V;
        } else { c = arithShift ? (b < 0) : 0; res = ub; fl &= ~F_V; }
        fl = (fl & ~(F_C | F_N | F_Z)) | (c ? F_C : 0) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z);
        signsAB(); break;
    }
    case 0x11: case 0x12: case 0x13: case 0x14: {                                                        // ASDH ASDL LSDH LSDL
        int n = std::clamp(sx24(aluShift_), -8, 8);
        bool arithShift = OP <= 0x12, high = OP == 0x11 || OP == 0x13;
        int64_t x = (int64_t(arithShift ? b : int32_t(ub)) * (int64_t(1) << 24)) | ua;   // B high, A low
        if (!arithShift) x &= (int64_t(1) << 48) - 1;
        int64_t y = n >= 0 ? int64_t(uint64_t(x) << n) : (arithShift ? x >> -n : int64_t(uint64_t(x) >> -n));
        res = uint32_t(high ? (y >> 24) : y) & 0xFFFFFF;
        if (arithShift && high && n > 0 && ((y >> 47) != (x >> 47) || (y >> 47) != (y >> 48))) { res = b < 0 ? 0x800000 : 0x7FFFFF; sat = true; }
        fl = (fl & ~(F_N | F_Z)) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z); signsAB(); break;
    }
    case 0x15: { uint32_t x = 0; for (int i = 0; i < 24; ++i) if (ub & (1u << i)) x |= 1u << (23 - i); res = x;  // BREV
                 fl = (fl & ~(F_N | F_Z)) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z); signsAB(); break; }
    case 0x16: { uint32_t x = 0; for (int k = 0; k < 12; ++k) x |= ((ub >> (2 * k)) & 3) << (2 * (11 - k)); res = x;  // DREV
                 fl = (fl & ~(F_N | F_Z)) | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z); signsAB(); break; }
    case 0x17: {                                                                                         // LIM
        bool up = !(ccr_ & F_NA);
        uint32_t na = ccr_ & (F_NA | F_NB);
        arith(int64_t(a) - b, ua < ub, false);
        res = up ? (a <= b ? ua : ub) : (a >= b ? ua : ub);
        fl = (fl & ~(F_NA | F_NB | F_Z)) | na | (a == b ? F_Z : 0); break;
    }
    default: write = false; setFlags = false; break;                                                     // Jcc JScc REPT
    }
    if (sat) {
        ++aluSat; ++satDest[d.C];
        if (d.C == spr::SER_DAC_L || d.C == spr::SER_DAC_R) ++satToDac;
    }
    if (setFlags) ccr_ = (fl & keep) | (fl & ~keep & 0x7F);
    return res;
}


// One routine per opcode (the switch above folds to one case); alu() and the specialised lines dispatch through it.
namespace {
using AluFn = uint32_t (*)(Esp2Core&, const Esp2Core::Decoded&, uint32_t, uint32_t, bool, bool&);
}
struct Esp2Core::Binder {
    template <int... I> static constexpr std::array<AluFn, 32> aluTable(std::integer_sequence<int, I...>) { return {&Esp2Core::aluS<I>...}; }
    static const std::array<AluFn, 32> kAlu;
    using RunFn = void (*)(Esp2Core&, const Decoded&);
    template <int K> static constexpr RunFn runAt() { return &Esp2Core::runT<K / 24, (K / 12) % 2, (K / 4) % 3, K % 4>; }
    template <int... K> static constexpr std::array<RunFn, 48> runTable(std::integer_sequence<int, K...>) { return {runAt<K>()...}; }
    static const std::array<RunFn, 48> kRun;
    // ALU = 2 with the opcode compiled in, for the common shapes (no skip bits, MAC NOP or plain MAC): [AG][MAC][op]
    template <int K> static constexpr RunFn runOpAt() { return &Esp2Core::runT<0, K / 64, (K / 32) % 2, 2, K % 32>; }
    template <int... K> static constexpr std::array<RunFn, 128> runOpTable(std::integer_sequence<int, K...>) { return {runOpAt<K>()...}; }
    static const std::array<RunFn, 128> kRunOp;
    static const Decoded kOutside;               // PC beyond the 300 lines
};

uint32_t Esp2Core::alu(const Decoded& d, uint32_t ua, uint32_t ub, bool setFlags, bool& write) {
    return Binder::kAlu[d.alu & 31](*this, d, ua, ub, setFlags, write);
}

void Esp2Core::stepRef() {
    static const Decoded kNop = {0x3FF, spr::REF, spr::REF, spr::MACRL, spr::ONE, spr::ZERO, 0x200, 0x0B, 2, 6, 6, 0, 0, false, false, false};
    const Decoded& d0 = pc_ < uint32_t(kInstr) ? dec_[pc_] : kNop;
    Decoded dInd; uint8_t incdec[7] = {0, 0, 0, 0, 0, 0, 0};
    const bool indirect = specFixes && d0.hasIndirect;
    if (indirect) dInd = resolveIndirect(d0, incdec);
    const Decoded& d = indirect ? dInd : d0;
    const bool ccClass = d.alu == 0x1B || d.alu == 0x1C || d.alu == 0x1D || d.alu == 0x1E;
    if (ccClass) cmr_ = d.A & 0x3FF;                     // preload is unconditional and effective on this line
    const bool sk = skip();
    const bool aluEx = !(d.aluSkip && sk), macEx = !(d.macSkip && sk), agEx = !(d.agSkip && sk);

    // 1. external memory access requested by the previous instruction
    if (memPend_.valid) {
        if (memPend_.write) memWrite(memPend_.addr, r_[spr::DOL0 - memPend_.latch]);
        else { dilPend_ = true; dilPendLatch_ = memPend_.latch; dilPendVal_ = memRead(memPend_.addr); }
        memPend_.valid = false;
    }
    // 1b. MAC operand fetch (sees MAC(n-1) and ALU(n-2) results)
    const bool macReserved = (d.mac >= 0x0C && d.mac < 0x14) || d.mac >= 0x1C;
    uint32_t dv = 0, ev = 0;
    if (!macReserved) { dv = readReg(d.D); ev = readReg(d.E); }
    const int64_t seedP = macp_, seedM = macLatch_;      // seeds are fetched with the operands (MACP: ALU writes +2 lines)
    // 1c. AGEN (same timing as the MAC fetch)
    if (agEx && d.ag != 6) {
        const uint32_t endA = spr::REGION_END0 - 3 * d.rgn, sizeA = endA + 1, baseA = endA + 2;
        int64_t addr = int64_t(r_[d.G]) + r_[baseA] + ((d.ag == 4 || d.ag == 5) ? 1 : 0);
        if (addr - int64_t(r_[endA]) > 0) addr -= int64_t(r_[sizeA]) + 1;
        uint32_t ea = uint32_t(addr) & 0xFFFFFF;
        if (d.ag != 7 && (int64_t(ea) < int64_t(r_[endA]) - int64_t(r_[sizeA]) || ea > r_[endA])) ++regionViolations;
        if (watch > 0 && d.ag != 7 && !(ea - kRamBase < kRamWords) && (ea & ~0x1Fu) != kVoicePort) {
            --watch; std::fprintf(stderr, "AGEN pc %03X op %d R%d G=%03X aor %06X base %06X sizem1 %06X end %06X -> %06X\n", pc_, d.ag, d.rgn, d.G, r_[d.G], r_[baseA], r_[sizeA], r_[endA], ea);
        }
        if (d.ag == 2 || d.ag == 3 || d.ag == 7) r_[baseA] = ea;
        if (d.ag != 7) { memPend_.valid = true; memPend_.write = d.ag & 1; memPend_.addr = ea; memPend_.latch = d.dl; }
    }
    // 2. ALU(n-1) result reaches its destination
    if (aluPend_) { writer_ = 'A'; writeReg(aluPendAddr_, aluPendVal_); aluPend_ = false; }

    // 3. ALU(n)
    uint32_t newpc = npc_, newnpc = npc_ + 1;
    bool biozNow = false;
    if (aluEx) {
        const bool cond = d.aluSkip;                     // conditionally executed ALU ops never set the CCR
        switch (d.alu) {
        case 0x1C: newnpc = d.B; break;                                                          // Jcc
        case 0x1D: pcStack_[sp_ & 3] = newnpc; sp_ = (sp_ + 1) & 3; newnpc = d.B; break;         // JScc (push the return)
        case 0x1E: sp_ = (sp_ + 3) & 3; newnpc = pcStack_[sp_]; break;                           // RScc
        case 0x1F: reptEnd_ = d.A; reptSt_ = npc_; reptCnt_ = d.B; break;                        // REPT (first form)
        default: break;
        }
        uint32_t ua = 0, ub = 0;
        bool isJump = d.alu == 0x1C || d.alu == 0x1D || d.alu == 0x1F;
        if (!isJump) { if (d.alu != 0x0B && d.alu != 0x1B && d.alu != 0x1E && d.alu != 0x18 && d.alu != 0x19) ua = readReg(d.A); ub = readReg(d.B); }
        bool write = false;
        uint32_t res = alu(d, ua, ub, !cond, write);
        if (write && d.C != spr::ZERO) { aluPend_ = true; aluPendAddr_ = d.C; aluPendVal_ = res; }
        if (d.alu == 0x19) {                                                                     // BIOZ
            if (iozStatus_) { iozStatus_ = false; ++biozPasses; } else biozNow = true;
        }
    }
    // 4. MAC(n)
    if (macEx) {
        if (macReserved) { ++reservedMacOps; }
        else {
            const int sh = int(d.sh) - 7;                // + = left
            const bool sub = d.mac & 1, seedShift = d.mac >= 0x14;
            const int group = seedShift ? (d.mac < 0x18 ? 1 : 2) : (d.mac >> 2);   // 0 MACZERO, 1 MACP, 2 MAC
            const bool latchWrite = !(d.mac & 2);
            int64_t prod = int64_t(sx24(dv)) * sx24(ev) * 2;
            int64_t seed = group == 0 ? 0 : group == 1 ? seedP : seedM;
            if (seedShift) seed = wrap52(shiftv(seed, sh));
            int64_t acc = wrap52(sub ? seed - prod : seed + prod);
            int64_t out = seedShift ? acc : shiftv(acc, sh);
            if (out > kMax48 || out < kMin48) {
                if (watch > 0) { --watch; std::fprintf(stderr, "MACSAT pc %03X op %02X sh %d D=%03X(%06X) E=%03X(%06X) seed %llx acc %llx F=%03X\n", pc_, d.mac, sh, d.D, dv, d.E, ev, (long long)seed, (long long)acc, d.F); }
                { double ov = double(out < 0 ? kMin48 - out : out - kMax48) / 16777216.0; double& mo = satOver[d.F | 0x1000]; if (ov > mo) mo = ov; }
                out = acc < 0 ? kMin48 : kMax48; ++macSat; ++satDest[d.F | 0x1000];
                if (d.F == spr::SER_DAC_L || d.F == spr::SER_DAC_R) ++satToDac;
            }
            macrl_ = uint32_t(out) & 0xFFFFFF; r_[spr::MACRL] = macrl_;
            if (latchWrite) macLatch_ = acc;
            writer_ = 'M'; writeReg(d.F, uint32_t(out >> 24));
        }
    }
    // 5. data from the previous instruction's read reaches its DIL
    if (dilPend_) { r_[spr::DIL0 - dilPendLatch_] = dilPendVal_; dilPend_ = false; }
    if (indirect) applyIncDec(incdec, aluEx, macEx, agEx);
    writer_ = 'H';

    // 6. sequencing (1-cycle branch latency; REPT loop; BIOZ suspension after the next queued line)
    if (pc_ == reptEnd_ && reptCnt_) { --reptCnt_; newpc = reptSt_; newnpc = reptSt_ + 1; }
    pc_ = newpc & 0x3FF; npc_ = newnpc & 0x3FF;
    if (biozArmed_) {
        biozArmed_ = false;
        if (iozStatus_) iozStatus_ = false; else { suspended_ = true; ++biozSuspends; }
    }
    if (biozNow) biozArmed_ = true;
}


// Translated execution: same evaluation order and results as stepRef(), with the per-instruction work decided at
// translation time (pure-NOP lines, MAC NOPs, plain operands, skip logic only where a skip bit or cc op exists).
__attribute__((always_inline)) inline void Esp2Core::step() {
    static const Decoded kNop = []() { Decoded n{0x3FF, spr::REF, spr::REF, spr::MACRL, spr::ONE, spr::ZERO, 0x200, 0x0B, 2, 6, 6, 0, 0, false, false, false};
                                       return n; }();
    const Decoded& d0 = pc_ < uint32_t(kInstr) ? dec_[pc_] : kNop;
    Decoded dInd; uint8_t incdec[7] = {0, 0, 0, 0, 0, 0, 0};
    const bool indirect = specFixes && d0.hasIndirect;
    if (indirect) dInd = resolveIndirect(d0, incdec);
    const Decoded& d = indirect ? dInd : d0;
    if (d.pureNop && !memPend_.valid && !aluPend_ && !dilPend_ && !biozArmed_ && pc_ != reptEnd_) {
        refpt_ = (refpt_ + 1) & 511;                     // the MOV REF>REF read advances the refresh pointer
        pc_ = npc_; npc_ = (npc_ + 1) & 0x3FF;
        return;
    }
    if (pc_ >= uint32_t(kInstr) || !d.pureNop) { /* full path below */ }
    if (d.ccClass) cmr_ = d.A & 0x3FF;
    bool aluEx = true, macEx = true, agEx = true;
    if (d.anySkip) { const bool sk = skip(); aluEx = !(d.aluSkip && sk); macEx = !(d.macSkip && sk); agEx = !(d.agSkip && sk); }

    if (memPend_.valid) {
        const uint32_t ra = memPend_.addr - kRamBase;
        if (ra < kRamWords) {                  // delay RAM, inline (statistics as memRead/memWrite)
            if (memPend_.addr < ramLow) ramLow = memPend_.addr;
            if (memPend_.addr > ramHigh) ramHigh = memPend_.addr;
            if (memPend_.write) { ++memWrites; ram_[ra] = dolTrunc(r_[spr::DOL0 - memPend_.latch]); wEpoch[ra] = epoch; }
            else {
                ++memReads; dilPend_ = true; dilPendLatch_ = memPend_.latch; dilPendVal_ = ram_[ra];
                if (wEpoch[ra] < epoch && memPend_.addr >= staleWinLo && memPend_.addr <= staleWinHi) { ++staleReads; staleLow = std::min(staleLow, memPend_.addr); staleHigh = std::max(staleHigh, memPend_.addr); }
            }
        } else if (memPend_.write) memWrite(memPend_.addr, r_[spr::DOL0 - memPend_.latch]);
        else { dilPend_ = true; dilPendLatch_ = memPend_.latch; dilPendVal_ = memRead(memPend_.addr); }
        memPend_.valid = false;
    }
    uint32_t dv = 0, ev = 0;
    const bool macWork = !d.macReserved && !d.macNop;
    if (macWork) { dv = d.dPlain ? r_[d.D] : readReg(d.D); ev = d.ePlain ? r_[d.E] : readReg(d.E); }
    const int64_t seedP = macp_, seedM = macLatch_;
    if (agEx && d.ag != 6) {
        const uint32_t endA = spr::REGION_END0 - 3 * d.rgn, sizeA = endA + 1, baseA = endA + 2;
        int64_t addr = int64_t(r_[d.G]) + r_[baseA] + ((d.ag == 4 || d.ag == 5) ? 1 : 0);
        if (addr - int64_t(r_[endA]) > 0) addr -= int64_t(r_[sizeA]) + 1;
        uint32_t ea = uint32_t(addr) & 0xFFFFFF;
        if (d.ag != 7 && (int64_t(ea) < int64_t(r_[endA]) - int64_t(r_[sizeA]) || ea > r_[endA])) ++regionViolations;
        if (watch > 0 && d.ag != 7 && !(ea - kRamBase < kRamWords) && (ea & ~0x1Fu) != kVoicePort) {
            --watch; std::fprintf(stderr, "AGEN pc %03X op %d R%d G=%03X aor %06X -> %06X\n", pc_, d.ag, d.rgn, d.G, r_[d.G], ea);
        }
        if (d.ag == 2 || d.ag == 3 || d.ag == 7) r_[baseA] = ea;
        if (d.ag != 7) { memPend_.valid = true; memPend_.write = d.ag & 1; memPend_.addr = ea; memPend_.latch = d.dl; }
    }
    if (aluPend_) { if (aluPendPlain_) r_[aluPendAddr_] = aluPendVal_; else { writer_ = 'A'; writeReg(aluPendAddr_, aluPendVal_); } aluPend_ = false; }

    uint32_t newpc = npc_, newnpc = npc_ + 1;
    bool biozNow = false;
    if (aluEx) {
        switch (d.alu) {
        case 0x1C: newnpc = d.B; break;
        case 0x1D: pcStack_[sp_ & 3] = newnpc; sp_ = (sp_ + 1) & 3; newnpc = d.B; break;
        case 0x1E: sp_ = (sp_ + 3) & 3; newnpc = pcStack_[sp_]; break;
        case 0x1F: reptEnd_ = d.A; reptSt_ = npc_; reptCnt_ = d.B; break;
        default: break;
        }
        if (d.aluKind == 1) { aluPend_ = true; aluPendPlain_ = d.cPlain; aluPendAddr_ = d.C; aluPendVal_ = r_[d.B]; }
        else if (d.aluKind == 0) {
            uint32_t ua = 0, ub = 0;
            if (d.aluReadsA) ua = d.aPlain ? r_[d.A] : readReg(d.A);
            if (d.aluReadsB) ub = d.bPlain ? r_[d.B] : readReg(d.B);
            bool write = false;
            uint32_t res;
            if (d.alu == 0x0B || d.alu == 0x18 || d.alu == 0x19) { res = ub; write = true; }   // MOV HOST BIOZ: no flags
            else res = alu(d, ua, ub, !d.aluSkip, write);
            if (write && d.C != spr::ZERO) { aluPend_ = true; aluPendPlain_ = d.cPlain; aluPendAddr_ = d.C; aluPendVal_ = res; }
        }
        if (d.alu == 0x19) { if (iozStatus_) { iozStatus_ = false; ++biozPasses; } else biozNow = true; }
    }
    if (macEx && !d.macNop) {
        if (d.macReserved) { ++reservedMacOps; }
        else {
            const int sh = d.macShift;
            int64_t prod = int64_t(sx24(dv)) * sx24(ev) * 2;
            int64_t seed = d.macSeed == 0 ? 0 : d.macSeed == 1 ? seedP : seedM;
            if (d.macSeedShift) seed = wrap52(shiftv(seed, sh));
            int64_t acc = wrap52(d.macSub ? seed - prod : seed + prod);
            int64_t out = d.macSeedShift ? acc : shiftv(acc, sh);
            if (out > kMax48 || out < kMin48) {
                { double ov = double(out < 0 ? kMin48 - out : out - kMax48) / 16777216.0; double& mo = satOver[d.F | 0x1000]; if (ov > mo) mo = ov; }
                out = acc < 0 ? kMin48 : kMax48; ++macSat; ++satDest[d.F | 0x1000];
                if (d.F == spr::SER_DAC_L || d.F == spr::SER_DAC_R) ++satToDac;
            }
            macrl_ = uint32_t(out) & 0xFFFFFF; r_[spr::MACRL] = macrl_;
            if (d.macLatchWrite) macLatch_ = acc;
            const uint32_t fv = uint32_t(out >> 24) & 0xFFFFFF;
            if (d.fKind == 1) r_[d.F] = fv; else if (d.fKind == 2) { writer_ = 'M'; writeReg(d.F, fv); }
        }
    }
    if (dilPend_) { r_[spr::DIL0 - dilPendLatch_] = dilPendVal_; dilPend_ = false; }
    if (indirect) applyIncDec(incdec, aluEx, macEx, agEx);
    writer_ = 'H';
    if (pc_ == reptEnd_ && reptCnt_) { --reptCnt_; newpc = reptSt_; newnpc = reptSt_ + 1; }
    pc_ = newpc & 0x3FF; npc_ = newnpc & 0x3FF;
    if (biozArmed_) {
        biozArmed_ = false;
        if (iozStatus_) iozStatus_ = false; else { suspended_ = true; ++biozSuspends; }
    }
    if (biozNow) biozArmed_ = true;
}

const Esp2Core::Decoded Esp2Core::Binder::kOutside = [] { Decoded n{}; n.run = &Esp2Core::runStep; return n; }();

// Threaded dispatch: each line's routine ends by running the next line's (a tail call), so every routine has its own
// indirect jump and the branch predictor learns the program's line sequence. runTo bounds a chain to 256 lines.
__attribute__((always_inline)) inline void Esp2Core::next() {
    ++cycle_;
    if (cycle_ < stop_ && !suspended_) {
        const Decoded& n = pc_ < uint32_t(kInstr) ? dec_[pc_] : Binder::kOutside;
        n.run(*this, n);
    }
}

// The common opcodes written out directly for the specialised lines (aluT is the definition; the lockstep test checks
// these against it): ADD ADDV SUBV SUB SUBREV set every flag from the exact result and the operand signs, AVG keeps
// C, AND OR XOR keep C V LT. Saturation statistics go through a cold call.
constexpr bool aluFastOp(int op) { return op == 0x00 || op == 0x01 || op == 0x03 || op == 0x04 || op == 0x1A || (op >= 0x08 && op <= 0x0A) || op == 0x0D; }

void Esp2Core::aluSatCold(const Decoded& d, int64_t exact) {
    const double ov = double(exact > 0 ? exact - 0x7FFFFF : -0x800000 - exact);
    double& mo = satOver[d.C]; if (ov > mo) mo = ov;
    ++aluSat; ++satDest[d.C];
    if (d.C == spr::SER_DAC_L || d.C == spr::SER_DAC_R) ++satToDac;
}

template <int OP>
__attribute__((always_inline)) inline uint32_t Esp2Core::aluFast(const Decoded& d, uint32_t ua, uint32_t ub, bool setFlags) {
    const int32_t a = sx24(ua), b = sx24(ub);
    const uint32_t signs = (a < 0 ? F_NA : 0) | (b < 0 ? F_NB : 0);
    uint32_t res, fl;
    if constexpr (OP <= 0x04 || OP == 0x1A) {
        const int64_t exact = OP <= 0x01 ? int64_t(a) + b : OP == 0x1A ? int64_t(a) - b : int64_t(b) - a;
        const uint32_t carry = OP <= 0x01 ? ((ua + ub) >> 24) & 1 : OP == 0x1A ? uint32_t(ua < ub) : uint32_t(ub < ua);
        const bool v = exact > 0x7FFFFF || exact < -0x800000;
        res = uint32_t(exact) & 0xFFFFFF;
        const bool nprime = res & 0x800000;
        if (__builtin_expect((OP == 0x00 || OP == 0x03 || OP == 0x1A) && v, 0)) { aluSatCold(d, exact); res = exact > 0 ? 0x7FFFFF : 0x800000; }
        fl = (carry ? F_C : 0) | (v ? F_V : 0) | ((nprime ^ v) ? F_LT : 0);
    } else if constexpr (OP == 0x0D) {
        res = uint32_t((int64_t(a) + b) >> 1) & 0xFFFFFF;
        fl = ccr_ & F_C;
    } else {
        res = OP == 0x08 ? (ua & ub) : OP == 0x09 ? (ua | ub) : (ua ^ ub);
        fl = ccr_ & (F_C | F_V | F_LT);
    }
    if (setFlags) ccr_ = fl | ((res & 0x800000) ? F_N : 0) | (res ? 0 : F_Z) | signs;
    return res;
}

// Specialised lines: step() with the per-line decisions made once, when the line is written (bind()). Template
// parameters: SK = the line has a skip bit; AG = AGEN active (ag != 6); MAC 0 = MAC NOP, 1 = plain D/E operands and a
// plain or ZERO destination, 2 = any other MAC; ALU 0 = MOV to ZERO (no effect), 1 = MOV with a plain source,
// 2 = an arithmetic/logic opcode with plain operands (one routine per opcode), 3 = any other ALU line handled here.
// Lines with indirection, condition-code ops, jumps, REPT, HOST or BIOZ, and pure NOPs that find work in flight, run
// step() itself. Every statement below is step()'s, in the same order; only the decisions known at bind time are gone.
template <int SK, int AG, int MAC, int ALU, int OP>
__attribute__((always_inline)) inline void Esp2Core::stepT(const Decoded& d) {
    bool aluEx = true, macEx = true, agEx = true;
    if (SK) { const bool sk = skip(); aluEx = !(d.aluSkip && sk); macEx = !(d.macSkip && sk); agEx = !(d.agSkip && sk); }

    if (memPend_.valid) {
        const uint32_t ra = memPend_.addr - kRamBase;
        if (ra < kRamWords) {
            if (memPend_.addr < ramLow) ramLow = memPend_.addr;
            if (memPend_.addr > ramHigh) ramHigh = memPend_.addr;
            if (memPend_.write) { ++memWrites; ram_[ra] = dolTrunc(r_[spr::DOL0 - memPend_.latch]); wEpoch[ra] = epoch; }
            else {
                ++memReads; dilPend_ = true; dilPendLatch_ = memPend_.latch; dilPendVal_ = ram_[ra];
                if (wEpoch[ra] < epoch && memPend_.addr >= staleWinLo && memPend_.addr <= staleWinHi) { ++staleReads; staleLow = std::min(staleLow, memPend_.addr); staleHigh = std::max(staleHigh, memPend_.addr); }
            }
        } else if (memPend_.write) memWrite(memPend_.addr, r_[spr::DOL0 - memPend_.latch]);
        else { dilPend_ = true; dilPendLatch_ = memPend_.latch; dilPendVal_ = memRead(memPend_.addr); }
        memPend_.valid = false;
    }
    uint32_t dv = 0, ev = 0;
    if (MAC == 1) { dv = r_[d.D]; ev = r_[d.E]; }
    else if (MAC == 2 && !d.macReserved) { dv = d.dPlain ? r_[d.D] : readReg(d.D); ev = d.ePlain ? r_[d.E] : readReg(d.E); }
    const int64_t seedP = macp_, seedM = macLatch_;
    if (AG && agEx) {
        const uint32_t endA = d.agEnd, sizeA = endA + 1, baseA = endA + 2;
        const uint32_t end = r_[endA], size = r_[sizeA];
        int64_t addr = int64_t(r_[d.G]) + r_[baseA] + d.agPlus;
        if (addr - int64_t(end) > 0) addr -= int64_t(size) + 1;
        uint32_t ea = uint32_t(addr) & 0xFFFFFF;
        if (d.agMem && (int64_t(ea) < int64_t(end) - int64_t(size) || ea > end)) ++regionViolations;
        if (watch > 0 && d.agMem && !(ea - kRamBase < kRamWords) && (ea & ~0x1Fu) != kVoicePort) {
            --watch; std::fprintf(stderr, "AGEN pc %03X op %d R%d G=%03X aor %06X -> %06X\n", pc_, d.ag, d.rgn, d.G, r_[d.G], ea);
        }
        if (d.agBase) r_[baseA] = ea;
        if (d.agMem) { memPend_.valid = true; memPend_.write = d.ag & 1; memPend_.addr = ea; memPend_.latch = d.dl; }
    }
    if (aluPend_) { if (aluPendPlain_) r_[aluPendAddr_] = aluPendVal_; else { writer_ = 'A'; writeReg(aluPendAddr_, aluPendVal_); } aluPend_ = false; }

    uint32_t newpc = npc_, newnpc = npc_ + 1;
    if (aluEx) {
        if (ALU == 1) { aluPend_ = true; aluPendPlain_ = d.cPlain; aluPendAddr_ = d.C; aluPendVal_ = r_[d.B]; }
        else if (ALU == 2) {
            bool write = false;
            uint32_t res;
            if constexpr (OP >= 0 && aluFastOp(OP)) { res = aluFast<OP>(d, r_[d.A], r_[d.B], !d.aluSkip); write = true; }
            else if constexpr (OP >= 0) res = aluT<OP>(d, r_[d.A], r_[d.B], !d.aluSkip, write);
            else res = d.aluFn(*this, d, r_[d.A], r_[d.B], !d.aluSkip, write);
            if (write && d.C != spr::ZERO) { aluPend_ = true; aluPendPlain_ = d.cPlain; aluPendAddr_ = d.C; aluPendVal_ = res; }
        } else if (ALU == 3) {
            if (d.aluKind == 1) { aluPend_ = true; aluPendPlain_ = d.cPlain; aluPendAddr_ = d.C; aluPendVal_ = r_[d.B]; }
            else if (d.aluKind == 0) {
                uint32_t ua = 0, ub = 0;
                if (d.aluReadsA) ua = d.aPlain ? r_[d.A] : readReg(d.A);
                if (d.aluReadsB) ub = d.bPlain ? r_[d.B] : readReg(d.B);
                bool write = false;
                uint32_t res;
                if (d.alu == 0x0B) { res = ub; write = true; }
                else res = alu(d, ua, ub, !d.aluSkip, write);
                if (write && d.C != spr::ZERO) { aluPend_ = true; aluPendPlain_ = d.cPlain; aluPendAddr_ = d.C; aluPendVal_ = res; }
            }
        }
    }
    if (MAC != 0 && macEx) {
        if (MAC == 2 && d.macReserved) { ++reservedMacOps; }
        else {
            // step()'s MAC without its per-line branches: a seed is always a 52-bit value (MAC latch: wrapped; MACP:
            // 48 bits), so wrap52 of an unshifted seed is the seed; seed - prod = seed + prod x -1.
            const int64_t prod = int64_t(sx24(dv)) * sx24(ev) * d.macMul;
            const int64_t seeds[3] = {0, seedP, seedM};
            const int64_t seed = wrap52(int64_t(uint64_t(seeds[d.macSeed]) << d.seedL) >> d.seedR);
            const int64_t acc = wrap52(seed + prod);
            int64_t out = int64_t(uint64_t(acc) << d.outL) >> d.outR;
            if (out > kMax48 || out < kMin48) {
                { double ov = double(out < 0 ? kMin48 - out : out - kMax48) / 16777216.0; double& mo = satOver[d.F | 0x1000]; if (ov > mo) mo = ov; }
                out = acc < 0 ? kMin48 : kMax48; ++macSat; ++satDest[d.F | 0x1000];
                if (d.F == spr::SER_DAC_L || d.F == spr::SER_DAC_R) ++satToDac;
            }
            macrl_ = uint32_t(out) & 0xFFFFFF; r_[spr::MACRL] = macrl_;
            macLatch_ = d.macLatchWrite ? acc : macLatch_;
            const uint32_t fv = uint32_t(out >> 24) & 0xFFFFFF;
            if (d.fKind == 1) r_[d.F] = fv; else if (MAC == 2 && d.fKind == 2) { writer_ = 'M'; writeReg(d.F, fv); }
        }
    }
    if (dilPend_) { r_[spr::DIL0 - dilPendLatch_] = dilPendVal_; dilPend_ = false; }
    writer_ = 'H';
    if (pc_ == reptEnd_ && reptCnt_) { --reptCnt_; newpc = reptSt_; newnpc = reptSt_ + 1; }
    pc_ = newpc & 0x3FF; npc_ = newnpc & 0x3FF;
    if (biozArmed_) {
        biozArmed_ = false;
        if (iozStatus_) iozStatus_ = false; else { suspended_ = true; ++biozSuspends; }
    }
    next();
}

void Esp2Core::runStep(Esp2Core& c, const Decoded&) { c.step(); c.next(); }

void Esp2Core::runNop(Esp2Core& c, const Decoded&) {
    if (!c.memPend_.valid && !c.aluPend_ && !c.dilPend_ && !c.biozArmed_ && c.pc_ != c.reptEnd_) {
        c.refpt_ = (c.refpt_ + 1) & 511;
        c.pc_ = c.npc_; c.npc_ = (c.npc_ + 1) & 0x3FF;
    } else c.step();
    c.next();
}

const std::array<AluFn, 32> Esp2Core::Binder::kAlu = aluTable(std::make_integer_sequence<int, 32>{});
const std::array<Esp2Core::Binder::RunFn, 48> Esp2Core::Binder::kRun = runTable(std::make_integer_sequence<int, 48>{});
const std::array<Esp2Core::Binder::RunFn, 128> Esp2Core::Binder::kRunOp = runOpTable(std::make_integer_sequence<int, 128>{});

// Chooses the routine for a decoded line (instruction memory only; indirect lines resolved at run time use step()).
void Esp2Core::bind(Decoded& d) {
    d.aluFn = Binder::kAlu[d.alu & 31];
    d.macMul = int8_t(d.macSub ? -2 : 2);
    const int sh = d.macShift;
    const uint8_t l = uint8_t(sh > 0 ? sh : 0), r = uint8_t(sh < 0 ? -sh : 0);
    d.seedL = d.macSeedShift ? l : 0; d.seedR = d.macSeedShift ? r : 0;
    d.outL = d.macSeedShift ? 0 : l; d.outR = d.macSeedShift ? 0 : r;
    d.agEnd = uint16_t(spr::REGION_END0 - 3 * d.rgn);
    d.agPlus = (d.ag == 4 || d.ag == 5) ? 1 : 0;
    d.agBase = d.ag == 2 || d.ag == 3 || d.ag == 7;
    d.agMem = d.ag != 7;
    const bool control = d.ccClass || (d.alu >= 0x18 && d.alu != 0x1A);   // HOST BIOZ MOVcc Jcc JScc RScc REPT
    if (d.pureNop) { d.run = &Esp2Core::runNop; return; }
    if (d.hasIndirect || control) { d.run = &Esp2Core::runStep; return; }
    const int sk = d.anySkip ? 1 : 0, ag = d.ag != 6 ? 1 : 0;
    const int mac = d.macNop ? 0 : (!d.macReserved && d.dPlain && d.ePlain && d.fKind != 2) ? 1 : 2;
    int alu = 3;
    if (d.alu == 0x0B && d.aluKind == 2) alu = 0;
    else if (d.alu == 0x0B && d.aluKind == 1) alu = 1;
    else if (d.aluKind == 0 && d.alu != 0x0B && d.aPlain && d.bPlain) alu = 2;
    d.run = (alu == 2 && !sk && mac < 2) ? Binder::kRunOp[(ag * 2 + mac) * 32 + d.alu] : Binder::kRun[((sk * 2 + ag) * 3 + mac) * 4 + alu];
}

void Esp2Core::runTo(uint64_t target) {
    while (cycle_ < target) {
        if (!running()) { haltedCycles += target - cycle_; cycle_ = target; break; }
        if (suspended_) { suspendedCycles += target - cycle_; cycle_ = target; break; }
        if (fastPath) {
            const uint64_t c0 = cycle_;
            while (cycle_ < target && !suspended_) {
                stop_ = std::min(target, cycle_ + 256);
                const Decoded& d = pc_ < uint32_t(kInstr) ? dec_[pc_] : Binder::kOutside;
                d.run(*this, d);
            }
            executed += cycle_ - c0;
        } else { stepRef(); ++cycle_; ++executed; }
    }
}

void Esp2Core::sampleTick() {
    ++ticks;
    dacOut[0] = sx24(r_[spr::SER_DAC_L]); dacOut[1] = sx24(r_[spr::SER_DAC_R]);
    for (int i = 0; i < 2; ++i) if (dacOut[i] == 0x7FFFFF || dacOut[i] == -0x800000) { ++dacSatSamples; break; }
    for (int i = 0; i < 8; ++i) auxOut[i] = sx24(r_[0x3EB - i]);
    r_[spr::SER_ADC_L] = uint32_t(serialIn[0]) & 0xFFFFFF;
    r_[spr::SER_ADC_R] = uint32_t(serialIn[1]) & 0xFFFFFF;
    if (control & (specFixes ? 0x10 : 0x01)) {           // IOZ_EN: HOST_CNTL bit 4 (spec); checkpoint used bit 0
        if (suspended_) { suspended_ = false; iozStatus_ = false; }
        else { if (iozStatus_ && running()) ++overruns; iozStatus_ = true; }
    }
}

// ------------------------------------------------------------------ state (plugin projects)
#include <cstddef>
#include <cstring>
#include "state_io.h"

namespace {
// A field's bytes at its offset in a struct's in-memory layout.
template <class F> void place(uint8_t* layout, size_t offset, const F& field) { std::memcpy(layout + offset, &field, sizeof field); }
}

void Esp2Core::save(StateWriter& w) const {
    // The pending memory operation and the pointer histories are stored in their in-memory layout (as states always
    // held them), with the padding as zeros: copied whole, the padding, never set, put leftover memory into the state.
    static_assert(sizeof(MemOp) == 12 && sizeof(PtrWrite) == 16 && sizeof(ptrPend_) == 28 * sizeof(PtrWrite), "state layout");
    w.put(r_); w.put(imem_); w.vec(ram_); w.put(port_);
    w.put(cycle_); w.put(pc_); w.put(npc_); w.put(biozArmed_); w.put(suspended_); w.put(iozStatus_);
    w.put(ccr_); w.put(cmr_); w.put(refpt_); w.put(aluShift_); w.put(macLatch_); w.put(macp_); w.put(macrl_);
    w.put(pcStack_); w.put(sp_); w.put(reptSt_); w.put(reptEnd_); w.put(reptCnt_);
    w.put(aluPend_); w.put(aluPendPlain_); w.put(aluPendAddr_); w.put(aluPendVal_);
    uint8_t mem[sizeof(MemOp)] = {};
    place(mem, offsetof(MemOp, valid), memPend_.valid); place(mem, offsetof(MemOp, write), memPend_.write);
    place(mem, offsetof(MemOp, addr), memPend_.addr); place(mem, offsetof(MemOp, latch), memPend_.latch);
    w.raw(mem, sizeof mem);
    w.put(dilPend_); w.put(dilPendLatch_); w.put(dilPendVal_);
    for (const auto& history : ptrPend_)
        for (const PtrWrite& p : history) {
            uint8_t ptr[sizeof(PtrWrite)] = {};
            place(ptr, offsetof(PtrWrite, visible), p.visible); place(ptr, offsetof(PtrWrite, value), p.value);
            w.raw(ptr, sizeof ptr);
        }
    w.put(ptrN_); w.put(writer_);
    w.put(control); w.put(lastControl); w.put(instrPerSample);
    w.put(voicePort); w.put(serialIn); w.put(dacOut); w.put(auxOut);
    const uint64_t stats[] = {regWrites, regReads, instrWrites, instrReads, controlWrites, readoutReads, mailboxReadsWhileRunning,
                              mailboxReadsNonZero, badInstrAddr, executed, suspendedCycles, haltedCycles, biozPasses, biozSuspends,
                              overruns, macSat, aluSat, dacSatSamples, ticks, memReads, memWrites, memUnmapped, voicePortReads,
                              voicePortWrites, reservedMacOps, unknownSprReads, unknownSprWrites, satToDac, staleReads,
                              regionViolations, extWrites};
    w.put(stats);
}

void Esp2Core::load(StateReader& r) {
    r.get(r_); r.get(imem_); r.vecExact(ram_, kRamWords); r.get(port_);
    r.get(cycle_); r.get(pc_); r.get(npc_); r.get(biozArmed_); r.get(suspended_); r.get(iozStatus_);
    r.get(ccr_); r.get(cmr_); r.get(refpt_); r.get(aluShift_); r.get(macLatch_); r.get(macp_); r.get(macrl_);
    r.get(pcStack_); r.get(sp_); r.get(reptSt_); r.get(reptEnd_); r.get(reptCnt_);
    r.get(aluPend_); r.get(aluPendPlain_); r.get(aluPendAddr_); r.get(aluPendVal_);
    r.get(memPend_); r.get(dilPend_); r.get(dilPendLatch_); r.get(dilPendVal_);
    r.get(ptrPend_); r.get(ptrN_); r.get(writer_);
    r.get(control); r.get(lastControl); r.get(instrPerSample);
    r.get(voicePort); r.get(serialIn); r.get(dacOut); r.get(auxOut);
    uint64_t s[31] = {};
    r.get(s);
    uint64_t* dst[] = {&regWrites, &regReads, &instrWrites, &instrReads, &controlWrites, &readoutReads, &mailboxReadsWhileRunning,
                       &mailboxReadsNonZero, &badInstrAddr, &executed, &suspendedCycles, &haltedCycles, &biozPasses, &biozSuspends,
                       &overruns, &macSat, &aluSat, &dacSatSamples, &ticks, &memReads, &memWrites, &memUnmapped, &voicePortReads,
                       &voicePortWrites, &reservedMacOps, &unknownSprReads, &unknownSprWrites, &satToDac, &staleReads,
                       &regionViolations, &extWrites};
    for (int i = 0; i < 31; ++i) *dst[i] = s[i];
    for (int i = 0; i < kInstr; ++i) decode(i);      // the translation cache follows the instruction memory
}
