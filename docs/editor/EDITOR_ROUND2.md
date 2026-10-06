# Editor round 2 — editing, Common and Arp pages, Follow on knobs, window fits the screen (F-30, F-32), 2026-10-06 (rev 2)

Rev 2: the live arp state, loading arp presets, the arp pattern field, the dump layout, knob labels, Common page 3
labels. Mockups as approved in the Claude Code thread.

Name-free. Every write path below was tested live in the project chat on the real engine with the real ROMs (Template
Preset unless noted), against a no-edit control, with read-back. Rule for this round: **a field is editable only if its
write path is listed here as verified; every other field stays display-only.** No parameters are added or removed: all
fields are the OS's own; the labels below are neutral descriptions.

## 1. Write paths

| Area / fields | Path | Evidence |
|---|---|---|
| Oscillator, single-byte fields (value types 0, 1, 3) | **NRPN**: MSB = 2 + sound·4 + osc·2 + (field >> 7), LSB = field & 0x7F (field = table index), value 14-bit two's complement, all four controllers then the null | cutoff 0 / 30 audible; −50 reads back exactly; a bit field (PitchBend Enable) changes only its own bit (0xFF → 0xFE → 0xFF) |
| Oscillator wave (field 51, value type 4) | **SysEx partial write of the 4-byte wave ID** at oscillator offset 0x4C; the ID for wave n comes from the OS's RAM wave list (PR_B_ANSWERS §7) | sounds the same as the panel's Wave Select to the same wave (centroid 709 Hz both; 4335 Hz before) |
| **Never** the wave field's NRPN | — | it writes the value as a raw word into the ID (wave 13 by NRPN gave ID 000D1436, a corrupt wave) |
| Sound settings (track records) | **SysEx partial write** (type 2, number 0x3FFF, sub-type 1, sub-number = sound) | track volume −16.7 dB; value type 2 stored as shown + 64 |
| System | **SysEx partial write** (type 0) | fine tune audible (+18.9 cents for 49); NRPN-transmit bit takes effect |
| Bit fields written by SysEx (receive flags, system flags) | **read-modify-write the whole byte**: read it, change the field's bit, write the byte | the OS stores several flags per byte (system byte 25 holds 0x5F with the NRPN bit clear) |
| Common: sound slots and knob values | **NRPN MSB 1** (the OS's validated path), LSB as in §2 | slot 1 set to User mode with top key B3: middle C silent, A3 sounds (live) |
| Arp fields | **whole-preset dump into the edit buffer** (layout in §1a), built from a fresh read of the edit buffer **with the live arp state folded into its arp block** (§3) | arp on: the OS lights the arp LED and the held chord arpeggiates; a dump while notes sound changes nothing audible (level within ±0.1 dB); after a panel Sound 2 selection, a dump built from a fresh read keeps the panel's display, selection and LEDs (only the arp LEDs change) and the panel knobs still edit Sound 2 |
| Load arp preset n (right-click list on the arp page) | the same dump, with the arp block's body replaced by arp preset n's record body (SysEx read, type 8, number n, record offsets 24–87), **keeping** live state bytes $1 (held), $2 (keyboard) and $14–$17 (both key-range pairs) | the resulting live arp state is identical, all 64 bytes, to selecting the same arp preset on the panel (PrSt) |
| Presets page: click a name | program change (0–63) on the base channel | as the host's program changes |

**Whole-preset dump rules** (arp fields): before each dump, read the current edit buffer (command 01, whole object) and
the live arp state (§3), copy the live state into the preset's arp block (preset offsets 350–413), then change only the
edited field. Panel edits made meanwhile are therefore kept (the panel writes the sound selection and the performance
knobs straight into the edit buffer, verified; its arp edits go to the live arp state, which is folded in). At most one
dump in flight; while dragging, coalesce to the latest value and send it when the previous dump has been answered
(~350 ms round trip at cable speed).

### 1a. Whole-object dump layout (from the OS's own 0x41 reply to a whole-object read; sending it with command 02 verified)

`F0 0F 11 01 <id> <cmd> <type> <number: 14-bit> <length: 32-bit> <data> <checksum: 14-bit> F7` — the reply has id 00 and
cmd 0x41; send cmd 02 with id 7F. Numbers 7 bits per byte, low first. Data: the object as big-endian 32-bit words, each
encoded like partial writes (5 bytes), last word zero-padded; the object starts with its own size word and its tag word.
Checksum: sum of the encoded data bytes & 0x3FFF. Example (edit buffer, 537 bytes): first 30
`F0 0F 11 01 00 41 07 00 00 1E 03 00 00 00 1E 03 00 00 00 31 28 4D 02 05 70 5A 15 23 05 65`, last 10
`04 00 00 00 00 00 00 6B 47 F7`. No sub-type field.

**Display-only this round** (no verified live path or format yet): sound common fields (pitch table, bend, alt bus,
GM alias, restrike), insert modulation (Common page 3), arp feel, the unidentified arp byte 366 (not shown), Insert and
Reverb pages (V-15), the global reverb mix (V-14, open: preset writes are accepted but the effect is not measurable on
the Template), the oscillator trigger-key field (encoding undecoded).

## 2. Common page fields (preset object, type 7, number 0; plain numbers, our measurements)

Sound slot n (0–3), base B = 28 + 8·n, NRPN MSB 1 LSB base L = 2 + 7·n:

| Field | Offset | Size | Range / values (neutral labels) | NRPN LSB |
|---|---|---|---|---|
| key range low (applies in User mode) | B + 0 | 1 | 0–127, note name | L |
| key range high (applies in User mode) | B + 1 | 1 | 0–127, note name | L + 1 |
| mode | B + 2 | 1 | 0 OFF, 1 LAYER, 2 SPLIT, 3 USER | L + 2 |
| oscillators | B + 3 | 1 | 0 NONE, 1 OSC 1, 2 OSC 2, 3 OSC 1+2 | L + 3 |

Performance-knob values (labels are the skin's P H Y Z O, perf1–perf5):

| Knob | Offset | Range | NRPN LSB |
|---|---|---|---|
| P (perf1) | 34 | 0–127 | 7 |
| H (perf2) | 35 | 0–127 | 8 |
| Y (perf3) | 42 | 0–127 | 14 |
| Z (perf4) | 43 | 0–127 | 15 |
| O (perf5) | 50 | 0–127 | 21 |

Page layout: page 1 a slot table (one row per sound: mode, oscillators, key range), page 2 the five knob values, page 3
insert modulation (display-only: MOD SOURCE 64, MOD DEST 65, SRC MIN 66, SRC MAX 68, DEST MIN 70, DEST MAX 72; the
last four 16-bit words).

## 3. Arp page fields

**Source of the values: the live arp state**, not the preset's stored block. The panel's arp edits (including selecting
an arp preset) change only the live state; the preset's block changes when the preset is saved (verified: selecting arp
preset 23 on the panel left the preset block unchanged). The live state is 64 bytes in RAM at **0x0BE093D0** (read with
the raw-memory SysEx read, type 0x7F, like the wave list), in the layout of an arp record's body: live byte $N = arp
record offset N + 24 = preset offset 350 + N. With no panel arp edits it equals the preset block (verified). Offsets
below are preset offsets; the live byte is offset − 350.

Labels are the OS's own 4-character codes, read at runtime from the addresses in PR_B_ANSWERS §4.

| Code (address) | Offset | Size | Stored values (all 64 factory presets checked) | Shown as |
|---|---|---|---|---|
| on / off (0x8882E / 0x88834 values) | 383 | 1 | 0 off, 2 on | OFF / ON |
| HELd (0x88850) | 351 | 1 | 0 / 1 | OFF / ON |
| keyboard (arp bypass) | 352 | 1 | 0 = keyboard on, 1 = off | ON / OFF |
| -3rd 3rd 4th 5th Oct (0x88828 …) | 361–365 | 1 each | 0 / 1, at most one on: setting one clears the others | OFF / ON |
| Oct# range up (0x88818) | 367 | 1 | 0, 12, 24, 36, 48 | `Oct%d` with value / 12 |
| range down | 368 | 1 signed | 0, −12, −24, −36, −48 | `Oct%d` with −value / 12 |
| Lo n / Hi n (0x8885C / 0x88868) | 370 / 371 (input filter) and 372 / 373 (output bounds) | 1 each | 0–127 | note name (`%s%d`); an edit writes both pairs, as the panel does |
| CEnt (0x88856) | 382 | 1 | 0 / 1 | OFF / ON |
| Tch (0x88898) | 384 | 1 | 0 = off, 1–127 | OFF / number |
| ndur (0x8886E) | 385 | 1 | 1–255 | shown = raw × 100 / 256 (the OS's own formula, traced); editing shown d stores the smallest raw that shows d |
| ntLt (0x8887C) | 386 | 1 | 0–99 | number |
| FEEL (0x88892) | 392 | 2 | 0xFFFF off, else a number | display-only (formatting not decoded) |
| nrES (0x8881E) | 402 | 4 | 1536, 768, 384, 192, 96, 48, 32, 64, 128, 256, 512, 1024 | the OS's names in its pointer-table order (0x8876C): whole, half, quarter, 8th, 16th, 32nd, then the triplets 32nd … whole. 384 ticks per quarter (factory presets: 96 = 16th in 58 of 64) |
| t### tempo (0x88812) | 406 | 4 | 25–320 | `t%3d` |
| arp pattern | 390 | 2 | arp pattern number, 0xFFFF none | display-only: the pattern's name (SysEx read, type 9, number n, offset 8, 16 characters) |
| PrSt (0x88882) | — | — | action, not stored | shows the OS's " Snd" (0x88888: the preset's own arp settings); right-click lists the 121 arp presets by number and name (`AP%02d`, 0x8889E; names: type 8, number n, offset 8) and loads the chosen one as in §1 |
| arp settings name | 334 | 16 | text | display-only |

## 4. Follow on panel knobs

The engine reports the last panel control moved (its control number, 0–25) alongside the existing gesture counter. With
Follow on, the editor shows the page holding that control's field and focuses it:

| Control (cc) | Goes to |
|---|---|
| 10 Wave Select / 7 Wave Amount | OSC WAVE: field 51 / 58 |
| 20 Pitch Tune / 18 Pitch Amount / 19 Glide Time | OSC PITCH: 14 / 27 / 18 |
| 22 Cutoff / 8 Resonance | OSC FILTER: 32 / 33 |
| 4 Filter Amount | OSC FILTER: 34; 37 while the Filter Keyboard LED (0x08) is lit |
| 2 Level / 1 Pan | OSC AMP: 12 / 13 |
| 0 Mod Speed | OSC LFO 128 while the LFO LED (0x0F) is lit; OSC NOISE 70 while the Noise LED (0x02) is lit |
| 17 Attack / 16 Decay / 12 Sustain / 21 Release | the envelope page of the lit Envelope LED (Pitch 0x11 → ENV1, Filter 0x0C → ENV2, Amp 0x0E → ENV3): fields 79 / 85 / 86 / 87, + 16 for ENV2, + 32 for ENV3 |
| 9, 13, 6, 5, 3 performance knobs P H Y Z O | COMMON page 2, the matching knob value |
| 14 Arp Value | ARP page |
| 11 Volume, 15 Mix, 23–25 wheels / pressure | no jump |

The LED codes come from the skin (`editor.followLeds`), extended with `noise`, `lfo` and `filterKbd`.

Knob labels: `editor.knobLabels{"P", "H", "Y", "Z", "O"}` — exactly 5 strings in perf1–perf5 order (perf1 = control 9,
perf2 = 13, perf3 = 6, perf4 = 5, perf5 = 3), at most 4 characters each, callable from the editor unit's onload like
`editor.followLeds`; without it the labels are PERF1–PERF5. Document it in docs/SKINS.md and use it in the test skin.
The rack skin is not in the repo: the project chat adds the call to the rack skin; don't add or change rack skin files.

## 5. Editing (as in EDITOR_SPEC.md, now implemented)

- Click and hold a value and drag up/down: 1 step per 4 dp, Shift fine. Values are sent while dragging (NRPN paced like
  panel knob moves; dump-based fields coalesced as in §1) and the final value on release. The mouse wheel never changes
  values.
- Double-click: typed entry (Enter sends, Esc cancels). Right-click: the OS's value or name list (waves by number and
  name; on the arp page's PrSt field, the 121 arp presets), and revert to the stored value.
- Every edit: clamp to the field's range, send, read back, show what the OS holds. Display-only fields ignore drags.
- Exclusive arp intervals: setting one clears the others in the same dump.

## 6. F-32 — the window fits the screen

Limit the window to the usable area of the display it is on: Zoom menu entries that don't fit are greyed out, the
corner drag stops there, and opening the editor at a zoom that doesn't fit steps down to the largest that does (the
remembered zoom is unchanged otherwise). On a 1080-pt display the editor fits up to ~105–110 %.

## 7. Tests

- CI (no ROMs): encoders for NRPN (signed values), partial writes, whole-object dumps with checksum; read-modify-write of
  bit fields; dump coalescing; exclusive intervals; the knob map; editing gestures on synthetic tables; window limits.
- Editor closed: engine output bit-identical to v0.6.0 (and so to v0.5.3).
- Project chat (real ROMs) before merging: each write path changes the sound against a no-edit control and reads back,
  as in §1; dragging a value sends nothing for display-only fields; the panel is not affected.
