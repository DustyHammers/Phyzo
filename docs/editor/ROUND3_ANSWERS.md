# Round 3: answers to the eight questions (verified on the real OS)

Name-free (neither the original synth's nor its maker's name). It gives addresses, layouts, slot numbers and rules
only: names, ranges, lists and values are still read from the user's OS image or the running OS at runtime. Addresses
are in the OS image (loading at 0x4000) or in RAM (0x0BExxxxx). The initialised-data section is image 0xBD4A8, copied to
RAM 0x0BE00000, so RAM address A in that section can be read from the image at 0xBD4A8 + (A − 0x0BE00000).

How it was checked:
- the OS code that reads each structure was disassembled;
- the decode was rebuilt independently from the image;
- the running OS was compared with the image over all 64 presets (RAM reads, SysEx reads, and audio for the
  modulation write path).

## 1. Routing-shell slots (slot 0, word at preset offset 110 + 2k)

| k | field | page |
|---:|---|---|
| 0 | insert output bus | INSERT SENDS |
| 1 | chorus output bus | not shown |
| 2 | reverb output bus | REVERB SENDS |
| 3 | dry bus output bus | INSERT SENDS |
| 4, 5 | FX-bus 1 / 2 chorus mix | not shown |
| 6–10 | FX-bus 1–5 reverb send | INSERT SENDS |
| 11 | insert input level | INSERT ALGO p1 |
| 12 | insert output level | INSERT ALGO p1 |
| 13 | insert wet/dry mix (= word 136) | INSERT ALGO p1 as INSERT MIX (see below) |
| 14 | insert wet balance | INSERT ALGO p1 |
| 15 | insert bypass | INSERT ALGO p1 |
| 16 | insert mute | INSERT ALGO p1 |
| 17 | insert mute point | INSERT SENDS |
| 18–24 | chorus input, output, wet/dry, balance, bypass, mute, mute point | not shown |
| 25 | reverb input level | REVERB SENDS |
| 26 | reverb output level | REVERB SENDS |
| 27 | reverb wet/dry mix (= word 164) | REVERB ALGO as REVERB MIX |
| 28 | reverb wet balance | REVERB SENDS |
| 29 | reverb bypass | REVERB SENDS |
| 30 | reverb mute | REVERB SENDS |
| 31 | reverb mute point | REVERB SENDS |

FX parameter message for these: slot 0, param k.
- INSERT MIX with a variation selected: write word 292, then the FX message slot 0, param 13 (what the panel's Mix
  knob does).
- With an insert variation selected, k = 6 (FX-bus 1 reverb send) plays the variation's own value. It is display-only
  then, like the insert parameters (k = 4 also, but it is not shown).

## 2. Values that play under a variation

Don't parse variation records: read the running performance.
- Partial read: type 2, number 0x3FFF, sub-type 0, sub-number 0, offset 0x2CC + (preset offset − 60). Length 0x10E
  gives the whole effect block.
- Verified:
  - 13 of 13 presets: the reply equals RAM byte for byte.
  - 8 of 8 presets with an insert variation: params, mod block, the k = 4 / 6 extras and the mix equal the variation
    record.
  - 64 of 64 presets: reverb params equal the reverb variation record.
- Record layout, for reference: header +0x1A points to 16-byte entries
  `ptr name (16 chars, right-aligned) · ptr mod block (10 bytes) · ptr params (n words) · ptr extra (4 bytes:
  [0] → shell k4, [1] → shell k6; [3] unused)`. Entry index = the variation number (0-based).
- Note: all 64 factory presets have a reverb variation selected. Their REVERB parameters are display-only until the user
  selects PSEt. The OS offers reverb PSEt only when the stored reverb area (294) is family 1 / sub 0; apply the same
  rule.

## 3. Edit-order maps (0xBF040)

- Format:
  - a list of u32 pointers, terminated by a 0 pointer;
  - 21 maps, pointing into the image; the RAM copy at 0x0BE01B98 is identical.
- Each map is `u8 family, u8 sub, u8 count, count × u8 slot`, padded to an even address. The map is selected by
  family/sub.
