# Editor screens (F-30) — implementation steps, 2026-10-05

Name-free document (safe for Claude Code). Current state: design revision 4 approved for implementation planning —
one glass of 50 × 4 14-segment cells spanning the editor unit, every control a clickable legend (areas, sections,
S1–S4 / O1 / O2, PAGE 1–9, ◀ PREV / NEXT ▶, FOLLOW), no buttons; write and read paths verified on the real OS (V-13 done);
non-interference with the main panel verified.

Standing rules for every step: confirm the baseline (version, file checksums) before any change; the main panel's art,
layout, display and behaviour are never changed by editor work; no parameters added or removed; mockup before RML;
the two names never reach GitHub; one change per PR, everything else byte-identical.

## Step 1 — Editor background art (Kris, can start now)

- Start from `EDITOR_PANEL_BLANK.png` (3590 × 788 px, 2x; face, ears and bands rebuilt from the current main-panel art),
  with `EDITOR_PANEL_POSITIONS.png` as a guide layer above it.
- The art needs only the face, ears, bands and, optionally, the glass window (left 131, top 40, 1532 × 308 dp,
  radius 16). Everything that changes (characters, underlines, legends) is drawn by the skin.
- Save as the editor background in the skin folder (suggested name `PhyzEditorBackground.png`). If you'd rather not
  paint anything yet, the blank works as-is.

## Step 2 — Claude Code, PR 1: the editor engine, read-only

Input: `EDITOR_SPEC.md` (revision 4) and `EDITOR_PANEL_LAYOUT.json`. Showing values first, without editing, tests the
riskiest new parts while nothing can change the sound or the panel.
- **MIDI transport:** the editor's messages enter the OS's MIDI input only between whole host / virtual-port messages,
  at cable speed; the OS's replies to the editor (partial read 0x44, dump 0x41) are removed from MIDI out; all other
  output passes unchanged. Nothing is sent while the editor is closed.
- **OS tables at runtime:** field names and offsets, value lists, system codes, arp codes and formats, effect names,
  read from the user's OS image at the addresses in the spec (never copied into the repo).
- **Page model:** the five page types, the name-shortening rule, value formatting, 50 × 4 cells; reads the visible
  page with SysEx partial reads on page open and after panel gestures; never polls.
- **Preset changes:** re-read the visible page directly behind any program change, bank select or SysEx dump sent into
  the OS, and on the OS's preset-load LED burst (panel preset steps load ~350 ms after the press; not the display change;
  ignore the tempo beat flash). The main panel's knobs stay where they are: intended, never changed.
- **Navigation (all by legends):** areas, sections, targets, pages, PREV / NEXT (lit only when that page exists),
  FOLLOW; follows the panel's Sound / OSC / Envelope LEDs.
- **Skin API:** a Lua `editor` API, a text-row element drawing characters from a sprite sheet (frame = ASCII code) with
  underline / focus-blink / ghost states, clickable legend elements (lit / ghost / hover).
- **Window:** right-click > Editor adds the 1795 × 394 dp unit below the rack (848 → 1242 dp), resizing as F-24.
- **Optional name file:** if the skin folder holds the wave-name text file (number, tab, name), the wave value shows
  "number name"; otherwise the number only.
- **CI tests (no ROMs):** message encoders and decoders round-trip; the reply filter; the queue never splits a host
  message; page generation against synthetic tables; with the editor closed, the engine's output is byte-identical.

## Step 3 — Project chat (real ROMs): verify PR 1, build the skin side, test

- **Verify PR 1:** editor closed → byte-identical to the previous version on all 64 presets; preset changes (program
  change and panel steps) update the visible page to the new preset's values; non-interference
  (no extra panel display or LED messages, panel Sound/OSC selection unchanged while the editor reads); every page's
  values equal a SysEx read of the same range.
- **Build the skin files** (skin folder, not GitHub): the 14-segment character sheet and legend sprites in the panel
  red with ghosts; the editor RML / RCSS / Lua; a render check against `EDITOR_PANEL_MOCKUP.png`. Re-flow the approved
  page mockups (drawn on the earlier 40-cell glass) onto the 50-cell grid first, for a quick look.
- **Kris tests in REAPER (read-only):** every area and page, Follow, targets, preset / arp / effect names, wave names,
  values following preset changes (host program changes and the panel's preset buttons), panel unaffected (knobs stay put).

## Step 4 — Claude Code, PR 2: editing; then verify and test

- Click-and-hold drag (sent while dragging, paced like knob moves; Shift fine; never the wheel), typed entry,
  right-click value / name lists, revert to stored; clamp every SysEx write to the field's range; read back after each
  edit and show what the OS holds.
- Write paths: NRPN (all four controllers, then the null) where defined, SysEx partial write otherwise. Fields whose
  live update is still open (V-14: arp on / off, global reverb mix) stay read-only until settled.
- Project chat: each write path changes the sound against a no-edit control and reads back; nothing else changes;
  non-interference again. Then Kris tests editing in REAPER.

## In parallel (analysis sessions)

| Item | Needed for |
|---|---|
| V-14: the OS's live-update path for arp on / off and global reverb mix | editing those fields (Step 4) |
| V-15: per-algorithm insert parameter lists, units and name tables; the 41st algorithm's name | insert pages beyond page 1 |
| V-16: remaining protocol pieces (NRPN MSB 0, SysEx 07–15), fine-tune unit | completeness; not blocking |
| V-17: the OS's own wave list (wave ID → number) and waves 1 and 58 | wave numbers from the OS image instead of a measured table |
| B-12: regenerate the private preset reference's wave names by number | documentation only |
