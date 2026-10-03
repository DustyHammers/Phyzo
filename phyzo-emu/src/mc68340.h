// Minimal MC68340 integrated-module models: timer 1, serial module (A/B), DMA channel 1.
// Register semantics follow the MC68340 user's manual as modelled in MAME's
// m68340 devices (BSD-3-Clause), used here as a behavioural reference only.
#pragma once
#include <cstdint>
#include <deque>
#include "ring_deque.h"
#include <string>
#include <vector>

class StateWriter;
class StateReader;

constexpr uint64_t kNever = ~uint64_t(0);

// Services the machine provides to the modules.
struct ModuleHost {
    virtual ~ModuleHost() = default;
    virtual uint64_t now() = 0;                 // current CPU cycle
    virtual void reschedule() = 0;              // device timing changed: end the CPU timeslice
    virtual void irqChanged() = 0;              // an interrupt line may have changed
    virtual uint8_t dmaRead8(uint32_t addr) = 0;
    virtual void dmaWrite8(uint32_t addr, uint8_t v) = 0;
    virtual void serialTx(int ch, uint8_t b, uint64_t when) = 0;   // byte left the shifter
    virtual void serialOutputPort(uint8_t op, uint64_t when) = 0;  // OP pins changed
};

class Timer {
public:
    explicit Timer(ModuleHost& h) : host(h) {}
    uint8_t read8(uint32_t off);
    void write8(uint32_t off, uint8_t v);
    void advance(uint64_t now);
    uint64_t nextEvent() const { return on ? nextZero : kNever; }
    int irqLevel() const;
    void save(StateWriter& w) const;
    void load(StateReader& r);
    uint8_t vector() const { return ir & 0xff; }

    uint16_t mcr = 0, ir = 0, cr = 0, sr = 0, prel1 = 0xffff, prel2 = 0xffff, com = 0;
    uint64_t timeouts = 0;          // number of TO events
private:
    uint16_t counter(uint64_t now) const;
    void crChanged();
    uint64_t divider() const;

    ModuleHost& host;
    bool on = false;
    uint64_t anchor = 0;     // cycle at which `loaded` was put into the counter
    uint16_t loaded = 0;
    uint64_t nextZero = kNever;
};

struct SerialChannel {
    uint8_t mr1 = 0, mr2 = 0, csr = 0;
    bool txEn = false, rxEn = false;
    // transmitter: holding register + shift register
    bool holdFull = false; uint8_t hold = 0;
    bool shiftBusy = false; uint8_t shiftByte = 0; uint64_t shiftDone = kNever;
    // receiver: 3-byte FIFO fed by a paced input queue
    RingDeque<uint8_t> fifo{16};
    RingDeque<std::pair<uint64_t, uint8_t>> pending{32768};   // room for a large SysEx without allocating
    uint64_t lastArrival = 0;
    uint8_t lastRead = 0;
    uint64_t byteCycles = 5120;   // set by the machine
    uint64_t txBytes = 0, rxBytes = 0, txWhileDisabled = 0, holdOverwrites = 0;
};

class Serial {
public:
    explicit Serial(ModuleHost& h) : host(h) {}
    uint8_t read8(uint32_t off, bool& handled);
    void write8(uint32_t off, uint8_t v, bool& handled);
    void advance(uint64_t now);
    uint64_t nextEvent() const;
    int irqLevel() const;
    uint8_t vector() const { return ivr; }
    uint8_t isr() const;
    void queueRx(int ch, const std::vector<uint8_t>& bytes, uint64_t earliest);
    void queueRx(int ch, const uint8_t* bytes, size_t n, uint64_t earliest);
    void save(StateWriter& w) const;
    void load(StateReader& r);
    bool rxIdle(int ch) const { return ch_[ch].pending.empty() && ch_[ch].fifo.empty(); }
    size_t rxBacklog(int ch) const { return ch_[ch].pending.size() + ch_[ch].fifo.size(); }   // bytes not yet read

    SerialChannel ch_[2];
    uint8_t ier = 0, ilr = 0, ivr = 0x0f, acr = 0, opcr = 0, op = 0;
    uint16_t mcr = 0;
    uint64_t isrBit3Reads = 0;
private:
    uint8_t status(int c) const;
    void command(int c, uint8_t v);
    void writeTx(int c, uint8_t v);
    uint64_t rxNext(int c) const;

    ModuleHost& host;
};

class Dma {
public:
    explicit Dma(ModuleHost& h) : host(h) {}
    uint8_t read8(uint32_t off);
    void write8(uint32_t off, uint8_t v);
    void save(StateWriter& w) const;
    void load(StateReader& r);

    uint16_t mcr = 0, intr = 0, ccr = 0;
    uint8_t csr = 0, fcr = 0;
    uint32_t sar = 0, dar = 0, btc = 0;
    uint64_t blocks = 0, bytes = 0;
private:
    void start();
    ModuleHost& host;
};

// Timer 2 is only touched by the hardware test; keep plain storage.
struct PlainRegs {
    uint8_t r[0x40] = {0};
};
