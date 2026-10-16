# MIDI learn (v0.8.4, F-55)

Play the panel from a hardware controller. A controller is a second front panel: a learned CC (or note, for a button)
produces exactly what the panel produces for that control, the same panel message to the OS through the same path as
the mouse. Learning changes only how a control is driven, never what it does. There are no new synth parameters and
no host automation parameters, and a project without bindings behaves exactly as before.

## What can be learned

- **Knobs:** the panel's analog controls cc 0–22 (`phyzo-emu/docs/PANEL_CONTROLS.md`). A binding belongs to the panel
  control (its cc), not to a skin element. Stacked knobs share a cc: they are one physical control, learned once, from
  any of their elements.
- **Buttons:** every panel button by its panel id (`<pbutton raw="…">`). The Built-in skin's −/No and +/Yes are the
  same panel controls as the rack skin's (raw 0x0E and 0x0D), so they share bindings. They can be learned when the
  running OS gives its −/No and +/Yes buttons those ids (then the Built-in skin's button and the learned one send the
  OS the same message); the plugin checks this against the loaded OS.
- **Never:** the pitch wheel, mod wheel and pressure (cc 23–25) and the keys. They stay plain MIDI, as on the
  hardware.

Names: the perf knobs by the skin's knob letters (`editor.knobLabels`, docs/SKINS.md), e.g. `X (Perf 1)` where X is the
skin's letter for perf1 (`Perf 1` … `Perf 5` without a letter); the other knobs by their names in PANEL_CONTROLS.md
(Cutoff, Resonance, Attack …); the buttons by their panel names (Save, Compare, Arp Mode …), or `Button 0xNN`.

## The menu (approved mockup)

Right-click on a learnable control opens this menu (v0.9.0: right-click anywhere else opens the plugin window, and an
editor-unit value shows its Revert when it was edited):

```
X (Perf 1) · CC 21, ch 1      the control and its binding (or "not learned"); "(14-bit)" once an LSB was seen
MIDI learn                    arms learn for this control (again, or Esc: cancel)
Set CC              ›         CC 0–31 › / CC 32–63 › / CC 64–95 › / CC 96–119 ›   (two clicks, no typing)
Channel             ›         Any / 1–16 (when bound)
Forget CC 21                  only when bound
───────────
Open window…                  the plugin window (SYSTEM)
```

- **Set CC:** the current CC is ticked. A CC the OS itself answers is labelled: CC 1 (OS: mod wheel), CC 12 (perf1,
  SYS3), CC 71 (perf2), CC 74 (perf3), CC 78 (perf5, SYS4), CC 79 (perf4). These are the OS's factory MIDI settings:
  the System Controller numbers are not read from the OS (the editor link does not offer them), so a System Controller
  3 or 4 changed in MIDI Edit is not shown here. A CC another control holds shows it (`CC 22 → Cutoff`). CC 120–127
  (channel mode messages) are never offered or learned. Set CC keeps the binding's channel (Any if it had none).
- **The plugin window's MIDI tab** (v0.9.0) lists every binding: control, MIDI (`CC 21`, `Note 60`, `(14-bit)`),
  channel (`Any` or 1–16) and **Forget** per row; then **Save as default**, **Clear default** (greyed out when none is
  saved) and **Forget all** (greyed out without bindings), and the hint "right-click a knob or button to learn it".

## Learn

- Arming shows `LEARN X  MOVE A CONTROL...`. The first eligible message on the host's MIDI input binds:
  - a knob: a CC 0–119 on any channel;
  - a button: a CC 0–119 or a note on/off (Learn only: Set CC offers CCs).
- Learn records the channel it heard (Channel can then be set to Any). The message that completes learn is swallowed;
  other messages pass normally while learn waits (keys still play while a knob is learned).
- One binding per control, and one control per (channel, CC) or (channel, note). Binding a CC another control holds
  moves it: `CC 22 MOVED  O -> X`. A binding on Any takes that CC on every channel; it replaces a binding of the same
  CC on a single channel, and the other way round.
- Arming another control, MIDI learn again or Esc cancels. With no learn armed, Esc releases held buttons as before.

## What a bound message does