- The OS (0x70FFE, used when a request carries flag 4) replaces requested index i by `map[3 + i]` when the family/sub
  match. Its bound is the algorithm's parameter count, not the map's count. Use `count` as the number of visible
  entries.
- An algorithm without a map uses the natural order 0 … n−1.
- Checked over all 41 + 2 algorithms:
  - every algorithm that has hidden placeholders (PID 0x0007, range 0..0) has a map;
  - each map lists exactly the non-hidden slots;
  - no unmapped algorithm has a hidden slot.

## 4. Insert-modulation fields (descriptor at image 0x720B8)

**Descriptor.** An ordinary 8-word family descriptor whose records, names and offsets live in RAM:
- +0: records at 0x0BE011E0;
- +4: names at 0x0BE011A8 (count + pointers);
- +8: offsets at 0x0BE011C4;
- lists 0xEAE8; 6 fields; mode 0.

The initial contents are in the image at 0xBD4A8 + (RAM − 0x0BE00000).

**Fields.** Offsets are relative to preset 64: [0, 2, 4, 1, 6, 8].

| field | preset | type | range | display |
|---|---|---|---|---|
| source | 64 | byte | 0..16 (from the record) | value list (index in the record) |
| source min | 66 | word | 0..127 | number |
| source max | 68 | word | 0..127 | number |
| destination | 65 | byte | 0 .. n−1 of the running insert algorithm | formatter 82: the name of insert slot `dest` |
| dest min | 70 | word | destination parameter's min .. max | the destination parameter's display |
| dest max | 72 | word | destination parameter's min .. max | the destination parameter's display |

**Destination max.** The OS writes n−1 into the destination record's max at 0x0BE0122C when the algorithm loads.

**Destination range rules.** Compute these from the destination's own record. Don't read the RAM records:
- The OS refreshes those RAM records only from its own parameter UI (0x173F0), not on preset changes.
- After preset changes they hold stale values (seen on all 64 presets).

**What the OS does when the destination changes** (0x173F0):
- Both ranges become [destination min, destination max].
- If the destination is tempo-syncable, the minimum becomes 11.
- The stored dest min is reset to that minimum, and dest max to the destination's max.

The editor does the same in the same write (see the write path below).

**Tempo-syncable.** A destination is tempo-syncable when its record uses formatter 80, or a value list whose first 11
entries equal the 11 names of 0xBE288. This reproduces the OS's own (hard-coded) set exactly for every parameter of
every selectable algorithm.

**Write path: this changes the spec.**
- The raw-write + FX-message path does not work for these fields. Measured with the mod wheel as source:
  - a raw write into the running copy switches the source live, but destination, min and max stay cached (same result
    for dest 1, 14, 19 and for min = max = 0);
  - a raw write into the edit buffer alone changes nothing.
- The only path the OS applies fully: one partial write of the preset (type 7 #0, offset 64, length 10, the whole block
  from the last read-back with the edited field changed). Verified: the destination and range are honoured
  (dest 14: +3.9 dB, range 0..0: no change).
- It reloads the insert, the same ⅓-second dip as an algorithm change. Send one write per gesture (on release / Enter),
  at most one per 150 ms, then read back.
- With an insert variation selected, these fields are display-only (the variation brings its own mod block).

## 5. 0xAC868 and the PID lookup

- **Table.** 32 u32 pointers indexed by family (PID >> 8). 0 means no family; a family ≥ 32 is an error (0xDE10). Each
  pointer is a family descriptor in the same 8-word format as the existing tables:
  - +0 records, +4 names (count + pointers), +8 offsets, +0x0C lists 0xEAE8, +0x10 handlers;
  - +0x14 u16 record count, +0x16 u8 mode, +0x18 RAM offset table.
- **Lookup** (0x710C8), for id = PID & 0xFF:
  - mode 0: walk the records (byte 0 = record length) and match the section-7 PID; a record without section 7 takes
    the previous id + 1, starting at 0;
  - mode 1/2: use the RAM table at +0x18 (u16 offset per id);
  - other modes: a fixed stride.
