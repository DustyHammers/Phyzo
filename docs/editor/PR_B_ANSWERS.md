# Answers for PR B (editor engine) — OS table formats and scope, 2026-10-05

Name-free. Traced in the OS image (load address 0x4000, big-endian 68k) in the project chat, decoded with a parser that
walks every record of every table listed here without a single misparse unless noted. Tags: **Verified** (decoded and
checked against known values or the running OS), **Derived** (consistent decode, not cross-checked), **Assumed**.
Everything below is numbers and layouts (our analysis); no OS strings are reproduced, except as short examples of what
the tables produce, which PR B must read from the user's OS image at runtime.

## 1. Field tables (oscillator, sound settings, sound common, system, insert control)

Each table is reached through a **descriptor** of 8 big-endian 32-bit words:

| Word | Content |
|---|---|
| 0 | pointer to the **field records** (one per field, see below) |
| 1 | pointer to the **names array** |
| 2 | pointer to the **offsets array** |
| 3 | pointer to the **value-list array** |
| 4 | pointer to a further array (not needed by the editor) |
| 5 | high 16 bits: field count; low 16 bits: flags (0x0200 on the oscillator table) |
| 6 | RAM address of the OS's working copy, or 0 |
| 7 | (next structure) |

| Table | Descriptor | Fields | Lists | Offsets are relative to |
|---|---|---|---|---|
| Oscillator | 0x731F8 | 138 | 15 | the oscillator sub-object (SysEx type 1, sub-type 2) — **Verified** (Filter Cutoff = 0x39, wave ID = 0x4C, as read back) |
| Sound settings (per sound in the preset) | 0x72064 | 55 | 13 | the track record (SysEx type 2, number 0x3FFF, sub-type 1, sub-number = sound) — **Verified** (volume = 4, pan = 5) |
| Sound common | 0x731DC | 12 | 7 | the sound object (type 1, number = sound, offsets 24–35) — **Derived** |
| System | 0x75528 | 55 | 6 | the system object (type 0) — **Verified** (nrPn = byte 25 bit 7) |
| Insert control | 0x72048 | 1 | 1 | — (not needed for PR B) |

**Arrays** (names, offsets, lists): a 32-bit count, then that many 32-bit entries. Names entries are pointers to
NUL-terminated 8-bit strings; offsets entries are 32-bit byte offsets; list entries are pointers to value lists.
Example: oscillator names array at 0x73388 (count 138, entries from 0x7338C), offsets array at 0x735B8 (count 138,
entries from 0x735BC).

**Field records** (the stream at descriptor word 0), one per field in field order. Zero bytes between records are
padding: skip them. Each record:

