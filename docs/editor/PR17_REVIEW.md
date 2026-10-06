# PR #17 (read-only editor engine, v0.6.0) — verification with the real ROMs and two fixes, 2026-10-06

Name-free. Verified in the project chat on the PR branch as downloaded (no name hits anywhere in the tree), against
v0.5.3, with the real OS image and native wave image (MD5s match the baselines).

## Verified

| Check | Result |
|---|---|
| Editor closed changes nothing | **Bit-identical.** The real `Engine` (48 kHz, 512-sample blocks), all 64 presets (program change, 3-note chord 1.6 s, release 1.2 s) plus 1 s settle: audio and the OS's MIDI output to the host hashed per preset; PR = v0.5.3 on every preset. Chip sources are unchanged in the PR, so `phyzo_play` is identical by construction |
| PR test suites | emulator 12/12 (including editor, editor_engine and the golden-hash sound tests); `skin_test` passes |
| `editor_tables` on the real OS image | Decodes all four tables exactly as traced: names, offsets, value types, ranges, display kinds, formats, list indexes and list contents (pan from −64, key track from −16, the OS's 4-character system values). Hidden fields are right (spares, wave class and checksum, program class, sound finder, unused system slots `????`) |
| Sound, common and system sections | Right (TRACK / RANGE / RECEIVE; system as one grid) |
| Preset-load fallback | Present (`kDisplayFallbackMs = 600`, after any display change) |
| Note names | Right: the original manual states C4 is middle C, so 60 = C4 |
| Names in capitals | Right (as the approved mockups) |
| Skin side | A real skin's editor unit (`rack-editor.rml`, 48 legends, 4 `vfdrow`s) loads in the PR's skin runtime with a clean log: 200 cells, characters applied, the legends lit as expected for a test state, `editor.followLeds` received with the rack's codes. Cell frames land exactly at the layout table's positions |

## Fix 1 — oscillator sections (the section guess from names is wrong for many fields)

Examples of what the current name rule produces: every envelope's "Ramp Time / Ramp Level" go to AMP ("ramp" contains
"amp"), the other envelope fields to GEN; the filter's Mode, Link, mod source, key tracking and breakpoint to GEN; the
wave's start index and its modulation to GEN; pitch modulation source and key tracking to GEN.

Replace the name rule for the **oscillator table** with this list by field index (our analysis, from the OS's own
block structure: each block of the oscillator object is closed by a spare named after it). Order within a section:
field index.

| Section | Oscillator field indexes |
|---|---|
| WAVE | 51, 53, 54, 55, 56, 57, 58 |
| PITCH | 14, 15, 16, 17, 18, 23, 24, 25, 26, 27, 28, 74 |
| FILTER | 30, 31, 32–38, 41–47 |
| AMP | 12, 13, 61–67 |
| NOISE | 70, 77 |
| LFO | 126–136 |
| ENV1 | 78–92 |
| ENV2 | 94–108 |
| ENV3 | 110–124 |
| GEN | 1–11, 19, 20, 75 |
| (hidden, as now) | 0, 21, 22, 29, 39, 40, 48, 49, 50, 52, 59, 60, 68, 69, 71, 72, 73, 76, 93, 109, 125, 137 |

Every index 0–137 appears exactly once. The sound, common and system tables keep their current sections.

## Fix 2 — repeated blocks become numbered columns

