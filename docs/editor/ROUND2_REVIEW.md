# Round 2 (v0.7.0) — verification with the real ROMs and one required change, 2026-10-06

Name-free. The PR branch as downloaded (no name hits), against v0.6.0 / v0.5.3, with the real OS and wave images (MD5s
match). Edits were made through the PR's own `Controller` (focus, typed entry, drag, menu choices) on the real `Engine`,
and each result was checked in the OS's memory and, where it matters, in the audio.

## Verified

| Check | Result |
|---|---|
| Editor closed | **Bit-identical** to v0.5.3 on all 64 presets (audio and MIDI out, real engine); the engine change (last panel control) has no effect on output |
| Tests | emulator 13/13 (including the new editing test); `skin_test` passes |
| Oscillator, NRPN | typed: filter cutoff 1 → 0 lands; negative: cutoff mod amt 1 → −50 lands as 0xCE; drag: resonance +10 steps → +10; bit field: PitchBend Enable off clears only its bit (0xFF → 0xFE) |
| Oscillator wave | choosing wave 13 writes its 4-byte ID (014B04CA), the glass shows "13 FAT …"; the wave NRPN is never used |
| Sound settings | track volume 127 → 48 lowers the note by 16.7 dB |
| System | tune 49 lands; NRPN-transmit on sets only bit 7 (0x5F → 0xDF), off restores 0x5F |
| Common | slot 1 User mode + top key B3: middle C silent (−42 → −97 dB, a previous note still decaying), A3 sounds; knob P → 93 lands |
| Display-only field | a drag on arp feel changes nothing |
| Presets | picking 02 loads it (display "P 02", edit buffer name changes) |
| Follow on panel knobs | Cutoff → FILTER, Filter Cutoff 1 focused with its new value; Wave Select → WAVE; Attack → ENV1 stage 1 (Pitch envelope LED lit); P → COMMON KNOBS; Level → AMP Volume |
| Skin | the rack skin's editor unit with `editor.followLeds{… noise, lfo, filterKbd}` and `editor.knobLabels{"P","H","Y","Z","O"}` loads with a clean log |

## Required change — every arp setting through the panel's own buttons (replaces direct writes and the dump)

Verified on the real OS (2026-10-06): a whole-preset dump stops a running arp and resets the Keyboard setting; direct
writes into the live arp state skip the OS's own bookkeeping (a panel resolution step also writes three runtime copies,
a tick value, the resolution index 0x0BE01884 and the arp-edited flag 0x0BE018AE; a panel Range step sets range up and
down together plus the octave count 0x0BE01885 and the flag; an arp-preset load runs four setup routines). So the editor
changes arp settings exactly as hands on the panel do, with buttons only — **it never moves a knob** (Kris).

**The OS's arp-control variable 0x0BE01877** (raw-memory read): 0 = Tempo, 1 = Range, 2 PrSt, 3 nrES, 4 ndur, 5 FEEL,
6 −3rd, 7 3rd, 8 4th, 9 5th, 10 Oct (interval), 11 Lo n, 12 Hi n, 13 tch, 14 CEnt, 15 ntLt, 16 HELd.

Procedure for one edit of arp setting t to value v:
1. Press the control's button (press, release): Tempo 0x1E (t = 0), Range 0x03 (t = 1), Arp Mode 0x21 (t = 2–16; the
   first press brings up the menu at its current parameter, each further press steps 2 → 16 → 2). Read 0x0BE01877 after
   every press and press Arp Mode again until it reads t. Never count presses.
2. Wait ~1.4 s (the OS then shows the value).
3. Press +/Yes 0x0D or −/No 0x0E once per step towards v (one press per 40 ms; verified exact at 25, 40 and 60 ms),
   reading the setting back from the live arp state or the OS variable; stop at v.
4. **Safety: never press −/No or +/Yes unless 0x0BE01877 = t was read after step 1** (outside the arp controls those
   buttons step the main preset).