| Bytes | Meaning |
|---|---|
| 0 | record length in bytes, including everything below |
| 1 | header flags: 0x01 or 0x03 seen everywhere; 0x80 additionally on a few fields (meaning not decoded, see §1.3) |
| 2–3 | **value type**: 0 unsigned byte; 1 signed byte; 2 offset byte (stored = shown + 64; added after the PR #17 review); 3 bit field (min = width << 8 \| bit number, max = 1 for flags: **Verified**); 4 multi-byte value (the 4-byte wave ID; min 0, max −1); 5 small unsigned (one oscillator field, 0–15); 6 signed byte with a built-in formatter; 7 signed byte where −1 is the list's first entry ("use the program's value") — **Derived** |
| 4–5 | minimum, signed 16-bit |
| 6–7 | maximum, signed 16-bit |
| 8 … end | sub-records, each starting with its own length byte, then a kind byte |

Sub-record kinds (byte 1 of the sub-record):

| Kind | Length | Meaning |
|---|---|---|
| 1 | 8 | **number** display: bytes 2–3 format flags, bytes 4–7 a parameter number (= the field index for the oscillator, which is also its NRPN parameter number — **Verified** for 0x20, 0x33, 0x39) |
| 9 | 10 | number display with an extra 16-bit parameter at bytes 8–9 (seen on volume, transposition, octave; meaning not decoded — display as a plain signed number until settled) |
| 3 | 12 | **value list**: bytes 8–11 = index into the descriptor's list array — **Verified**. One record (sound settings field 23) has length byte 0x0E with two trailing bytes: trust the record length, not the sub-record length |
| 5 | 12 | **built-in formatter**, not a list: bytes 8–11 = formatter number (1 and 6 seen: fine tuning, delay offset). Display as a signed number until the formatters are decoded |
| 1 | 6 | (sound settings only) a short extra record, meaning unknown; ignore |

Format flags (kind 1 and 9, bytes 2–3), **Derived** from where they occur: 0x0000 plain number; 0x2000 signed, shown
with its sign; 0x0002 Off/On; 0x0003 MIDI note name (key ranges, breakpoints); 0x0001 and 0xC000 shown 1-based
(bank, program, system id, base channel) — **Assumed**, confirm with `editor_tables` against the hardware's display.

### 1.1 Value lists
Fields point to their list **by index** into the descriptor's list array (kind-3 sub-record). Each list string:
- optional header: byte 0x05 followed by a signed 16-bit value = the value of the first entry (e.g. −64, −16, −1);
  without the header the first entry is value 0 (or the field's minimum);
- entries separated by CR (0x0D), the list ends at NUL; ignore a 0x80 marker byte;
- compression: a byte 0x11–0x1F copies (byte − 0x10) characters from the same position of the previous entry;
  0x10 copies the rest of the previous entry and ends the entry. (**Verified**: decodes the filter modes, modulation
  sources, key-track fractions and system values exactly.)
- A list shorter than the range: values past the last entry display as numbers (**Assumed**; seen on key group and
  restrike limit, whose lists hold only their first, named value).

### 1.2 Hidden fields
Skip fields whose name contains `SPARE`, plus the wave class / wave checksum / program class fields, "Use Layer",
"Old Keybd Track", the two sound-finder fields, and system entries whose name is `????` (unused slots).

### 1.3 Known oddities (for `editor_tables` to confirm)
- Sound settings field 0 (MIDI channel): header flags 0x83 and range 0..1, but its list has 17 entries: use the list
  length as the range until decoded.
- Oscillator field 5 (trigger key): type 3 with min/max that are not a bit position; show as number.
- Pan lists carry header bytes before the first entry; decode with the header rule above.

## 2. Value lists — answered in §1.1 (by index).

## 3. System table
A normal descriptor (0x75528). Its names are the OS's 4-character codes (some with leading spaces); `????` entries are
unused. Offsets are 32-bit, into the system object. Sizes, ranges, value lists (the OS's 4-character values) and formats
come from the field records exactly as in §1. No separate code/offset arrays are needed (the 0x75554 / 0x75634 arrays in
the earlier spec are this descriptor's names and offsets arrays).

## 4. Arpeggiator
There is **no arp field table**. The arp display code (0x87FC2–0x88590) references each code and print-format string
individually; the strings are NUL-terminated and packed between 0x887C4 and 0x888A8. Addresses: tempo format 0x88812,
range format 0x88818, note-resolution code 0x8881E, the interval codes 0x88828 / 0x8883A / 0x8883E / 0x88844 /
0x8884A, On / Off values 0x8882E / 0x88834, hold 0x88850, centre 0x88856, low / high note 0x8885C / 0x88868 with
note format 0x88862, duration 0x8886E with format 0x88874, note limit 0x8887C, PrSt 0x88882, sound 0x88888, number
format 0x8888E, feel 0x88892, touch 0x88898, arp preset format 0x8889E. Note-resolution names: a pointer table at
0x8876C (12 values, then a fallback entry at 0x8879C). The mapping from the stored resolution value to that index is not
decoded yet. The arp page therefore needs a field list from our analysis (preset offsets, sizes, ranges) with these
strings as its labels — see §5.

## 5. Preset-level offsets in the repo
The preset object has no field table in the OS (no names exist for its fields), so the Common and Arp pages need our own
field list: offsets, sizes, ranges. These are measured numbers, like the addresses in the OS profile, not OS content.
**Recommendation: yes, as plain numbers with neutral labels.** Kris to confirm; if confirmed, the project chat supplies
the list in neutral form. If not, PR B leaves Common and Arp out.

## 6. LED codes
Agreed: the skin supplies them through the Lua API (`editor.followLeds{...}`), nothing hard-coded. The rack skin's
values: Sound 1–4 = 0x0B, 0x13, 0x03, 0x07; OSC 1 / 2 = 0x06, 0x15; envelope Pitch / Filter / Amp = 0x11, 0x0C, 0x0E.
Preset-load detection: agreed, count only LED on / off / flash messages, never the beat-flash rate message. Add one
fallback: after the display shows a new preset number, if no LED-state burst follows within ~600 ms (a preset whose LED
states equal the previous one's), re-read anyway. (Measured: panel preset steps load ~350 ms after the press.)

## 7. Waves (V-17 resolved)
The OS builds its wave list in RAM at boot: entry n (1–58) is the 4-byte wave ID at **0x0BE06F4C + 4·n**; entries 0 and
59 are zero. OS code uses this table (references at 0x8C732, 0x8C796) — **Verified**: all 56 measured number↔ID pairs
match, plus waves 1 and 58. Read it once after boot with the raw-memory SysEx read (type 0x7F, offset 0x0BE06F50,
length 232) and show the **wave number**; no hex IDs, no table in the repo. Names stay in the optional user file.

## 8. Scope of PR B
Agreed, with one addition if §5 is confirmed:
- PR B: Presets (name list), System, Sound (settings + common), Oscillator — all fully specified now;
- Common and Arp in PR B if §5 is confirmed (the arp resolution mapping can show the raw value until decoded);
- Insert and Reverb after V-15.

## Proposals
- **`<name>-editor.rml`** loaded below the rack, menu item greyed out without it, no editor unit in the Built-in skin:
  agreed.
- **`editor_tables`** (terminal-only, writes nothing): agreed. Please print, per table: index, name, offset, record
  flags, value type, min, max, display kind, format flags, list index and the decoded list entries. It reads the OS image only
  (the wave list exists only in RAM at runtime, §7). The project chat will run it against the OS image and confirm §1.2–§1.3.
