# Editor screens (F-30)

Name-free design documents for the editor unit, from the project chat:

- `EDITOR_SPEC.md`: the specification (revision 4).
- `EDITOR_PANEL_LAYOUT.md` / `.json`: the editor unit's geometry and every legend's click box and action. Where it
  differs from the specification, the layout is authoritative: the unit is 1795 x 394 dp and the window grows from
  848 to 1242 dp (the specification's "340 dp / 1188" line is out of date).
- `PR_B_ANSWERS.md`: the OS table layouts (descriptors, field records, value lists, the system table, the arp strings,
  the RAM wave list), the LED codes the rack skin passes to Follow, and the scope of step 2.
- `PR17_REVIEW.md`: the review of step 2 with the real OS (oscillator sections by field index and other fixes).
- `EDITOR_ROUND2.md` (revision 6): round 2: editing with the write paths verified live, the COMMON and ARP pages, Follow
  on panel knobs, the window fitting the screen (F-32).
- `ROUND2_REVIEW.md`: the review of round 2 with the real OS: every arp setting through the panel's own buttons, and
  the Presets page marking the current preset.
- `EFFECTS_EDITOR_SPEC.md` and `ROUND3_ANSWERS.md` (round 3): the INSERT and REVERB pages, the effect tables' layouts
  and the verified write paths.
- `IMPLEMENTATION_STEPS.md`: the plan.

## What is implemented

- `phyzo-emu/src/editor/`: messages (`editor_proto`: partial reads and writes, whole reads and dumps, NRPN, program
  changes), the OS tables and arp texts read from the user's OS at run time (`editor_tables`), the preset object's
  COMMON and ARP fields (`editor_fields`), pages (`editor_pages`) and the controller (`editor_controller`): areas
  SYSTEM, PRESETS, COMMON, SOUND, OSC, ARP, INSERT and REVERB.
- INSERT (ALGO, MOD, SENDS) and REVERB (ALGO, SENDS), round 3 (EFFECTS_EDITOR_SPEC.md): names, ranges, lists, units,
  the edit order, the variations and the panel's codes read from the user's OS image at run time. Effect values: a
  raw-memory write of the word into the edit buffer, then the OS's effect parameter message (clamped to the record's
  range; never a partial write of the preset, never NRPN LSB 34/35); the algorithm by NRPN MSB 1 LSB 30, 31, then
  32 = 0; the variations by LSB 32 / 33 (PSEt only where the panel offers it); the insert modulation as one 10-byte
  partial write of the edit buffer (offset 64) per gesture. After that write or NRPN LSB 30-33 the OS rebuilds its
  effects: every effect send waits 350 ms (the latest queued value goes then) and the read-back follows. Under a
  variation the slot's values are the ones that play (read from the running performance); the first edit of one of
  them in the editor copies the variation to PSEt (write path 4b: what plays raw-written into the edit buffer, PSEt
  only once the read-back equals the copy, one rewrite after 1 s, else a notice and the variation stays), then the
  edit follows after the rebuild hold. The main panel keeps the OS's own behaviour.
  The insert modulation moved here from COMMON.
- Editing (round 2): only fields with a write path verified live are editable (EDITOR_ROUND2.md section 1), every other
  field is display only. Drag a value (1 step per 4 dp, Shift fine), double-click for typed entry, right-click for the
  OS's list and revert, arrows and +/-; every edit is clamped, shown at once, sent (paced like panel knob moves, the
  latest value winning) and read back. ARP settings change only through the panel's own buttons, as hands on the
  panel do (ROUND2_REVIEW.md): the arp control's button, steered by the OS's arp-control variable, then -/No and +/Yes,
  each press only right after the OS's panel mode read the arp menu (0x0C) and the variable read the control, else the
  edit stops without pressing; any panel input or other MIDI during an edit stops it (nothing more pressed); edits start
  only in panel mode 0x01 or 0x0C (otherwise nothing is pressed and the user is told); a run begun in 0x01 returns there
  with the preset bank button, pressed once only while the mode reads 0x0C; arp on/off and keyboard by their buttons. No writes into the arp state, no whole-preset
  dump (the encoders stay, tested, unused). PRESETS: a click loads the preset; the current preset is marked.
- `Engine` (`setEditorOpen`, `editorSend`, `setEditorStandingRead`, `editorReplies`, `lastPanelControl`): the editor's
  messages enter the OS's MIDI input as whole messages at cable speed; the read of the visible page goes in directly
  behind every program change, bank select and SysEx dump; replies to the editor's own requests are taken out of the
  MIDI output, all other output passes. Nothing at all while the editor is closed.
- The skin runtime's `<vfdrow>` and `<legend>` elements, the Lua `editor` API (including `editor.knobLabels`) and
  `<name>-editor.rml` (docs/SKINS.md).
- The plugin: right-click > Editor, the window growing by the unit's height and never beyond the screen, the open state
  saved with the project, the value lists as right-click menus, the keys.

`editor_tables <OS image or update file> [oscillator|sound|common|system|arp|effects]` (built with phyzo-emu) prints the
tables, the arp texts and the effect tables as decoded, to check them against the hardware; it writes nothing.

To confirm with the real OS (project chat): the arp texts' addresses (on/off strings, codes, the formats and the
resolution names); the arp preset numbers' format (`AP%02d`, shown here from AP01); the base channel used for NRPN and
program changes (channel 1 for now); the insert modulation ranges shown as signed 16-bit numbers; the keyboard flag's
sense (shown ON when the OS's flag is set); the current preset, found as the stored preset whose name the edit buffer
holds (the last one picked here if names repeat) until the OS's own variable for it is known; the arp tempo shown from
the live arp state ($38-$3B) until the variable the Tempo control edits is confirmed (AtPO).