- **Swallowed:** a message a binding takes is removed from the host's MIDI before the engine sees it; it never
  reaches the OS. Everything unbound passes exactly as today. (CC 74 learned to a perf knob moves that knob only; the
  OS's own CC 74 path no longer sees it.)
- **Knobs are absolute,** like the real pots, with no pickup: 7-bit value v → raw = round(v × 1023 / 127) (0, 64, 127 →
  0, 516, 1023).
- **14-bit:** for a knob bound to CC n with n 0–31, CC n+32 on the same channel is its LSB (unless n+32 is bound to a
  control itself: that binding wins). MSB then LSB, the standard way: once an LSB has been seen, the MSB sets the coarse
  value (LSB = 0) and the LSB refines it: raw = round(v14 × 1023 / 16383) (0, 8192, 16383 → 0, 512, 1023). The LSB is
  swallowed too, and only when paired. The menu then shows `CC 21 (14-bit)`; the flag is saved with the binding.
- **The panel path:** a learned knob goes through Engine::setControl exactly like the mouse: the same pacing (one
  message per control per 10 ms, the latest value wins, nothing while the panel link is backed up), the position kept
  (the OS's F4 answer at boot). The visible knob(s) of that cc move as Lua's `panel.setKnob` moves them, and their
  positions are saved with the project as today.
- **Buttons need both edges:** a CC value ≥ 64 presses, < 64 releases; a note on with velocity > 0 presses, a note off
  (or velocity 0) releases. A repeated press with no release between (or a repeated release) is ignored, never a
  double press. Held combinations arrive exactly as sent (Save + Compare, Arp Mode + Value, Sound + keys …), through
  Engine::pressRaw exactly like the skin's buttons.
- **One-edge warning:** after a button is learned (or set), if no release comes within 2 s of its first press, or a
  second press comes with no release between, `SAVE = CC 80  ONE EDGE ONLY: SET BUTTON MOMENTARY` shows (the channel is
  left out when the line would pass 50 columns). The binding stays.
- **Stuck buttons:** forgetting or moving a binding, Forget all, loading a project, or closing the plugin while a
  learned button is held sends its release first.

## Messages

Upper case, at most 50 columns: `LEARN X  MOVE A CONTROL...`, `X = CC 21 CH 1`, `CC 22 MOVED  O -> X`,
`X: CC 21 FORGOTTEN`, the one-edge warning. While the plugin window is open they show in its footer; otherwise in the
skin's message banner for 3 s.

## Saving

- **Project:** processor state version 6 appends the bindings (control id, channel (0 = Any), CC or note number, kind,
  14-bit seen). A version-6 project with an empty list has no bindings. Projects from versions 1–5 load as before and
  then get the default bindings.
- **Defaults:** a new instance (or an older project) starts with the default bindings, from `Phyzo.settings` (key
  `midiLearnDefaults`, compact text such as `k9:1:c21:f b0B:0:n60`): empty unless saved with Save as default for new
  instances; Clear default removes the key.
- Bindings are plugin settings: never synth parameters, never host parameters.

## Threads

- The audio thread never locks and never allocates for a bound message. The bindings live on the message side and are
  mirrored into fixed tables of atomics ((Any + 16 channels) × 128 for CCs and for notes); learn is armed through an
  atomic, and what the audio thread hears (a learned message, a one-edge button) goes to the message thread through a
  lock-free single-producer queue. Until the message thread commits a learned message, messages from the same source
  already act on the control.
- Engine::setControl and Engine::pressRaw are lock-free and allocation-free from any thread: the knob positions for
  the next boot are per-control atomics, and the button queue is a bounded lock-free multi-producer queue (the
  message thread, the skin's render thread and the audio thread may all press buttons).

## Not in this version

Controller maps (save or load a controller's bindings as a file), LED feedback to a controller, pickup mode (F-48),
CC remap. The virtual MIDI port is not affected: bindings apply only to MIDI from the host; the virtual port stays a
plain editor/librarian link.

Code: `phyzo-emu/src/midi_learn.*` (no JUCE; tests: `midi_learn_test`), the plugin's menu and state in `plugin/Source/`
(test: `midi_learn_plugin_test`).
