# Editor screens — specification (F-30), revision 4, 2026-10-05

Neutral document: no original product or manufacturer names (hard rule). Message formats were traced in the OS and
verified on the running OS in the emulator; the private results report keeps the evidence. Mockups approved in general
by the owner on 2026-10-05 (with the edits in this revision); RML waits for the next approval step (rule 3). No
parameters are added or removed: the editor exposes the OS's own fields.

## Hard requirement: the main panel is not affected
- The panel (its art, layout, 4-character display and every control) keeps its current behaviour. The editor adds no
  interaction to it and never sends panel messages, presses panel buttons or moves panel knobs.
- The editor only observes the panel's LED and display messages (as the skin does) and the OS's MIDI output.
- The editor's Sound/OSC choice is its own; it never changes the panel's selection. Verified: editor NRPN and SysEx
  writes leave the OS's panel selection, display and LEDs unchanged.
- With the editor closed, nothing is sent: plugin output stays byte-identical to the previous version.

## Goal
VFD-style editor screens, separate from the main panel, giving access to every parameter the OS supports. Parameters
that also have a panel control appear here too. The editor talks to the OS the way an external editor does (MIDI
parameter messages and SysEx), so the OS itself holds and applies every edit, exactly as on hardware.

## User interface
- **Opening:** the right-click menu item "Editor" (not by clicking the panel). It opens as a second rack unit attached
  below the panel; the window grows by 340 dp (848 → 1188 dp at 100 %), resizing keeps the proportions (F-24). Closing
  (the same menu item) restores the original size. Open/closed state remembered per project. The editor unit is
  1795 × 394 dp; exact positions in the layout table delivered with the skin art.
- **Layout:** the glass alone, spanning the face. No buttons: every control is a legend on the glass.
- **Glass:** 4 rows × 50 character cells (two halves of 25) of a 14-segment display, with three rows of fixed legends above (areas;
  oscillator sections and S1–S4 / O1 / O2; other sections and PAGE 1–9; plus FOLLOW, EDITED, DATA). Legends are always
  drawn ghosted and lit when active. Each cell has a decimal point and an underline bar, both ghosted when off.
- **Legends are controls:** row 1 selects an area, rows 2–3 a section, S1–S4 / O1 / O2 the sound and oscillator the editor
  shows and edits, PAGE 1–9 a page, PREV / NEXT (centred in the top row) the previous / next page and are lit only when
  that page exists, FOLLOW toggles Follow; EDITED and DATA are indicators. Hover brightens a legend.
  Each legend has a click box (layout table).
