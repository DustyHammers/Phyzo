# phyzo_boot: headless boot harness

Boots an original synthesizer OS image on an emulated MC68340 machine with
placeholder devices plus an emulated voice chip, and records what the OS does. No UI, no JUCE; audio is rendered to WAV by phyzo_play.
It builds on Linux and macOS with CMake and a C++17 compiler.

The repository contains **no manufacturer code or data**. The OS image is a
user-supplied file passed on the command line; every table the harness uses
(segment font, button map) is read from that image at run time. The addresses in
`src/os_profile.h` come from our own analysis and locate things in the image;
they are not copied content.

## What is emulated

| Part | Model |
|---|---|
| CPU | Musashi in 68020 mode (the OS uses only instructions common to CPU32 and 68020) |
| Flash 0x0–0xFFFFF | Read-only to the CPU; synthetic vector table and stubs below 0x4000; OS at 0x4000; rest erased (0xFF). The user flash 0xD0000–0xFFFFF is written only through the flash-write service |
| RAM 0x0BE00000–0x0BE3FFFF | 256 KB, zero-filled at start |
| 68340 timer 1 | Counts down from PREL1 at fsys/2, sets TO, reloads; IRQ at the IR level when CR.IE2 |
| 68340 serial A/B | Holding + shift register transmitter, 3-byte receive FIFO, paced input; ISR bit 3 reads 0 |
| 68340 DMA ch1 | Copies BTC bytes per start (byte/byte, SAPI/DAPI), leaves SAR advanced |
| ESP2 | emulated (src/esp2_core.*): runs the microcode the OS loads; see "ESP2 core" below. The boot-milestone placeholder remains as `--esp2-stub` |
| Voice chip | emulated (src/voice_core.*): 48 voices, loop-end IRQ on level 5, wave memory from `--wave`; see "Voice-chip core" below |
| Front panel | Serial channel A model: answers the hello and the F4 request (all 26 analog control positions, `docs/PANEL_CONTROLS.md`), decodes display and LED messages |
| Everything else | Reads 0, writes ignored, logged |

Boot-ROM services are host traps: reboot (vector slot 1), OS update (slot 254),
and an unexpected exception lands on a per-vector stub. Slot 251, the flash-write
service, is served (since v0.8.3), so the OS's saves work:

- The OS reads the slot once, at boot (into RAM 0x0BE00808), and calls it as a C function with (destination flash
  address, source address, length, scratch buffer, scratch length) on the stack; D0 = 0 means success. It passes whole regions (0xD0000 and 0xE0000, 64 KB each;
  0xF0000, 32 KB; 0xFFE00, 512 bytes), so a plain copy is all the flash needs (no erase or program sequence).
- Slot 251 points at a two-instruction routine in the boot block, `move.l sp,($2040).l` then `rts`. Its write to the
  service port 0x2040 (only from that routine) runs the copy in the host (`Machine::flashService`), which puts the
  result in D0. A host service rather than a 68k copy loop: the copy takes no machine time, and the result is in D0
  before the routine returns, so a state saved between any two instructions is complete.
- Only destinations inside the user flash, 0xD0000–0xFFFFF, are accepted (the copy may end exactly at 0x100000). Any
  other copies nothing, returns 1 and is logged as unmapped access `S` at the destination (value = the length).
- Every other flash write stays read-only. Calls, bytes and refusals are counted (`flash_service` in the boot summary).

## Build