`twoColumnRows` numbers only **adjacent** identically named fields. In the oscillator table the two filters are two
consecutive **blocks** with the same names in the same order (indexes 32–38 and 41–47), so nothing is numbered and the
filter page shows each name twice with no way to tell the filters apart. The approved layout (spec: "a run of
identically named fields becomes two columns numbered 1 and 2") is:

```
MODE  RESONANT2LP/2LP        LINK       INDEPENDENT
FILTER CUTOFF 1         19   FILTER CUTOFF 2         19
RESONANCE (Q) 1         16   RESONANCE (Q) 2          0
...
```

Rule: within a section's item list, when a block of n ≥ 2 items is followed immediately by n items with the same names
in the same order, pair them row by row (left = first block, right = second block) and append " 1" / " 2" to the names.
Items before and after (Mode, Link) are laid out as now. A test with a synthetic table (two identical 3-field blocks
after two single fields) covers it.

Both fixes touch only the page model; the engine is unchanged, so the byte-identical result above stands.

## Fixes 3–6 — from the first test in the plugin (2026-10-06)

Kris installed the build with the editor skin; it loads and reads values. Seen in the plugin (preset 01):

**Fix 3 — Follow takes a flashing Sound LED.** The panel had Sound 4 solidly lit (Sounds 1–3 flashing) and both OSC
LEDs lit; the editor showed S1 / O1. `followLeds()` takes the first LED whose mode is not 0, and mode 2 (flashing)
counts. The original manual: the sound being edited has its LED **solidly lit**; the other active sounds' LEDs
**flash**. Take the Sound LED whose mode is 1 (solid); ignore mode 2; if none is solid, keep the current sound. Same for
the envelope LEDs (solid). OSC: both LEDs solid means both oscillators are being edited: show oscillator 1's values and
light both O1 and O2 legends; one solid: that one.

**Fix 4 — names cut short with room to spare.** Osc > Wave: the right-hand name shows as "WAVESHI" with its value at the
far right and ~10 empty cells between. A name should use all the cells its half has left after the value's reserved
width and one space; shorten only when it really doesn't fit.

**Fix 5 — leading spaces kept from the OS's codes.** System codes such as "  id", " tch", " brd" are shown indented.
Trim leading and trailing spaces of names (and of list values) before layout.

**Fix 6 — underline over padding.** Underlines include the blank cells of a value's reserved width ("_12", "_1" on the
System page). Underline from the first to the last character of the value as shown.

## Fixes 7–9 — from running the PR's controller on the real engine and OS (project chat, 2026-10-06)

The controller (PR code unchanged) was driven on the real `Engine` with the real ROMs through a copy of the plugin's
`EditorLink`, stepping every area, section and page with the legend commands. Findings beyond Fixes 1–6:

**Fix 7 — split names at camel-case boundaries.** Names are only split at spaces, so they read "SEMITONETUNING",
"ENV1PITCHMODAMT", "NOISESOURCE RATE", "PITCHBEND ENABLE", "TRIGGERCTRLHI". Before upper-casing and shortening, insert a
space at every lower→upper boundary and between letters and digits ("SemitoneTuning" → "SEMITONE TUNING",
"Env1PitchModAmt" → "ENV 1 PITCH MOD AMT" or, keeping a digit with its word, "ENV1 PITCH MOD AMT"; prefer the latter:
split letter→digit only when the digit run is followed by a capital). Then shorten as specified.

**Fix 8 — value type 2 is an offset byte** (an omission in PR_B_ANSWERS.md, now corrected there): stored = shown + 64.
Today the sound settings show every centred field as "+64" (semitone shift, LFO rates / depth / delay, filter cutoff /
resonance, amp and filter envelope offsets, velocity, glide time, octave) and Track Pan blank (its list index falls off
the end). Decode type 2 as `raw − 64`; lists on type-2 fields then index with the list's start value as usual.

**Fix 9 — PREV / NEXT within the section, as specified.** NEXT currently moves on to the next section after a section's
last page, so in single-page sections the PAGE number never changes and the user can't tell where they are (reported
from the plugin). The spec: PREV / NEXT step pages within the current section and are lit only when that page exists;
sections change by their legends.

Also confirmed on the real engine: preset names read completely (all 16 characters); System and Presets pages correct;
Follow sees the panel's LEDs (Sound 4 solid, 1–3 flashing, both OSC solid on preset 01), so Fix 3 is all that's needed.

Capitals: the glass now shows capitals through the skin (lower-case characters use the capital shapes), so no code
change is needed for that.

## Not in PR #17 (for later, as agreed)

- Follow on a panel knob turn: needs the panel-control → field map; the project chat supplies it with the §5 field list.
- Common and Arp pages (§5), Insert and Reverb (V-15), editing (PR 2).

## Recommendation

Push Fixes 1–9 to PR #17, then merge. The project chat re-runs the real-engine page dump on the fixed branch before
merging. The skin's editor unit is ready in the project chat and goes into the
user's skin folder (never into the repo).