- **Value marking:** every parameter value has its underline lit from its first to its last character; names are never
  underlined (lists to pick from, such as preset names, aren't either). The focused value blinks (~2.5 Hz) with its
  underline staying lit.
- **Skin elements needed:** a text cell row element drawing characters from a character sprite sheet
  (frame = ASCII code) and clickable legend elements with lit/unlit/hover classes. All art stays in
  the skin folder.

## Page generation (from the OS's tables, read at runtime from the user's OS image)
| Page type | Rule |
|---|---|
| Two-column list | fields in the OS table order, two per row (name left, value right in a 25-cell half); a run of identically named fields becomes two columns numbered 1 and 2; rows whose values don't fit two halves flow across the row |
| Stage table | envelope fields named "… Time (n)" and "… Level (n)": stages across, time and level down |
| Slot table | one row per sound slot of the preset |
| Code grid | the OS's 4-character codes and value formats (system page, arpeggiator), four per row |
| Name list | preset names, 8 per view |

- **Hidden fields:** names containing SPARE, the wave checksum, "Old Keybd Track", "Use Layer".
- **Name shortening** (only when a name doesn't fit): split at word and camel-case boundaries, apply a fixed generic word
  list (CUTOFF→CUT, KEYBD→KBD, TRACK→TRK, BREAKPOINT→BRKPT, RESONANCE→RES, AMOUNT→AMT, MODULATION→MOD, VELOCITY→VEL,
  ATTACK→ATK, RELEASE→REL, LEVEL→LVL, SCALING→SCALE, SEMITONE→SEMI, TUNING→TUNE, PRESSURE→PRESS, VOLUME→VOL,
  DELAY→DLY, ENVELOPE→ENV, FEEDBACK→FDBK, DAMPING→DAMP, SOURCE→SRC), then join word pairs from the end one at a time,
  then truncate, keeping a trailing column number. Both halves of a row get the same room.
- **Values:** the OS value lists and print formats; signed where the range is signed; fields centred on 64 shown signed.
- **Page map:** System 2 pages; Presets 8; Common 3 (sound slots, the five performance-knob values labelled by the skin,
  insert modulation); Sound 9 per sound (55 track fields + 12 sound-common fields); Oscillator ≈ 20 per oscillator
  (wave, pitch, filter, amp, noise, LFO, envelopes 1–3, general); Arp 2; Insert 1 + per-algorithm parameters; Reverb 2.

### OS tables (addresses in the OS image, loading at 0x4000; read at runtime, never copied into the repo)
Full layouts (descriptors, field records, list encoding, system table, arp strings, the RAM wave list): `PR_B_ANSWERS.md`,
which supersedes the address list below where they differ.
| Table | Address |
|---|---|
| Oscillator field names (138 pointers) and offsets | 0x7338C, offsets 0x735BC |
| Sound-in-preset field names (55) and offsets | 0x720DC |
| Sound common field names (12) and offsets | 0x73218 |
| Value lists (entries separated by CR; bytes 0x11–0x1F copy that many characters from the previous entry, 0x10 copies the rest and ends the entry; a 0x80 marker starts the first entry) | 0x74C00–0x74F00 |
| System page: 55 code pointers, then a count, then 55 system-block offsets (unused entries point at 0x75544) | 0x75554, offsets 0x75634 |
| Arpeggiator codes and print formats | around 0x88850 |
| Effect parameter-name pointer tables per family | 0xAD89C, 0xADBAC, 0xADE24, 0xAE174, 0xAE5E4, 0xAE8FC, 0xAECEC, 0xAF704, 0xAFC4C, 0xAFDCC, 0xAFFE4 |
| Reverb parameter names (76) and routing names (32) | 0xAC994, 0xAF084 |

## Following the panel
- Sound 1–4 and OSC 1/2 from the OS's Sound and OSC LEDs; envelope page from the Envelope LEDs (Pitch → Env 1,
  Filter → Env 2, Amp → Env 3). Never by counting button presses.
- Follow on: turning a panel knob shows the page holding its parameter, cursor on it. Clicking S1–S4 / O1 / O2 picks
  a target and switches Follow off; clicking FOLLOW switches it back on.
- Preset change: reload the visible page (see "Preset changes" below).

## Preset changes (verified on the real OS)
The editor always shows the values of the preset the OS currently holds. The main panel does **not** change on a preset
change beyond what the OS itself sends (LEDs, display): its knobs stay where they are (absolute, no pick-up, no
jumping). That is the intended panel behaviour and the editor never changes it.
- **Triggers for re-reading the visible page** (one SysEx partial read, nothing else):
  1. any program change or bank select sent into the OS (host or virtual port), and any SysEx dump into the OS: queue
     the read directly behind it. The OS handles its MIDI input in order, so the read already sees the new preset
     (verified 5 ms after a program change);
  2. a burst of LED messages from the OS (debounced ~50 ms). The OS sends this burst when a preset actually loads, also
     when the panel selected it: the panel's preset buttons change the display at once but the OS loads the preset
     ~350 ms later (deferred load), exactly when the burst arrives. Do not trigger on the display change alone (it would
     show the old values under the new number), and ignore the tempo LED's regular beat flash (the OS sends it every
     beat; triggering on it would poll);
  3. Compare, and any panel button press (cheap; covers sound and oscillator selection changes too).
- After a preset change, Follow picks up the OS's new Sound / OSC selection from the LEDs.
- Rapid stepping through presets on the panel produces one load, so one re-read.

## Editing
- Click and hold a value and drag up/down: 1 step per 4 dp, Shift = fine. Values are sent while dragging, paced like
  panel knob moves, and the final value on release.
- The mouse wheel never changes values.
- Double-click: typed entry (Enter sends, Esc cancels). Right-click: the value or name list, and revert to stored value.
  Arrow keys move the focus, +/- step it.
- Every edit: clamp to the field's range, send, read back, display what the OS holds.

