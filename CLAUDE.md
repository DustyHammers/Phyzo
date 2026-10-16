# Phyzo: rules for working in this repository

Phyzo is a VST3/AU plugin for personal use that runs a 1998 hardware synthesizer's original OS on emulated hardware
(`phyzo-emu/`). The OS provides all synth behaviour; our code is the hardware. The owner has no local build machine:
GitHub Actions builds everything, and the owner tests the plugin in REAPER on an Apple Silicon Mac. Guide the
owner step by step through anything they must do by hand on github.com or on the Mac.

## Rules (from the owner; do not relax them)

1. **Never commit ROM files or other proprietary data**: no OS image, wave data, tables, recordings, renders,
   trace output or SysEx dumps. CI rejects .bin/.img/.wav/.mid/.syx/.mp3 files and any file over 1 MB.
2. **No synth parameters added or removed** without the owner's explicit instruction.
3. **Mockup before any UI.** Show a mockup and wait for approval before building any user interface.
4. **Stop after each phase**: summarise what changed and say what to test.
5. **THIRD_PARTY_NOTICES.md** (`phyzo-emu/THIRD_PARTY_NOTICES.md`): never change its existing entries. Complete the
   "Planned dependencies" entry for each dependency (full licence text and version) when it is added.
6. **Identify ROMs by checksum, never by file name** (`phyzo-emu/src/rom_id.*`):
   OS image MD5 `8de06f48bfb0d847cacab05b763e22c5`, native wave image MD5 `42a974e31e48b05815d07554d0063171`.
   The OS may also be the original manufacturer's OS update file (MIDI file or SysEx), decoded in memory
   (`os_image.*`, `osupdate::`) and identified by the decoded image's MD5; the decoded image is never written to disk.
7. **Never commit skins, images or fonts.** Skins live in `~/Documents/Phyzo/skins/<name>/` on the owner's Mac; the
   repo has only the skin engine and the text-only test skin (`plugin/skin/tests/skins/`). CI rejects image and font
   files and .rml/.rcss files outside the test skin. Gearmulator's skinning and Lua docs are a design reference
   only: write our own code.
8. **README.md:** Never change `README.md` unless the prompt explicitly asks for that exact README change. That covers
   changelog or "What's new" entries, version notes, feature lists and wording fixes alike. Release notes go in the PR
   description. If a prompt doesn't mention the README, leave it alone. If a change seems to need README text, say so
   in the report and stop there.

## Naming and identity

- Never write the original synth's name or its maker's name anywhere in this repository or on GitHub (code, comments,
  file names, docs, tests, commit messages, branch names, PR titles and descriptions, issues, CI names). Use neutral
  terms: the original synth, the original manufacturer, the OS, the update file. Other deliberately kept names are
  listed in PACKAGE_CONTENTS.md. Chip part numbers MC68340, ES5506 and ESP2 stay. Say "table sweep", not the
  original technique's brand-like name.
- The five realtime performance knobs are named by function in code: perf1 (System Controller 3, effect modulation),
  perf2 (table-sweep position, CC 71), perf3 (filter cutoff, CC 74), perf4 (detune, CC 79), perf5
  (System Controller 4). Visible labels come from the skin.
- Keep the MIDI/SysEx identity (manufacturer code, model number, message formats) exactly as the OS uses it.
- Plugin identity, fixed forever (hosts use it to find the plugin in saved projects): manufacturer "DHammers",
  manufacturer code "DHmr", plugin name "Phyzo", plugin code "Phyz", bundle ID com.dhammers.phyzo, plus
  `JUCE_VST3_CAN_REPLACE_VST2=0`. Resulting VST3 class ID `ABCDEF019182FAEB44486D725068797A`, AU `aumu Phyz DHmr`.
  CI checks all of these (`scripts/check_plugin_identity.py`).

## Other constraints

- No CI job may ever need the ROM files.
- Do not copy Gearmulator code (GPL-3); using its design as a reference is fine.
- Speed work must keep the emulator's output byte-identical to the current version.

## Build and CI

- Dependencies are pinned in `scripts/deps.env` and fetched by `scripts/fetch_deps.sh` into `work/deps/` (ignored).
- `.github/workflows/ci.yml`: data-file guard over every commit (`scripts/check_no_data.py`), ROM-free tests on
  Linux (`ctest` in `phyzo-emu` and `tools/rombuilder`), and, on `workflow_dispatch` only, the macOS plugin build
  (universal arm64 + x86_64, `lipo` checked; x86_64 tests and the cross-architecture state test under Rosetta) with
  identity check, auval, pluginval (strictness 10) and the downloadable zip (kept 7 days). The ESP2 and voice-core
  tests hold golden hashes of the current output: a speed change that alters them has changed the sound.