```sh
../scripts/fetch_deps.sh musashi          # Musashi at the pinned commit (scripts/deps.env) into ../work/deps
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Targets: `phyzo_boot` (fast core, used for speed figures), `phyzo_boot_trace`
(per-instruction hook: boot checkpoints and PC history), `os_disasm`.

## Run

```sh
./build/phyzo_boot_trace --os /path/to/os_image.bin --out boot_out
./build/phyzo_boot_trace --roms /path/to/roms --out boot_out
```

`--roms <folder>` finds the OS image and the native wave image in a folder by MD5 checksum, whatever the file
names (`src/rom_id.*`; supported versions: OS image `8de06f48bfb0d847cacab05b763e22c5`, native wave image
`42a974e31e48b05815d07554d0063171`). If one is missing, the message names it and the folder. An explicit `--os`
or `--wave` wins over the folder; an OS image with another checksum gets a warning, since the addresses in
`src/os_profile.h` belong to the supported version.

Options: `--expect "P 01"`, `--boot-ms`, `--settle-ms`, `--idle-ms`,
`--no-buttons`, `--no-notes`, `--no-mailbox-rule`, `--answer-f4 <0..1023>` (one value for all controls),
`--no-answer-f4` (leave F4 unanswered, the behaviour before F-02), `--f2-reply <hex>`, `--cpu-hz`.
By default the panel answers F4 with the fresh-instance positions of `docs/PANEL_CONTROLS.md`. The summary reports
ESP2 register 0x0F9 after boot. `phyzo_play` accepts `--no-answer-f4` too.

The run boots until the display shows the expected text, settles for one second,
runs the button tests, plays one panel key and one MIDI note, then runs idle for
the speed measurement. Exit code 0 means boot and button test passed.

Outputs in `--out`:

| File | Contents |
|---|---|
| `summary.txt` | Baseline check, stop reason, checkpoints, display history, button results, device statistics, speed |
| `panel_transcript.txt` | Every byte between OS and panel, decoded |
| `midi_out.txt` | Channel B output |
| `voice_writes.csv` | Every voice-chip register write |
| `esp2_commands.csv` | Every ESP2 register and instruction command, and control writes |
| `unmapped_accesses.csv` | Accesses outside the modelled devices, aggregated |

**The trace files quote values produced by the OS (ESP2 microcode, register
images). Keep them out of this repository.** `.gitignore` excludes the usual output
locations.

`ctest` runs the tests that need no ROM data (`rom_id`: MD5 and ROM identification), plus the boot test when
configured with `-DOS_IMAGE=/path/to/image`. `state_xarch_test save <file>` / `load <file>` moves a complete machine
state (68k with interrupts, 8 voices, an ESP2 program; no ROM data) between builds: the macOS CI job saves on arm64,
loads under Rosetta (x86_64) and the other way round, and checks the same continuation and the same saved bytes.

## Voice-chip core (src/voice_core.*)

The voice chip at 0x00400000 is now emulated: 48 voices, one flat page of 32 longword registers per voice
(page select 0x7C), ACTV/MODE/IRQV globals, 44.1 kHz output. Each CPU slice ends on a sample boundary, so
register writes take effect within one sample. Wave memory: `--wave <native image>` loads the 4 MB image
built by `rombuilder --native-out` into bank 2 (chip addresses 0x20000000 + n). The bank comes from address
bits 28-29 of START/END/ACCUM.

Modelled: ES5506 linear interpolation (11-bit fraction), FC with 11 fraction bits, END fraction (4 bits,
0x34) and ACCUM fraction (11 bits, 0x3C), forward/bidirectional loops, BLE+LEI one-shot latches, loop-end IRQ
on level 5 (IRQV read acknowledges), STOP0 at the end of one-shots (a voice with START = END keeps running: the OS's table-sweep frame chaining
writes START = END and expects that), CR bit 19 as a start strobe, signed 8.8
volume and K1/K2 ramps with the slow bit (bit 0: applied every 8th sample), ECOUNT, the four non-resonant
4-pole filter modes, the ES5506 log volume law, live read-back of every register, and the filter field in CR bits 8-10 (see below).
Register 0x48, 0x58-0x68, 0x74 and CR bit 10 changes are logged to `resonance_register_writes.csv`. Output is the dry stereo sum, unclamped; 2^19 is the full scale of the chip's 20-bit output. Voice level =
(sample × volume) >> 12 (A-17 output headroom for 48 voices, Derived; was >> 11, i.e. 6.02 dB louder).

## phyzo_play: script-driven renders

    build/phyzo_play --os <OS image> --wave <native image> --script <file> --out <dir> [--trace]
    build/phyzo_play --roms <folder> --script <file> --out <dir> [--trace]

Boots to "P 01", settles 1 s, then runs the script in 10 ms real-time steps. Script lines:
`<ms> midi <hex...>`, `<ms> panel <hex...>`, `<ms> poke8|poke16|poke32 <addr> <value>`,
`<ms> find <hex...>`, `<ms> rec <name>`, `<ms> stop`, `<ms> end`. Recordings are 32-bit float WAV
(1.0 = 20-bit full scale). `summary.txt` lists peak/RMS, samples over the 20-bit range, the real-time factor
and the level-5 IRQ count per recording. `--trace` adds `voice_writes.csv` and `resonance_register_writes.csv`.
`phyzo_boot` / `phyzo_boot_trace` accept `--wave` as well.

### Filter modes

CR bits 8-10 select the filter: 0x000-0x300 the four ES5506 pole-pair modes (unchanged), 0x600 resonant LP,
0x700 resonant BP, 0x500 bypass. Register 0x48 = A << 9 | B, two 9-bit shift-and-add codes
((1 +/- 2^-u) * 2^-s; s = bits 0-3, u = bits 4-7, bit 8 = subtract). Resonant modes: input * A, then two
state-variable (Chamberlin) sections in series on K1 and K2, both damped by B; LP outputs for 0x600, BP outputs for
0x700. k = K/65536 at the same 12-bit precision as the pole-pair modes. States are sample << 4 and saturate at
+/-2^21 (the ES5506's 18-bit state width; assumed); `--state-limit <bits>` changes it. The model and its fit against
recordings are documented in FILTER_REPORT.md (private data repo).

phyzo_play prints per recording the resonant-mode samples and filter-state saturations.

### filter_test (chip-level tests, no OS CPU)

    build/filter_test --os <image> --wave <native image> --zone-start N --zone-len N --zone-pitch P [--key 36]
                      (--sweep <dir> | --stress | --bench <cr filter bits hex> <index>) [--state-limit bits]

Plays one looped zone (addresses are arguments, from your own zone list) through one voice, takes the resonance codes
and cutoff values from the user's OS image at run time, and writes sweep WAVs (every mode; resonant modes at
indices 0, 16, 32, 49, 50), a stability stress table (all indices, extreme and toggling K, maximum ramps) or a
48-voice CPU benchmark. Build it against an older core with `-DFILTER_TEST_BASELINE` for before/after comparisons.

## ESP2 core (src/esp2_core.*)

Written from the public ESP2 documents: Part I Instruction and Hardware Specification (Andreas, Mauchly, Dattorro),
the ESP2 Object Format Specification, and US patent 5,517,436. No microcode is contained here; the core runs whatever
the OS loads through the host port.

- Instruction memory 300 × 96 bits, 1,024 × 24-bit operand space with the SPR map of Part I section 11, host
  interface per section 12 (HOST_CNTL bits, HOST_ESP_FACE on host bytes 0x1B-0x1D).
- MAC (24×24, fixed normalising shift, 52-bit accumulator, MACZERO/MACP/MAC seeds, barrel shift, 48-bit saturation,
  MACRL, MACH/MACL), ALU (32 ops, CCR/CMR, skip bits), delayed jumps, PC stack, REPT, BIOZ, AGEN (8 regions, modulo,
  plus-one, BASE update), DIL/DOL, indirection with the patent's pointer latencies, magnitude truncation of written
  data per HARD_CONF (MAG_TRUNC, TRUNC_WIDTH).
- Per instruction the order is: memory access of the previous line, MAC/AGEN operand and seed fetch, previous ALU
  result lands, ALU, MAC result, DIL load. This reproduces the latency rules of Part I section 1.3.
- Execution is translated: each instruction is classified when the host writes it (pure NOPs, plain operands,
  MAC seed/shift/latch behaviour, discarded results); `--esp2-ref` runs the reference interpreter instead.
  `--esp2-checkpoint` restores the pre-spec SPR semantics of the v0.5 checkpoint.
- Threaded dispatch: every line has its own routine (`runT<...>` for the specialised lines, `runStep`, `runNop`), and
  each routine ends by jumping to the next line's routine. Since v0.8.5 that jump is a guaranteed tail call
  (`[[clang::musttail]]`, or `[[gnu::musttail]]` with gcc 15 or later; other compilers: a plain call, as before), so
  a chain of up to 256 lines never nests calls and each routine keeps its own indirect jump for the branch predictor.
  The output is the same to the bit either way. `scripts/check_esp2_dispatch.py` disassembles the core and checks
  that every line routine reaches the next line by a jump; the macOS CI job runs it for arm64 and x86_64 (the plugin
  is universal since v0.8.6).
- Clock: 192 instruction cycles per 44.1 kHz period (Assumed; the OS-built image needs 188).

Board wiring (derived from the OS's shell program): the voice chip's ten output channels are read by the ESP2 as
external words at 0xC00000 + 2·channel (+1 = right), 20-bit values left-justified (Assumed); delay RAM 64K words at
0x600000; SER0 (0x3EF/0x3EE) = audio input, SER1 (0x3ED/0x3EC) = stereo DAC.

### phyzo_play additions

`<name>_wet.wav` (DAC lines, 1.0 = 2^23) is written next to every dry recording; `clip_report.csv` lists clamps and
saturations per recording by destination register. Options: `--esp2-stub`, `--esp2-ref`, `--esp2-checkpoint`,
`--esp2-ips N`, `--audio-in file.wav` (16-bit or float, looped into SER0), `--profile` (host time split into 68k CPU
+ devices, voice core, ESP2). Script commands `display [label]` and `esp2reg <addr...>` log the panel display and
ESP2 registers.

### esp2_test (chip level, no OS CPU)

    build/esp2_test --os <image> --list | --testsine out.wav | --bypass | --bench s
    build/esp2_test --replay <esp2_commands.csv> [--seconds 10] [--ref] [--checkpoint] [--ips N] [--wav out.wav]

`--replay` re-applies a recorded host command stream at its original times and then runs the loaded image with a
deterministic signal on all ten voice channels: ESP2-alone speed and a DAC hash for A/B comparisons.

### Audit instrumentation (v0.6)

phyzo_play script commands: `esp2dump <name> [ram_start count]` (instruction memory, registers, RAM words with write
epochs), `esp2stats <label>` (saturations, stale reads, region-bound violations, unmapped and external writes),
`esp2epoch [lo hi]` (start a new write epoch; stale reads are counted inside the window). Options: `--trace-esp2`
(ESP2 host commands only), `--esp2-ram-alias` (decode unmapped ESP2 addresses into RAM by their low 16 bits; default
drops them). Every run writes `esp2_external_writes.csv` and lists CPU accesses outside the mapped devices; the clip
report gives, per destination register, the count and the largest overshoot in 24-bit LSBs.

The bookkeeping behind these on every delay-RAM access and AGEN (the RAM range used `ramLow` / `ramHigh`, the write
epochs `wEpoch`, the stale reads `staleReads` / `staleLow` / `staleHigh`, the region-bound violations
`regionViolations`) is an option, `Esp2Core::audit` (default on: the tools and the tests). The plugin's engine turns it
off for its machines (v0.8.5): those four are then not updated (they keep the values they were loaded with), every
other statistic is kept, and the output is the same to the bit. The option is not part of the saved state.

## Plugin engine (src/engine.*, src/resampler.*)

The plugin (`plugin/`) runs the machine through `Engine`, which has no plugin-framework code and is tested here
(`engine_test`). It boots the OS (or restores a saved machine state) on a worker thread, then runs the machine per
host audio block. MIDI bytes go to channel B at the machine cycle that matches their host sample position plus a
constant latency, which the plugin reports to the host. The ESP2 DAC output (24-bit, 1.0 = 2^23) is converted from
44.1 kHz to the host rate by `Resampler` (Kaiser windowed sinc, 128 taps at 44.1 kHz, about 100 dB accuracy, no
added delay). `Machine::saveState()/loadState()` hold the complete machine (CPU, RAM, all devices, timing) and the
user flash (section `UFLS`, since v0.8.3); the rest of the flash and the wave memory are not stored. A state without
the user flash (saved before v0.8.3) loads exactly as before, with the user flash left erased (that machine cannot
save: its OS read the service pointer at boot, when it was null, and indexed blank user memory). Several
machines can run in one process: Musashi's single CPU is shared under a lock, with each machine's CPU state parked
while another one runs.

Factory user flash (`src/user_flash.*`, `Engine::setFactoryUserFlash`, on in the plugin): what the OS's own factory
restore writes (164,352 bytes). It is made once per OS image in a hidden machine: a freshly booted machine plays the
restore on its panel (Save down, Compare pressed, Save up; the display asks `r5t?`; +/Yes; the display shows `burn`,
then `P 01`), waiting for the display rather than a fixed delay. The flash is read only once the display is back on
`P 01` and the OS has written exactly that many bytes (`burn` is watched for but not required: the service takes no
machine time, so it can be gone before it reaches the panel). It is cached by the OS image's MD5 (the plugin:
`~/Documents/Phyzo/cache/`; made from the user's own OS on the user's machine, never shipped or committed). A new
machine gets it before it boots; a restored state keeps its own user flash (none for a state from before v0.8.3);
when the OS restarts itself, the new machine keeps the user flash the old one had. Without it (tests, tools) the user
flash starts erased.

Audio with the factory user flash is not byte-identical to an erased machine (v0.8.2), by design: the OS indexes its
user memory at boot, which takes about 0.2 s longer, so LFO, arp and random timing differ. Every preset and sound is
identical.

## Preset library (src/library/*, v0.8.3)

The editor's LIBRARY area (`docs/editor/LIBRARY.md`) browses `.syx` files in `~/Documents/Phyzo/Library/`.

- **Auditioning:** a clicked preset's stored single-preset message goes into the synth's MIDI input, and the OS puts
  it in Compare. Save on the panel keeps it.
- **Hidden machines:** the files are read by hidden machines on a worker thread, one job at a time in the process,
  never on the audio thread. The machines are driven only through the OS: panel Dump and +/Yes, program changes,
  SysEx in, name reads.
  - Fresh machines boot with the factory user memory. Clones restore the running machine's snapshot.
  - `syx.*` splits and classifies the files: single preset `02 17`, bank `0D <bank>`.
- **Index cache:** `library.*` keeps `cache/library/<file MD5>.idx`. It also scans the folder (factory banks first,
  then files, then SINGLES) and makes new file names (never overwriting).
- **Jobs (`service.*`):**
  - make the factory bank files from the user's own OS (`Library/Factory/`, never shipped);
  - read files;
  - SAVE (refused while the Compare LED, 0x16, is lit), EXPORT and IMPORT.

## phyzo_stress: real-time check of the plugin engine

    build/phyzo_stress [--seconds S] [--no-state]

No ROMs needed (a small program of our own stands in for the OS). Runs the engine as a host would, one block per
period, at 512 and 128 samples and 44.1 and 48 kHz, while another thread sweeps a knob (one move per ms), presses a
button every 50 ms and asks for the state every 100 ms. Prints mean, 99.9th percentile and worst block time,
overruns and the longest wait for the engine lock. On a shared machine the worst block also shows the machine's own
scheduling noise; the lock wait is the part the engine controls.

## speed_bench: component cost without ROMs

    build/speed_bench            # all parts
    build/speed_bench effect     # only the ESP2 core on the effect-like program
    build/speed_bench multi [S]  # 1, 2 and 3 instances at once, S seconds each (default 5)

Host milliseconds per second of 44.1 kHz audio (1000 = one core in real time) for: the ESP2 core on an effect-like
program and on a busy random program (fast path and reference interpreter, with output hashes that must not change
with speed work), the voice chip with 32 voices, and the 68k with the machine. In the plugin, the debug overlay shows
the same split measured on the real OS and presets.

`multi` runs 1, 2 and 3 machines on their own threads at once, paced as a host at 48 kHz with blocks of 512 and 128
samples; each has a 68k busy loop, 32 voices and the effect-like ESP2 program. Prints CPU per instance (average and
peak block, in % of the block period), the worst block and the overruns. Instances share one 68k core (Musashi has a
single global CPU): only 68k slices take turns on it, under a lock held for at most one sample period; each machine's
timers, serial channels, voice chip and ESP2 run outside it, in parallel with the other instances.