Verified: arp presets (PrSt) step exactly (" Snd" → 0 → 1 …, −/No back), the arp keeps playing, the main preset is
unchanged, no knob moves; resolution 16th → 32nd; Range Oct1 → Oct2; Lo n +3 (both key-range pairs, the edited flag set);
FEEL Off → 0. The main display shows the arp setting, as when the panel is used by hand.

Fields and sources (values read from the OS; no LED codes needed):
- PrSt: the OS's current arp preset 0x0BE01880 (32-bit, −1 = " Snd", else the arp preset), shown with its name; drag
  one step per step, typed entry and the right-click list step to the target. (v0.8.3.5, ARP_ROUND.md: labelled with
  the OS's own number from 0; the list ends where the OS stops answering name reads.)
- nrES, ndur, FEEL, −3rd … Oct, Lo n, Hi n, tch, CEnt, ntLt, HELd: from the live arp state (0x0BE093D0, layout as in
  EDITOR_ROUND2.md §3); all editable as above. FEEL is now editable (stepping, shown as the OS's own values).
- Range (Oct n, up and down together): from live $11 / 12.
- Tempo: the value the OS's Tempo control shows and edits (the running arp tempo). Confirm in the project chat's
  verification which variable that is under each system arp-tempo source setting (AtPO); until then show the live
  arp tempo ($38–$3B) and edit through the Tempo button.
- On/off and Keyboard: the OS's runtime flags 0x0BE018BA / 0x0BE018BB; switched by pressing the panel's own Arp On/Off
  (0x06) and Arp Keyboard (0x05) buttons once (verified: display unchanged, only the arp LEDs change).
- The arp settings name (offset 334) and the arp pattern (offset 390) stay display-only (set by loading an arp preset).
- **No direct writes into the live arp state and no whole-preset dump** are sent by the editor. Keep the encoders and
  their tests, unused.

## Required change — Presets page: show the current preset

Clicking a preset name loads it (verified end to end in the project chat: the real rack skin in the skin runtime, a
click at the name's screen position, the controller, the real engine: the edit buffer and the main display change to
the clicked preset). But the page never marks which preset is current, so a click shows no change on the glass and the
page looks unselectable (reported from the plugin). Change: the current preset's name is the page's focused item (its
cells blink like a focused value; names stay un-underlined), the page opens on the current preset's view, and it updates
when the preset changes (any route). Add a CI test that drives a click on a name through `SkinView` into the controller
(fake engine link) and checks the program change and the marking.

## Every open item, in one place (nothing else is known to be open)

| Item | This PR | Why / status |
|---|---|---|
| Arp settings, arp presets, on/off, keyboard | **editable** (above) | verified routes |
| Presets page marks the current preset | **fix** (above) | clicks load (verified end to end); marking missing |
| Sound common fields (pitch table, bend up/down, alt bus, GM alias, restrike, bend mode) | display-only | no verified live write path yet |
| Insert modulation (Common page 3), Insert and Reverb pages | display-only / later | per-algorithm parameters undecoded (V-15) |
| Global reverb mix | display-only | effect not measurable on the Template (V-14 remainder) |
| Oscillator trigger key (shows 144) | display as number | encoding undecoded |
| Two formatters (fine tuning 6, delay offset 1) and the number+parameter display (volume, semitone, octave) | plain signed numbers | not decoded |
| Sound settings MIDI channel range (reads 0..1) | use the list length (17) | header flag 0x80 not decoded |
| OSC and envelope Follow | from the panel LEDs (works) | OS selection bytes found (0x0BE2FE67, 0x0BE2FF7D, 0x0BE2FF24) but not decoded; LEDs stay |
| Window fits the screen (F-32) | **in this PR** | as specified |

## Not changed

Everything else verified above stays as it is. Re-verification in the project chat after the change: every arp setting and arp preset through the buttons while the
arp runs (keeps playing, no knob moves, the value lands, the safety rule holds), on/off and keyboard following the OS's flags and switching like the panel buttons, the Presets page marking the
current preset and loading on click, no dump sent, and the
bit-identical check again.
