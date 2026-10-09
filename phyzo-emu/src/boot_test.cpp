// Headless boot test: boots the user-supplied OS image on the emulated machine,
// waits for the expected display text, exercises panel buttons and writes logs.
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "machine.h"
#include "os_image.h"
#include "os_profile.h"
#include "rom_id.h"

extern "C" {
#include "m68k.h"
}

namespace {

struct Options {
    std::string os, out = "boot_out", expect = "P 01", wave, roms;
    double bootMs = 8000, settleMs = 1000, idleMs = 5000;
    bool buttons = true, mailboxRule = true, notes = true, esp2Stub = false, esp2Checkpoint = false, noAnswerF4 = false;
    int answerF4 = -1, f2Reply = -1;
    double cpuHz = 16.0e6;
};

void usage() {
    std::puts("usage: phyzo_boot (--os <image> | --roms <folder>) [--out dir] [--expect \"P 01\"] [--boot-ms N] [--settle-ms N]\n"
              "               [--idle-ms N] [--no-buttons] [--no-notes] [--esp2-stub] [--no-mailbox-rule] [--answer-f4 V | --no-answer-f4] [--f2-reply XX]\n"
              "               [--cpu-hz HZ] [--wave native_wave_image.bin]");
}

std::string hexBytes(const std::vector<uint8_t>& b) {
    std::string s; char t[4];
    for (size_t i = 0; i < b.size(); ++i) { std::snprintf(t, sizeof t, "%s%02X", i ? " " : "", b[i]); s += t; }
    return s;
}

struct ButtonResult {
    std::string name, sent, before, after;
    std::vector<std::string> changes;
    bool changed = false;
};

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if (a == "--os") o.os = next();
        else if (a == "--wave") o.wave = next();
        else if (a == "--roms") o.roms = next();
        else if (a == "--out") o.out = next();
        else if (a == "--expect") o.expect = next();
        else if (a == "--boot-ms") o.bootMs = std::atof(next().c_str());
        else if (a == "--settle-ms") o.settleMs = std::atof(next().c_str());
        else if (a == "--idle-ms") o.idleMs = std::atof(next().c_str());
        else if (a == "--no-buttons") o.buttons = false;
        else if (a == "--no-notes") o.notes = false;
        else if (a == "--no-mailbox-rule") o.mailboxRule = false;
        else if (a == "--esp2-stub") o.esp2Stub = true;
        else if (a == "--esp2-checkpoint") o.esp2Checkpoint = true;
        else if (a == "--answer-f4") o.answerF4 = std::atoi(next().c_str());
        else if (a == "--no-answer-f4") o.noAnswerF4 = true;
        else if (a == "--f2-reply") o.f2Reply = int(std::strtol(next().c_str(), nullptr, 16));
        else if (a == "--cpu-hz") o.cpuHz = std::atof(next().c_str());
        else { usage(); return 2; }
    }
    if (!o.roms.empty()) {   // find the ROMs by checksum; an explicit --os/--wave wins
        romid::ScanResult rs = romid::scanFolder(o.roms);
        if (o.os.empty() && !rs.hasOs()) { std::fprintf(stderr, "%s\n", rs.problem().c_str()); return 2; }
        if (o.os.empty()) o.os = rs.osPath;
        if (o.wave.empty()) o.wave = rs.wavePath;
    }
    if (o.os.empty()) { usage(); return 2; }
    mkdir(o.out.c_str(), 0755);

    OsImage os;
    std::string err;
    if (!os.load(o.os, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    if (os.md5 != romid::kOsImageMd5)
        std::fprintf(stderr, "warning: OS image MD5 %s is not the supported version (%s)\n", os.md5.c_str(), romid::kOsImageMd5);

    Machine m;
    Machine::Config cfg;
    cfg.cpuHz = o.cpuHz;
    cfg.esp2Stub = o.esp2Stub;
    if (!m.init(os, cfg, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    m.setTraceDir(o.out);
    if (!o.wave.empty() && !m.loadWaveMemory(o.wave, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    m.esp2stub.mailboxAutoClear = o.mailboxRule;
    m.esp2.specFixes = !o.esp2Checkpoint;
    m.panel.answerF4Value = o.answerF4;
    m.panel.answerF4 = !o.noAnswerF4;
    m.panel.replyF2 = o.f2Reply;

    FILE* sum = std::fopen((o.out + "/summary.txt").c_str(), "w");
    auto both = [&](const char* fmt, auto... args) {
        std::printf(fmt, args...);
        if (sum) std::fprintf(sum, fmt, args...);
    };
#ifdef HARNESS_TRACE
    both("%s", "build: trace (instruction hook on)\n");
#else
    both("%s", "build: fast (no instruction hook)\n");
#endif
    both("os_image: %s\nos_size: %zu\nos_md5: %s\nos_header_body_end: 0x%08X\nos_header_entry: 0x%08X\n",
         os.path.c_str(), os.bytes.size(), os.md5.c_str(), os.bodyEnd, os.entry);
    both("os_trailer_sum: stored 0x%08X computed 0x%08X %s\n", os.storedSum, os.computedSum, os.sumOk ? "OK" : "MISMATCH");
    both("cpu_hz: %.0f\nesp2: %s\nesp2_mailbox_rule: %s\npanel_answer_f4: %s\n", cfg.cpuHz, o.esp2Stub ? "placeholder" : "core", o.esp2Stub ? (o.mailboxRule ? "on" : "off") : "n/a (microcode)",
         o.noAnswerF4 ? "off" : o.answerF4 >= 0 ? std::to_string(o.answerF4).c_str() : "panel positions");

    // Button translation: OS button id -> raw panel id, from the image's own table.
    int rawFor[64]; std::fill(rawFor, rawFor + 64, -1);
    for (uint32_t i = 0; i < profile::kButtonMapLen; ++i) {
        uint8_t id = os.at(profile::kButtonMap + i);
        if (id < 64 && rawFor[id] < 0) rawFor[id] = int(i);
    }

    // ---------------- Phase 1: boot
    uint64_t sampleFrom = m.cyclesFromMs(std::max(0.0, o.bootMs - 500));
    uint64_t expectAt = 0;
    auto seen = [&]() { return m.panel.text() == o.expect; };
    Machine::Stop stop = m.runUntil(sampleFrom, seen);
    if (stop == Machine::Stop::TimeLimit) {
        m.sampling = true;
        stop = m.runUntil(m.cyclesFromMs(o.bootMs), seen);
        m.sampling = false;
    }
    double bootHost = m.hostSeconds;
    uint64_t bootCycles = m.cycles();
    bool pass = (stop == Machine::Stop::Predicate);
    if (pass) {
        for (auto& d : m.panel.displayHistory) if (d.text == o.expect) { expectAt = d.cycle; break; }
    }
    const char* stopName[] = {"time limit", "trap", "hard stall", "expected display"};
    both("\n[boot]\nstop: %s\nemulated_ms: %.3f\nhost_s: %.4f\n", stopName[int(stop)], m.ms(bootCycles), bootHost);
    if (pass) both("expected_display_at_ms: %.3f\n", m.ms(expectAt));
    if (!o.esp2Stub) both("esp2_reg_0F9: 0x%06X\n", m.esp2.reg(0x0F9));
    if (stop == Machine::Stop::Trap) both("trap: %s (PPC 0x%06X)\n", m.trapReason.c_str(), m.trapPc);
    if (stop == Machine::Stop::HardStall) {
        const char* why = "unknown bra * loop";
        for (auto& e : profile::kErrorLoops) if (e.pc == m.stallPc) why = e.meaning;
        both("stall_pc: 0x%06X (%s)\n", m.stallPc, why);
    }
    both("display_now: \"%s\"\n", m.panel.text().c_str());

    // ---------------- Phase 2: settle and button tests
    std::vector<ButtonResult> buttons;
    double idleHost = 0, idleEmuMs = 0;
    uint32_t flag1526 = 0, flag0800 = 0;
    double noteMarkMs = -1, midiNoteMs = -1;
    uint64_t panelNoteWrites = 0, midiNoteWrites = 0;
    uint32_t tickAtBoot = m.peek(profile::kTickCounter, 4);
    uint32_t tickIdle0 = 0, tickIdle1 = 0;
    if (pass) {
        m.runUntil(m.cycles() + m.cyclesFromMs(o.settleMs));
        both("display_after_settle: \"%s\"\n", m.panel.text().c_str());
        flag1526 = m.peek(profile::kHeldFlag1526, 1);
        flag0800 = m.peek(profile::kTestFlag0800, 1);

        auto step = [&](const std::string& name, std::vector<std::vector<uint8_t>> msgs, double gapMs, double tailMs) {
            ButtonResult r; r.name = name; r.before = m.panel.text();
            size_t histStart = m.panel.displayHistory.size();
            for (size_t k = 0; k < msgs.size(); ++k) {
                r.sent += (k ? " | " : "") + hexBytes(msgs[k]);
                m.panel.inject(msgs[k], m.cycles(), "button test: " + name);
                m.runUntil(m.cycles() + m.cyclesFromMs(k + 1 < msgs.size() ? gapMs : tailMs));
            }
            r.after = m.panel.text();
            for (size_t k = histStart; k < m.panel.displayHistory.size(); ++k) {
                char b[64]; std::snprintf(b, sizeof b, "%.1f ms \"%s\"", m.ms(m.panel.displayHistory[k].cycle),
                                          m.panel.displayHistory[k].text.c_str());
                r.changes.push_back(b);
            }
            r.changed = r.after != r.before || !r.changes.empty();
            buttons.push_back(r);
        };

        if (o.buttons) {
            const uint8_t yes = uint8_t(rawFor[1]), no = uint8_t(rawFor[0]);
            const uint8_t PRESS = 0x81, RELEASE = 0x80;   // observed polarity (see report)
            step("80 +/Yes alone (expect no change)", {{RELEASE, yes}}, 0, 600);
            step("81 +/Yes alone, held (expect change)", {{PRESS, yes}}, 0, 300);
            step("80 +/Yes release", {{RELEASE, yes}}, 0, 600);
            step("+/Yes click: 81, 100 ms, 80", {{PRESS, yes}, {RELEASE, yes}}, 100, 600);
            step("-/No click: 81, 100 ms, 80", {{PRESS, no}, {RELEASE, no}}, 100, 600);
            step("-/No click: 81, 100 ms, 80", {{PRESS, no}, {RELEASE, no}}, 100, 600);
            step("+/Yes held 1500 ms (auto-repeat)", {{PRESS, yes}, {RELEASE, yes}}, 1500, 600);
            step("+/Yes double click within 250 ms", {{PRESS, yes}, {RELEASE, yes}, {PRESS, yes}, {RELEASE, yes}}, 60, 600);
            step("-/No held 3000 ms (back down)", {{PRESS, no}, {RELEASE, no}}, 3000, 600);
            step("checklist literal: 80 01 then 81 01 (raw 0x01)", {{0x80, 0x01}, {0x81, 0x01}}, 100, 600);
            step("release raw 0x01 (80 01)", {{0x80, 0x01}}, 0, 600);
        }

        // ---------------- Phase 2b: note test (first voice-chip reference trace)
        if (o.notes) {
            noteMarkMs = m.ms(m.cycles());
            uint64_t vw0 = m.voice.writes;
            // Panel key: 9x kk vv, velocity = vv + (x << 7); key = kk + transpose (0x24).
            m.panel.inject({0x90, 0x18, 0x64}, m.cycles(), "note test: panel key 0x18 (+0x24 = 60) vel 100");
            m.runUntil(m.cycles() + m.cyclesFromMs(500));
            m.panel.inject({0x90, 0x18, 0x00}, m.cycles(), "note test: panel key 0x18 release");
            m.runUntil(m.cycles() + m.cyclesFromMs(1000));
            panelNoteWrites = m.voice.writes - vw0;
            uint64_t vw1 = m.voice.writes;
            midiNoteMs = m.ms(m.cycles());
            m.serial.queueRx(1, {0x90, 0x3C, 0x64}, m.cycles());          // MIDI note on, channel 1
            m.runUntil(m.cycles() + m.cyclesFromMs(500));
            m.serial.queueRx(1, {0x80, 0x3C, 0x40}, m.cycles());          // note off
            m.runUntil(m.cycles() + m.cyclesFromMs(1000));
            midiNoteWrites = m.voice.writes - vw1;
        }

        // ---------------- Phase 3: steady-state speed
        uint64_t c0 = m.cycles(); double h1 = m.hostSeconds;
        tickIdle0 = m.peek(profile::kTickCounter, 4);
        m.runUntil(c0 + m.cyclesFromMs(o.idleMs));
        tickIdle1 = m.peek(profile::kTickCounter, 4);
        idleHost = m.hostSeconds - h1;
        idleEmuMs = m.ms(m.cycles() - c0);
    }

    // ---------------- Reports
    both("\n[speed]\nboot_emulated_s: %.4f\nboot_host_s: %.4f\nboot_realtime_factor: %.2fx\n",
         m.ms(bootCycles) / 1000, bootHost, bootHost > 0 ? (m.ms(bootCycles) / 1000) / bootHost : 0.0);
    if (idleEmuMs > 0)
        both("idle_emulated_s: %.4f\nidle_host_s: %.4f\nidle_realtime_factor: %.2fx\nidle_cpu_share_of_realtime: %.2f%%\n",
             idleEmuMs / 1000, idleHost, (idleEmuMs / 1000) / idleHost, 100.0 * idleHost / (idleEmuMs / 1000));
    both("total_emulated_s: %.4f\ntotal_host_s: %.4f\n", m.ms(m.cycles()) / 1000, m.hostSeconds);
#ifdef HARNESS_TRACE
    both("instructions: %llu\nmips_emulated: %.3f\n", (unsigned long long)m.instructions,
         m.instructions / (m.ms(m.cycles()) / 1000) / 1e6);
#endif

    both("\n[devices]\ntimer1_timeouts: %llu\ntick_counter_0BE072F4: %u (emulated %.0f ms)\n",
         (unsigned long long)m.timer1.timeouts, m.peek(profile::kTickCounter, 4), m.ms(m.cycles()));
    both("tick_at_boot_end: %u (emulated %.1f ms)\n", tickAtBoot, m.ms(bootCycles));
    if (idleEmuMs > 0) both("tick_over_idle: %u ticks in %.1f ms\n", tickIdle1 - tickIdle0, idleEmuMs);
    both("irq_taken: L2=%llu L3=%llu L4=%llu L5=%llu spurious=%llu\n",
         (unsigned long long)m.irqTaken[2], (unsigned long long)m.irqTaken[3], (unsigned long long)m.irqTaken[4],
         (unsigned long long)m.irqTaken[5], (unsigned long long)m.spuriousAcks);
    both("serial_A: tx %llu rx %llu tx_while_disabled %llu hold_overwrites %llu\n",
         (unsigned long long)m.serial.ch_[0].txBytes, (unsigned long long)m.serial.ch_[0].rxBytes,
         (unsigned long long)m.serial.ch_[0].txWhileDisabled, (unsigned long long)m.serial.ch_[0].holdOverwrites);
    both("serial_B: tx %llu rx %llu tx_while_disabled %llu\n",
         (unsigned long long)m.serial.ch_[1].txBytes, (unsigned long long)m.serial.ch_[1].rxBytes,
         (unsigned long long)m.serial.ch_[1].txWhileDisabled);
    both("serial_isr_reads: %llu\n", (unsigned long long)m.serial.isrBit3Reads);
    both("dma1: blocks %llu bytes %llu\n", (unsigned long long)m.dma1.blocks, (unsigned long long)m.dma1.bytes);
    auto esp2Stats = [&](const auto& e) {
        both("esp2: reg_writes %llu reg_reads %llu instr_writes %llu instr_reads %llu control_writes %llu last_control %02X\n",
             (unsigned long long)e.regWrites, (unsigned long long)e.regReads, (unsigned long long)e.instrWrites,
             (unsigned long long)e.instrReads, (unsigned long long)e.controlWrites, e.lastControl & 0xff);
        both("esp2_mailbox_0F7: reads_while_running %llu nonzero_reads %llu bad_instr_addr %llu readout_reads %llu\n",
             (unsigned long long)e.mailboxReadsWhileRunning, (unsigned long long)e.mailboxReadsNonZero,
             (unsigned long long)e.badInstrAddr, (unsigned long long)e.readoutReads);
    };
    if (o.esp2Stub) esp2Stats(m.esp2stub);
    else {
        esp2Stats(m.esp2);
        const Esp2Core& e = m.esp2;
        both("esp2_core: executed %llu halted_cycles %llu suspended_cycles %llu bioz_suspends %llu bioz_passes %llu overruns %llu pc %03X\n",
             (unsigned long long)e.executed, (unsigned long long)e.haltedCycles, (unsigned long long)e.suspendedCycles,
             (unsigned long long)e.biozSuspends, (unsigned long long)e.biozPasses, (unsigned long long)e.overruns, e.pc());
        both("esp2_core: mem_reads %llu mem_writes %llu ram_span %06X-%06X unmapped %llu voice_port_reads %llu writes %llu mac_sat %llu alu_sat %llu reserved_mac %llu unknown_spr r/w %llu/%llu\n",
             (unsigned long long)e.memReads, (unsigned long long)e.memWrites, e.ramLow, e.ramHigh, (unsigned long long)e.memUnmapped,
             (unsigned long long)e.voicePortReads, (unsigned long long)e.voicePortWrites, (unsigned long long)e.macSat,
             (unsigned long long)e.aluSat, (unsigned long long)e.reservedMacOps, (unsigned long long)e.unknownSprReads, (unsigned long long)e.unknownSprWrites);
    }
    both("voice_core: samples %llu irqs_raised %llu irq_acks %llu voice_stops %llu wave_memory %s\n",
         (unsigned long long)m.voice.samples, (unsigned long long)m.voice.irqsRaised, (unsigned long long)m.voice.irqAcks,
         (unsigned long long)m.voice.voiceStops, o.wave.empty() ? "none (zeros)" : "native image in bank 2");
    both("voice: writes %llu reads %llu bad_page %llu ACTV %08X MODE %08X\n",
         (unsigned long long)m.voice.writes, (unsigned long long)m.voice.reads,
         (unsigned long long)m.voice.badPage, m.voice.actv, m.voice.mode);
    both("%s", "voice_write_counts:");
    for (auto& kv : m.voice.writeCounts) both(" %02X:%llu", kv.first, (unsigned long long)kv.second);
    both("%s", "\nvoice_read_counts:");
    for (auto& kv : m.voice.readCounts) both(" %02X:%llu", kv.first, (unsigned long long)kv.second);
    both("\npanel: hello %llu f2 %llu f3 %llu f4 %llu reset_pulses %llu unknown %llu\n",
         (unsigned long long)m.panel.helloCount, (unsigned long long)m.panel.f2Count,
         (unsigned long long)m.panel.f3Count, (unsigned long long)m.panel.f4Count,
         (unsigned long long)m.panel.resetPulses, (unsigned long long)m.panel.unknownMessages);
    both("%s", "leds:");
    for (auto& kv : m.panel.ledState) both(" %02X=%d", kv.first, kv.second);
    both("\nmidi_out_bytes: %zu\nflash_writes: %llu\nflash_service: calls %llu bytes %llu refused %llu\nflag_0BE01526: %u\nflag_0BE00800: %u\n",
         m.midiOut.size(), (unsigned long long)m.flashWrites, (unsigned long long)m.flashServiceCalls,
         (unsigned long long)m.flashServiceBytes, (unsigned long long)m.flashServiceRefused, flag1526, flag0800);

#ifdef HARNESS_TRACE
    both("%s", "\n[checkpoints]\n");
    for (auto& c : profile::kCheckpoints) {
        double t = -1;
        for (auto& h : m.checkpointHits) if (h.first == c.pc) t = m.ms(h.second);
        if (t >= 0) both("0x%04X %-40s %10.3f ms\n", c.pc, c.label, t);
        else both("0x%04X %-40s %10s\n", c.pc, c.label, "not reached");
    }
#endif

    both("%s", "\n[display history]\n");
    for (auto& d : m.panel.displayHistory)
        both("%10.3f ms  \"%s\"  raw %02X %02X %02X %02X  dots %02X\n", m.ms(d.cycle), d.text.c_str(),
             d.raw[0], d.raw[1], d.raw[2], d.raw[3], d.dots);

    if (!buttons.empty()) {
        both("%s", "\n[buttons]\n");
        for (auto& b : buttons) {
            both("%-48s sent [%s]  \"%s\" -> \"%s\"  %s\n", b.name.c_str(), b.sent.c_str(), b.before.c_str(),
                 b.after.c_str(), b.changed ? "CHANGED" : "no change");
            for (auto& c : b.changes) both("      %s\n", c.c_str());
        }
    }

    if (!pass) {
        both("%s", "\n[stall analysis]\n");
        std::vector<std::pair<uint64_t, uint32_t>> h;
        for (auto& kv : m.pcSamples) h.emplace_back(kv.second, kv.first);
        std::sort(h.rbegin(), h.rend());
        both("%s", "pc_histogram_last_500ms (top 16):\n");
        for (size_t i = 0; i < h.size() && i < 16; ++i) {
            char buf[128]; m68k_disassemble(buf, h[i].second, M68K_CPU_TYPE_68020);
            both("  %06X x%-6llu %s\n", h[i].second, (unsigned long long)h[i].first, buf);
        }
#ifdef HARNESS_TRACE
        both("%s", "last_pcs:");
        for (int i = 0; i < 32; ++i) both(" %06X", m.pcRing[(m.pcRingPos - 32 + i) & 255]);
        both("%s", "\n");
#endif
    }
    both("%s", "\n[recent device accesses]\n");
    for (auto& d : m.recentDeviceAccesses(48)) {
        both("%10.3f ms pc %06X %-8s %c%d %08X = %08X\n", m.ms(d.cycle), d.pc,
             d.dev == 'v' ? "voice" : d.dev == 'e' ? "esp2" : d.dev == 'm' ? "m68340" : "unmapped",
             d.rw, d.size, d.addr, d.value);
    }

    // Panel transcript
    if (FILE* f = std::fopen((o.out + "/panel_transcript.txt").c_str(), "w")) {
        std::fprintf(f, "# t_ms  dir  bytes  decode   (OS> = OS to panel, <PNL = panel to OS)\n");
        for (auto& e : m.panel.transcript)
            std::fprintf(f, "%10.3f %s %-24s %s\n", m.ms(e.cycle), e.fromOs ? "OS> " : "<PNL", hexBytes(e.bytes).c_str(),
                         e.decoded.c_str());
        std::fclose(f);
    }
    if (FILE* f = std::fopen((o.out + "/midi_out.txt").c_str(), "w")) {
        std::fprintf(f, "# t_ms byte (channel B, MIDI out)\n");
        for (auto& e : m.midiOut) std::fprintf(f, "%10.3f %02X\n", m.ms(e.first), e.second);
        std::fclose(f);
    }
    if (FILE* f = std::fopen((o.out + "/unmapped_accesses.csv").c_str(), "w")) {
        std::fprintf(f, "address,rw,size,count,first_pc,first_ms,last_value\n");
        for (auto& kv : m.unmapped)
            std::fprintf(f, "%08X,%c,%d,%llu,%06X,%.3f,%08X\n", std::get<0>(kv.first), std::get<1>(kv.first),
                         std::get<2>(kv.first), (unsigned long long)kv.second.count, kv.second.firstPc,
                         m.ms(kv.second.firstCycle), kv.second.lastValue);
        std::fclose(f);
    }
    if (noteMarkMs >= 0)
        both("\n[note test]\npanel_key_at_ms: %.3f voice_writes: %llu\nmidi_note_at_ms: %.3f voice_writes: %llu\n",
             noteMarkMs, (unsigned long long)panelNoteWrites, midiNoteMs, (unsigned long long)midiNoteWrites);
    both("\n[unmapped summary]\ndistinct (addr,rw,size): %zu\n", m.unmapped.size());

    bool buttonOk = false;
    for (auto& b : buttons) if (b.name.rfind("+/Yes click", 0) == 0 && b.changed) buttonOk = true;
    both("\nRESULT: boot %s, button %s\n", pass ? "PASS" : "FAIL", buttonOk ? "PASS" : (buttons.empty() ? "not run" : "FAIL"));
    if (sum) std::fclose(sum);
    return (pass && (buttonOk || !o.buttons)) ? 0 : 1;
}
