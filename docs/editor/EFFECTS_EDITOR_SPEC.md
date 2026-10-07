# Editor INSERT and REVERB pages — specification (round 3)

Name-free document for Claude Code (neither the original synth's nor its maker's name). It describes structures,
addresses and rules only. Every name, range, value list and default comes from the user's OS image or the running OS
at runtime, as for the existing pages; no OS table content is ever copied into the repo. Addresses are in the OS image
(loading at 0x4000) or in RAM (0x0BExxxxx). Research and verification were done on the real OS in the project chat;
anything marked "verify" must be checked there again after implementation.

## Decisions (owner, 2026-10-06)

1. **Variation selected = display-only.** When an insert variation (preset byte 332 ≠ 0xFF) or a reverb variation
   (333 ≠ 0xFF) is selected, the OS plays the variation's values and ignores the stored ones. Show the values that
   play, display-only (no underline), with the hint "select PSEt to edit". Under PSEt (0xFF) the stored values are
   editable.
2. **Insert modulation moves to INSERT > MOD**, shown with names (source list, destination = the name of the insert
   parameter it targets, min/max in the destination's units). Remove the insert-modulation fields from COMMON > MOD.
3. **The global chorus fields are not shown.** The OS never loads the chorus slot; its two FX-bus mix fields stay off
   the pages.
4. No parameters are added or removed. The skin already has every legend (INSERT, REVERB, ALGO, MOD, SENDS, pages
   1-9): no skin change.

## Pages

| Area | Section | Page | Fields |
|---|---|---|---|
| INSERT | ALGO | 1 | ALGO (the 41 algorithm names, right-click list) · VARIATION (the panel's codes: variations ` u01`…, and `PSEt`, offered only when bytes 330/331 equal the stored algorithm 174/176) · INSERT MIX (word 136 under PSEt, word 292 with a variation; what the panel's Mix knob writes) · WET BALANCE · INPUT LEVEL · OUTPUT LEVEL · BYPASS · MUTE (routing shell parameters for the insert) |
| INSERT | ALGO | 2-5 | the algorithm's parameters in the OS's own edit order, 8 per page |
| INSERT | MOD | 1 | MOD SOURCE · MOD DEST · SRC MIN · SRC MAX · DEST MIN · DEST MAX |
| INSERT | SENDS | 1 | FX-bus 1-5 reverb sends · insert output bus · insert mute point · dry bus out (routing shell parameters) |
| REVERB | ALGO | 1 | VARIATION (` r01`… ` r08`, `PSEt`) · REVERB MIX (word 164) · the reverb's 6 parameters |
| REVERB | SENDS | 1 | input level · output level · wet balance · bypass · mute · mute point · reverb output bus |

PREV / NEXT and PAGE legends as on the other areas; INSERT and REVERB legends become available.

## Where things are

**Preset edit buffer** (type 7 #0, RAM 0x0BE0DA20). Effect block (EFX1) at preset offset 60. Slot areas:

| slot | area offset | params at (word k) | count |
|---|---:|---:|---:|
| 0 routing shell | 90 | 110 + 2k | 32 |
| 1 insert | 174 (family word 174, sub word 176) | 194 + 2k | per algorithm, ≤ 32 |
| 2 chorus | 258 | 278 + 2k | not shown |
| 3 reverb | 294 | 314 + 2k | 6 |

Other bytes: 330/331 = selected insert algorithm (family, sub); 332 = insert variation (0xFF = PSEt); 333 = reverb
variation (0xFF = PSEt); word 136 = insert mix (PSEt); word 292 = insert mix (variation); word 164 = reverb mix.
Insert modulation: bytes 64 (source), 65 (destination slot), words 66/68 (source min/max), 70/72 (destination
min/max). The destination is the insert parameter slot k (offset 194 + 2k), not the edit order.

**Algorithm tables (OS image).**
- Insert algorithm pointer table: 0xBE080, 41 entries (index 0-40); routing shell 0xBE078; reverb 0xBE128.
- Algorithm header: +0 name pointer; +6 family; +8 sub; +0x0E parameter count; +0x10 parameter map; +0x18 variation
  count; +0x1A variation table.
- Parameter-map entry (16 bytes): `u16 family, u16 id, u16 n, ptr, u16 m, ptr`; entry k is parameter slot k.
  Parameter id (PID) = family << 8 | id.
- Parameter definitions: descriptor table 0xAC868 (32 family descriptors, the same 8-word format as the oscillator
  and sound tables). Field records are sectioned; byte 1 of a record is the mask of sections present (size table
  0x71418): section 0 display (header `[len][bits][u16 fmt]`, sub-sections sized by 0x71424: name index, value-list
  index, formatter number, prefix/suffix bytes, width/decimals bytes); section 1 handler; section 7 PID.
  All effect fields are 16-bit signed words.
- Edit order: per-algorithm maps from the table at 0xBF040 (format in ROUND3_ANSWERS.md); each map
  lists the visible slots and omits hidden placeholders (PID 0x0007, range 0..0).
- Display: list = `values[raw]` (raw value is the index); number = value / 10^decimals + suffix (suffix list 0x6FED8,
  index = byte − 1); formatter 80: 0-10 tempo-sync values from table 0xBE288, ≥ 11 `(v − 11) ms`; formatter 81: `v ms`;
  formatter 82 (modulation destination): the name of insert parameter slot `dest`.
- Variation names: the 16-character right-aligned field in each variation record (`~` separators shown as spaces);
  the panel's codes ` u%02d`, ` r%02d`, `PSEt` come from 0x89390 (5-byte stride; the table ends with them).
- Insert-modulation fields: descriptor at image 0x720B8 (records and names in RAM, initial copy in the image); destination min/max take the destination
  parameter's record.
- Algorithm without a stored name (index 40): use its panel code from the 0x89390 table.

## Write paths (verified on the real OS)

1. **Effect parameter value (stored, live, no audio dip):** write the word into the edit buffer with the SysEx raw
   memory write (cmd 05, type 0x7F = raw memory, address 0x0BE0DA20 + offset, 2 bytes; the same encoding the
   editor's existing raw writes use; verify the address field), then apply it live with the OS's effect
   parameter message, 14 bytes: `F0 0F 7F 7F 00 00 00 00 <slot> 00 <param> <value & 0x7F> <(value >> 7) & 0x7F> F7`
   (slot 0 routing, 1 insert, 3 reverb; param = slot index k). The OS does not clamp: clamp to the record's min/max.
   This is what the OS's own Mix knob does internally.
2. **Never** use a SysEx partial write of the preset object for effect parameter values (exception: the insert
   modulation block, 4a) (it reloads the insert: wet path −5 to
   −8 dB for ~⅓ s, and writes under 100 ms apart are lost from the sound), and **never** NRPN MSB 1 LSB 34/35 (the OS
   writes the high byte of the mix word).
3. **Algorithm:** NRPN MSB 1 LSB 30 = family, LSB 31 = sub, then LSB 32 = 0 (variation 1), always in this order (the
   variation message is what makes the OS rebuild the insert). Verified: the edit buffer is byte-identical to the
   panel's result for the same algorithm, display identical. The stored algorithm (174/176) is not changed, as on the
   panel.
4. **Variation:** NRPN MSB 1 LSB 32 (insert) / LSB 33 (reverb), value −1 (PSEt) or 0..n−1. PSEt is allowed only when
   330/331 equal 174/176 (insert), and for the reverb only when the stored reverb area (294) is family 1 / sub 0; the
   same rules as the panel. All 64 factory presets have a reverb variation selected.
4a. **Insert modulation (preset 64-73):** one SysEx partial write of the preset edit buffer (type 7 #0, offset 64,
   length 10: the whole block from the last read-back with the edited field changed); one write per gesture, then
   read back. It reloads the insert (the same short dip as an algorithm change); this is the only
   path the OS applies fully (a raw write into the running copy switches only the source; destination and range stay
   cached). When the destination changes, write in the same block dest min = the destination's min (11 if
   tempo-syncable) and dest max = its max, as the OS does. Display-only under an insert variation.
5. NRPN: all four controllers (99, 98, 6, 38), then the null (99/98 = 0x7F); the OS applies on CC 38.
5a. **Rebuild hold:** after the modulation write or NRPN LSB 30-33, hold every effect send for 350 ms (the OS loses
   effect edits from the sound for about 280 ms while it rebuilds), then send the latest queued value.
6. Read back after every edit (partial read) and show what the OS holds. Under a variation every field of that slot
   is display-only (decision 1).

## Tests

- ROM-free (CI): the table parsers against synthetic tables (header, parameter map, sectioned records, edit-order
  map, value lists, suffixes, formatters 80-82); the FX message encoder (values 0, 127, 128, negative, max); the
  NRPN order for algorithm selection; variation-selected fields are not editable; COMMON no longer shows the insert
  modulation.
- Editor closed: output byte-identical to the previous version.
- With the real OS (project chat, after the PR): every page's values equal the OS's; each write path lands, reads
  back and changes the sound against a no-edit control; algorithm and variation changes equal the panel's result;
  the main panel is unaffected.