- To give the owner a build: once the branch is ready to test, trigger CI's `workflow_dispatch` on that branch.
- Owner's install guide: `docs/INSTALL_MAC.md`.
- Plugin architecture: `phyzo-emu/src/engine.*` (no JUCE) runs the machine per host block, boots/restores on a worker
  thread, delivers MIDI sample-accurately with a constant reported latency, and converts 44.1 kHz to the host rate
  (`resampler.*`); the optional Audio In bus goes the other way into the ESP2's serial input (SER0), aligned like
  MIDI. MIDI ports: a CoreMIDI virtual pair per instance (`plugin/Source/VirtualMidiPort.*`), merged into the emulated
  MIDI input as whole messages (`midi_stream.*`). `plugin/` wraps it (JUCE) and scans `~/Documents/Phyzo/roms/`. Musashi has one global CPU:
  machines take turns on it per 68k slice under a lock (`Machine::becomeCpuOwner`); each machine's devices (timers,
  serial, voice chip, ESP2) run outside the lock, in parallel across instances. Project state = complete machine state
  (`Machine::saveState`), tied to the ROM checksums, including the user flash 0xD0000-0xFFFFF (presets saved on the
  panel), which only the boot ROM's flash-write service (slot 251, a host service; the OS reads the slot only at boot)
  writes. New instances get the factory user flash (`user_flash.*`): made once per OS image by the OS's own factory
  restore in a hidden machine, cached in `~/Documents/Phyzo/cache/` (from the user's OS: never shipped or committed).
  States from before v0.8.3 (no user flash) load exactly as then, erased (they can't save). With the factory user flash
  the OS indexes its user memory at boot (about 0.2 s longer): audio is not byte-identical to v0.8.2 (LFO, arp, random
  timing), by design; every preset and sound is.
- Startup log (F-65, `plugin/Source/Diag.h`): `~/Documents/Phyzo/Phyzo-log.txt` (under 256 KB) and NSLog `Phyzo:`,
  always on: host, ROMs, machine state, prepareToPlay, refused layouts, audio start, bypass, editor/GL/skin steps. Never
  written from the audio thread (it sets flags the processor's timer logs). Separate from the skin log.
- Skins: `plugin/skin/` (no JUCE) is the runtime: RmlUi 6.3 + Lua 5.4 (sandboxed, 3 s limit) + FreeType, the custom
  elements knob/pbutton/led/vfdigit and the `panel`/`plugin` Lua API; `skin_test` drives the test skin in CI.
  `plugin/Source/RmlSkinComponent.*` draws a file skin with OpenGL (all RmlUi/Lua work on the GL thread);
  `PluginEditor.*` has the Built-in skin, the right-click routing (a learnable control: MIDI learn's menu; an edited
  editor value: Revert; each then "Open window…"; anything else opens the window), zoom and the fallback (rack, then
  Built-in). The plugin window (v0.9.0, `PhyzoWindow.*`): over the panel and the editor unit, never changing the
  plugin's size; tabs SYSTEM, LIBRARY, SKIN, MIDI (the old menu's Skin, Reload, Zoom, Editor, MIDI and Developer moved
  there) and a footer for messages, questions and progress; Esc or its round close button closes it (Esc goes to the
  host only when no window, menu or inline edit is open). Its shared, themeable parts are in `plugin/Source/ui/` (no
  plugin includes): the font is the skin's `<meta name="ui-font">` file (loaded with JUCE on load and F5), else JUCE's
  default sans; never commit a font. Rack legends `<legend action="rack" value="EDITOR|SAVE">` (`skin_view.cpp`)
  toggle the editor unit / open Save with a name; `action="library"` is obsolete (unavailable, one skin-log line).
  Skin authoring guide: `docs/SKINS.md`. The skin is a global setting; the window scale (`WindowScale.h`: 50-200 %; a file
  skin's 100 % is 3/4 of its 1795 x 848 dp body in points, 133 % is full size; Built-in's 100 % is its own size;
  aspect locked, resized by our own corner drag, never larger than the screen's usable area (F-32): Zoom steps that
  don't fit are greyed out and a saved size that doesn't fit steps down) is saved globally and per project; neither is
  a parameter.
  Per-element knob positions and the window scale are project state (processor state version 8; version 3 sizes are
  on the old scale and are converted x 4/3 for file skins when an editor opens; version 5 adds the editor unit's
  open state; version 6 the MIDI learn bindings; version 7 Sync to host tempo; version 8 the files last loaded into
  Bank 1 / 2, older: none).
- Editor screens (F-30, docs/editor/): `phyzo-emu/src/editor/` (no JUCE) reads the OS's parameter tables from the
  user's OS image at run time (never copied into the repo), builds the pages and reads the visible page with SysEx
  partial reads; `Engine` queues its messages whole at cable speed, puts the page read directly behind program changes,
  bank selects and dumps, and takes only the replies to its own requests out of the MIDI output. The main panel is
  never affected: no panel messages, no change to its selection; with the editor closed nothing is sent.
  Editing (docs/editor/EDITOR_ROUND2.md): a field is editable only if its write path was verified live (oscillator
  NRPN, the wave ID by SysEx, sound settings / system partial writes with read-modify-write for bit fields, COMMON NRPN
  MSB 1); everything else is display only. ARP settings change only through the panel's own buttons, as hands do
  (docs/editor/ROUND2_REVIEW.md): the control's button steered by the OS's arp-control variable, then -/No / +/Yes,
  each press only right after the OS's panel mode (0x0BE01A8B) read 0x0C (the arp menu) AND the arp-control variable
  read the wanted control, else the edit stops without pressing (the variable keeps its value outside the arp menu;
  there -/No / +/Yes step the main preset); any panel input or other MIDI during an edit stops it at once (nothing more
  is pressed). Arp edits start only in panel mode 0x01 (normal) or 0x0C (arp menu); in any other mode nothing is pressed
  and the user is told. A run begun in normal mode ends with the preset bank button (0x19) pressed once, only while the
  mode reads 0x0C, back to 0x01 (in normal mode that button switches the bank); begun in the arp menu, it stays there.
  Never a knob, never a write into the arp state, never a whole-preset dump (it stops a running arp). INSERT and
  REVERB (docs/editor/EFFECTS_EDITOR_SPEC.md, ROUND3_ANSWERS.md): effect values by a raw-memory write of the word into
  the edit buffer, then the OS's effect parameter message; never a partial write of the preset for them and never NRPN
  LSB 34/35; the algorithm by NRPN LSB 30, 31, then 32 = 0; the insert modulation as one 10-byte partial write
  (offset 64) per gesture; after that write or NRPN LSB 30-33 every effect send waits 350 ms (the OS rebuilds its
  effects); under a variation the slot shows the values that play, and the first edit of one in the editor copies the
  variation to PSEt (write path 4b: raw writes, PSEt only once the read-back equals the copy). COMMON
  and ARP fields (no OS table) are in `editor_fields.*`; ARP values come from the live arp state and the OS's arp
  variables in RAM. ARP page (v0.8.3.5, docs/editor/ARP_ROUND.md): the owner-approved order (the Arp Mode button's);
  arp presets by the OS's own number (from 0); their list ends where the OS stops answering name reads (never a count
  in the code); PATTERN / RIFF (types 9 / 11) display only, FEEL with its type-10 name. Follow also jumps to the page of the panel knob last moved (`Engine::lastPanelControl`).
  The skin side: `<name>-editor.rml`, `<vfdrow>` (values take drags, double-click, right-click), `<legend>`, the Lua
  `editor` API with `editor.knobLabels` (docs/SKINS.md).
  LIBRARY (v0.8.3 step 2; the plugin window's LIBRARY tab since v0.9.0; docs/editor/LIBRARY.md,
  `phyzo-emu/src/library/`): `.syx` files in `~/Documents/Phyzo/Library/`. The editor unit has no LIBRARY area.
  - A clicked preset's stored single-preset message goes into the running machine's MIDI input
    (`Engine::libraryInput`; the OS puts it in Compare; Save on the panel keeps it).
  - Files are read by hidden machines on a worker thread, one job at a time in the process. Fresh machines boot the
    v0.8.3 way; clones restore the running machine's snapshot, which is never paused, pressed or read otherwise.
    Every clone job first waits for `P nn` (`waitForPresetDisplay`): `donE` is waited out; any other display (an
    effect, reverb or tempo page) gets Bank 0x19 pressed once in the clone, never on `P nn` (it switches banks there),
    never while the panel mode (0x0BE01A8B) reads 0x01, never twice.
  - The machines are driven only through the OS: panel Dump 0x0C and +/Yes 0x0D, program changes, SysEx in, name
    reads (#0, 0x40 + n).
  - Index cache: `cache/library/<file MD5>.idx` (so Rename reads nothing again). Factory bank files:
    `Library/Factory/`, from the user's OS, never shipped. Groups: FACTORY, BANKS (`folder/name`), SINGLES, BACKUPS.
  - Save with a name (rack SAVE): refused while the Compare LED (0x16) is lit; a clone's draft (P nn, the OS's name,
    the dump), then the typed name made safe (`_`), empty = the OS's name, never overwriting (` 2`). Rename / Delete
    (to the Trash) for library files only, never the factory files.
  - Bank load into Bank 1 / 2 (the only write to the synth's memory): a clone backs the bank up to
    `Library/Backups/Bank N YYYY-MM-DD HHMM.syx` first (no backup, no load; Cancel only then), then the file's bank
    message with byte 6 = the target and nothing else changed, then done when the bank's 32 stored names (a clone)
    equal the index's (30 s after the OS read it all). One bank job at a time; last-loaded names in project state.
- Real-time rules for `engine.*`: the audio thread never waits on other threads. `getState` asks the audio thread
  for a snapshot at a block boundary (`Machine::saveStateInto`, preallocated); panel controls go through a
  lock-free latest-value slot per cc, sent at most once per 10 ms each and only while the panel link keeps up (the
  boot positions are per-control atomics); buttons go through a bounded lock-free multi-producer queue and are never
  merged or dropped. setControl and pressRaw are lock-free and allocation-free from any thread, the audio thread
  included. `phyzo_stress` measures this. The window's library messages (`Engine::libraryInput`) go through a
  preallocated ring and are queued whole only when the MIDI port's input queue (`Serial::kMidiRxRoom`, made
  beforehand) has room: no allocation, no waiting.
- Idle runner (v0.8.6, F-63, `Engine::setIdleRun`): off by default; the plugin turns it on only while its window is
  open, the host renders in real time and prepareToPlay has run. After 250 ms without a host block a background thread
  runs the machine in real time (panel, virtual port, editor; audio discarded, Audio In silent, nothing to the host) in
  slices of at most 10 ms of machine time under `mtx_` taken with try_lock only; process() keeps its lock and sets
  lastProcessMs_ before taking it; the next host block retimes; never while booting, restoring or stopped.
- Sync to host tempo (v0.8.7, F-68, `phyzo-emu/src/midi_clock.*`): the OS follows MIDI clock (24 F8 a quarter; it
  ignores Start/Stop/Continue/SPP), so the plugin sends only F8, on the host's grid (ppq = k/24) from the playhead,
  merged after the host's events at the same offset in processBlock (after MIDI learn); modes Off (events untouched,
  identical to v0.8.6), While playing (default), Always (free-running at the bpm while stopped); no bpm: nothing; the
  host's own F8 is dropped unless Off; a plugin setting in processor state version 7 (older: While playing), never a
  parameter; the window's MIDI tab. F8 does not count as other MIDI for the editor's arp edits.
- MIDI learn (v0.8.4, F-55, docs/MIDI_LEARN.md, `phyzo-emu/src/midi_learn.*`): a controller is a second front panel.
  Learned CCs (notes for buttons) from the host's MIDI drive panel controls (knobs cc 0-22, panel buttons by raw; never
  cc 23-25 or the keys) through setControl / pressRaw exactly as the mouse, and are swallowed; unbound MIDI passes
  unchanged; 14-bit pairs (CC n + 32), buttons need both edges. Bindings: tables of atomics read by the audio thread
  (no lock, no allocation), saved in processor state version 6 (and later), defaults in Phyzo.settings (`midiLearnDefaults`);
  never synth or host parameters. The virtual MIDI port is not affected.
- Panel analog controls (26, `Bx cc vv`, raw 0-1023): `phyzo-emu/docs/PANEL_CONTROLS.md`. Their positions are the
  panel's physical state (`PanelModel::controls`, saved with the machine), never host parameters. The pitch wheel
  must be answered at 512 to the OS's F4 request.