## Names shown (only names the OS holds)
| Name | Source |
|---|---|
| Preset names (64) | SysEx partial read, type 7, number 0x40 + n (the stored preset), offset 8, 16 characters; current name: preset edit buffer offset 8 |
| Arp settings name stored in the preset | preset edit buffer, offset 334, 16 characters |
| Arp preset names (121) | SysEx partial read, type 8, number n, offset 8, 16 characters |
| Arp pattern names | SysEx partial read, type 9, number n, offset 8, 16 characters |
| Insert algorithm name stored in the preset | preset edit buffer, offset 178, 16 characters |
| Insert algorithm names (40 of 41) | OS image: effect variation records, a 16-character field, right-aligned with leading spaces, repeated once per variation, between 0x93700 and 0xB9700; one algorithm has no stored name (use the panel's 4-character code) |
| Insert variations, global reverb variations | no names in the OS: show VAR 1–6 / PRESET and R01–R08 / PRESET |
| Waves | the OS stores a 4-byte wave ID (oscillator offset 0x4C) and displays a wave number (1–58). The number comes from the OS's wave list in RAM (entry n at 0x0BE06F4C + 4·n, read once by raw-memory SysEx; V-17 resolved). Add a name only from an optional user-supplied text file in the skin folder (number, tab, name) |

## Write path (verified)
**NRPN** (parameters that have an NRPN number), on the base channel, always four controllers, then the null:
`Bn 63 MSB`, `Bn 62 LSB`, `Bn 06 (value >> 7)`, `Bn 26 (value & 0x7F)`, then `Bn 63 7F`, `Bn 62 7F`.
- The OS applies the edit **only when CC 38 arrives**; the value is 14 bits, sign-extended from bit 13.
- MSB 1: preset-common parameters (LSB = parameter number). MSB 2–17: oscillator parameters: MSB = 2 + sound × 4 +
  oscillator × 2 + (parameter >> 7), LSB = parameter & 0x7F (sound 0–3, oscillator 0–1).
- The OS checks and applies the value itself and updates the sound at once.

**SysEx partial write** (all other fields): `F0 0F 11 01 7F 05 <type> <number:14> <subtype> <subnumber:14>
<offset:32> <length:32> <data words> F7`.
- Numbers are sent 7 bits per byte, least significant first: 14-bit = 2 bytes, 32-bit = 5 bytes. Data is sent as
  32-bit big-endian words, each encoded as a 32-bit number (5 bytes); a final partial word is padded.
- No checksum; the OS commits on F7. Raw bytes: the editor must clamp ranges itself.
- Device id byte: 0x7F reaches any unit (the system id setting is the alternative).

## Read path (verified)
**SysEx partial read:** `F0 0F 11 01 7F 04 <type> <number:14> <subtype> <subnumber:14> <offset:32> <length:32> F7`.
Reply: `F0 0F 11 01 00 44` + the same header (with the length actually returned) + data words + F7.
Whole objects: `F0 0F 11 01 7F 01 <type> <number:14> F7`, reply command 0x41.

| Type | Object | Number | Sub-objects |
|---|---|---|---|
| 0 | system settings, 50 bytes | 0 | — |
| 1 | sound, 500 bytes | 0–3 = Sound 1–4 edit buffers | 1 = sound common (36 bytes); 2 = oscillator (180 bytes), sub-number 0/1 |
| 2 | performance, 1398 bytes | 0x3FFF = edit buffer | 1 = track settings, sub-number 0–3 |
| 7 | preset, 414 bytes | 0 = edit buffer; 0x40 + n = stored preset n (user flash); 0x100 + n = the factory copy of preset n in the OS image | — |
| 8 | arp preset, 88 bytes | 0 | — |
| 9 | arp pattern, 37 bytes | 0 | — |

Field offsets within each object are the OS table offsets above (oscillator, sound, sound common) and the private
parameter map's preset and system offsets (16-bit and 32-bit fields big-endian).

**When to read:** on page open, after each edit, after each panel knob/button gesture (debounced ~50 ms), after a
preset change. Never poll continuously.

## MIDI coexistence
- Editor messages are queued as whole messages into the OS's MIDI input between host and virtual-port messages, at
  cable speed (as now, B-08).
- Replies to the editor's requests (command 0x44 / 0x41 while a request is pending) are removed from the OS's MIDI output
  and not passed to the host or the virtual port. All other output passes unchanged.
- The NRPN transmit setting (system byte 25, bit 7; off by default) is the user's choice and is left alone; NRPN receive
  works regardless of it.

## Tests (with the real ROMs, project chat)
- Preset changes: after a program change and after a panel preset step, the visible page shows the new preset's values
  (compare with a direct read); the panel's knobs and display are unchanged by the editor.
- Non-interference: with the editor reading and writing, the OS sends no panel display or LED message beyond what it
  sends idle, and the panel's knobs still edit the panel's selected sound.
- Each page type: values shown equal a partial read of the same range.
- Each write path: an edit changes the sound against a no-edit control and reads back; nothing else in the render changes.
- Plugin with the editor closed: output byte-identical to the previous version.

## Open (settle before the affected pages are implemented)
1. Some raw object writes are accepted and read back but not applied (arp on, global reverb mix): find the OS's path for
   those fields (V-14).
2. Insert pages: per-algorithm parameter lists, displayed units and name-table assignment; the 41st algorithm's name (V-15).
3. Undecoded: NRPN MSB 0 immediate parameters, SysEx commands 07, 08, 0B, 0C, 0D, 12, 13, 15; fine-tune unit (V-16).