- **All effect families are mode 2**, and their RAM table is built at boot. Walking the records and matching the
  section-7 u16 to the full PID gives the same record:
  - checked against the RAM tables for all 326 effect records;
  - all 853 parameters of the 44 algorithms resolve.
- **Effect families differ from the existing tables:**
  - the names array is shorter than the record count (names are shared through the record's name index);
  - the offsets array is a shared one (count 32).
  - So the existing `readTable` count check (names = offsets = records) fails for them. Effect fields take their name
    from `names[name index]` and their storage from the slot (194 + 2k etc.), not from the offsets array.

## 6. Size tables 0x71418 / 0x71424 (units: bytes)

**Generic walker** (0x70C4C): `walk(block, n, table)`.
- Byte 1 of the block = a mask of the sub-blocks present.
- Start at block + table[0] (the header size).
- For each present i < n: add table[1 + i]. 0xFF means variable: add the byte at the current position (the
  sub-block's own length).
- Result: the start of sub-block n (an error if bit n is clear).

**Record** (table 0x71418 = `08, FF×7, 02`):
- an 8-byte header `[len][section mask][u16 type][i16 min][i16 max]`;
- sections 0–6 are length-prefixed;
- section 7 is a bare u16 PID, at record + 8 + the lengths of the present sections 0–6.

**Section 0, display** (table 0x71424 = `04, 04, 04, 04, 02, 02`):
- a 4-byte header `[len][sub mask][u16 fmt]`;
- bit 0: name index (u32);
- bit 1: value-list index (u32);
- bit 2: formatter number (u32);
- bit 3: prefix byte, suffix byte;
- bit 4: width byte, decimals byte.
- Masks seen across all 326 effect records:
  - 0x03 name + list (239);
  - 0x01 name only, a plain number (25);
  - 0x19 name + prefix/suffix + width/decimals (24);
  - 0x09 name + prefix/suffix (22);
  - 0x05 name + formatter (15);
  - 0x11 name + width/decimals (1).
- `fmt` is 0 on every effect record. Every effect record has sections 0, 1 and 7 (mask 0x83).
- Walk the masks bit by bit. The existing parser's sub-record "kind" switch (1/3/5/9) is this mask, but it does not
  cover 0x11 or 0x19, which carry the decimals.

**Section 1, handler** (table 0x71430 = `02, 04, …`): a 2-byte header `[len][mask]`, then the u32 handler index at bit
0. Two effect records also set bit 6; ignore it. The modulation records use bits 0 and 2.

## 7. 0x6FED8 and 0xBE288

- **0x6FED8:** the suffix list, in the same encoding as the value lists the editor already decodes:
  - a 0x80 marker, entries separated by 0x0D, ending with 0x00;
  - 12 entries;
  - the suffix byte n shows entry n−1, and 0 means none.
- The OS reaches it through the pointer at 0xBEA5C. The pointer at 0xBEA58 is the prefix list (0x6FED6, empty).
- **0xBE288:** 11 u32 pointers to C strings, then a 0 pointer (RAM copy 0x0BE00DE0).
- **Formatter 80** (0xE9F4): v < 11 shows `names[v]`; v ≥ 11 shows `printf("%ldms", v − 11)`. The format string is at
  0x124A0.
- **Formatter 81** (0xEA90): shows `printf("%ldms", v)` with the same string.

## 8. 0x89390

- 41 entries of 5 bytes: 4 characters + NUL. Index = the insert algorithm index 0..40, the same order as the pointer
  table at 0xBE080 (0x88F56: `0x89390 + 5 × index`).
- The variation strings that follow are separate strings, not on the 5-byte stride:
  - `PSEt` at 0x8945E;
  - the insert format ` u%02d` at 0x89464;
  - the reverb format ` r%02d` at 0x8946C.
- The panel shows a variation v ≥ 0 as format(v + 1), and v < 0 as `PSEt` (0x88F1A–0x88FB0).

## Also confirmed

The raw-memory write uses the same address form as the editor's existing raw reads: type 0x7F, number 0, sub-type 0,
sub-number 0, offset = the absolute RAM address. A write to 0x0BE0DA20 + 64 landed byte for byte.
