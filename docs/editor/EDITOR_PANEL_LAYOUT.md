# Editor unit — exact layout (F-30), revision 4, 2026-10-05

Art at 2x, laid out at half size, as the rack skin. Positions in dp from the editor unit's top-left corner; the unit sits directly below the rack, so in the window add 848 dp to every y. Art 3590 × 788 px, layout 1795 × 394 dp; the window grows from 848 to 1242 dp.

**Files:** `EDITOR_PANEL_BLANK.png` (rack ears, black bands and purple face only, for GIMP), `EDITOR_PANEL_MOCKUP.png` (finished look), `EDITOR_PANEL_POSITIONS.png` (transparent guide layer, same size as the blank), `EDITOR_PANEL_LAYOUT.json` (machine-readable copy, including every legend's click box).

**Revision 4:** no buttons at all. Every control is a legend on the glass, including ◀ PREV and NEXT ▶, centred in the top legend row. The glass spans the face (25 dp from each ear): 50 × 4 cells (two halves of 25) at the same character size as revision 3 (22 × 39 dp).

## Frame

- Rack ears: left 0–106 dp, right 1688.5–1795 dp; mounting holes 29–49 dp from the top and from the bottom, as on the rack.
- Black bands: top 0–24 dp, bottom 364–394 dp, between the ears.
- Purple face: x 106–1688.5, y 24–364 dp.

## VFD glass

- Window: left 131, top 40, 1532 × 308 dp, corner radius 16 dp; centred on the face both ways.
- Character cells: 50 columns × 4 rows. Cell (column c, row r) top-left = (149 + 30·c, 118 + 55·r) dp; glyph 22 × 39 dp, slant 0.11 (top edge leans right ≈ 4.3 dp); underline 44.7–47.6 dp below the cell top; decimal point at the lower right.
- Legend rows: tops at y 56, 74, 92 dp, 13 dp condensed bold. Row 1: areas from the left, ◀ PREV / NEXT ▶ centred on the glass, FOLLOW / EDITED / DATA at the right. Rows 2–3: sections from the left; S1–S4, O1, O2 and PAGE 1–9 at the right.
- ◀ PREV and NEXT ▶ are lit when there is a page to go to, ghosted otherwise.

## Legends (click boxes, dp)

| Legend | Row | Click does | left | top | width × height |
|---|---:|---|---:|---:|---|
| SYSTEM | 1 | go to area | 143 | 55 | 61.0 × 18 |
| PRESETS | 1 | go to area | 220.0 | 55 | 66.5 × 18 |
| COMMON | 1 | go to area | 302.5 | 55 | 69.5 × 18 |
| SOUND | 1 | go to area | 388.0 | 55 | 55.5 × 18 |
| OSC | 1 | go to area | 459.5 | 55 | 35.0 × 18 |
| ARP | 1 | go to area | 510.5 | 55 | 34.5 × 18 |
| INSERT | 1 | go to area | 561.0 | 55 | 55.0 × 18 |
| REVERB | 1 | go to area | 632.0 | 55 | 60.0 × 18 |
| ◀ PREV | 1 | previous page | 831.2 | 55 | 55.5 × 18 |
| NEXT ▶ | 1 | next page | 906.8 | 55 | 56.0 × 18 |
| FOLLOW | 1 | toggle Follow | 1458.0 | 55 | 63.5 × 18 |
| EDITED | 1 | indicator only | 1533.5 | 55 | 55.5 × 18 |
| DATA | 1 | indicator only | 1601.0 | 55 | 42.0 × 18 |
| WAVE | 2 | go to section | 143 | 73 | 45.5 × 18 |
| PITCH | 2 | go to section | 200.5 | 73 | 47.5 × 18 |
| FILTER | 2 | go to section | 260.0 | 73 | 50.5 × 18 |
| AMP | 2 | go to section | 322.5 | 73 | 37.5 × 18 |
| NOISE | 2 | go to section | 372.0 | 73 | 48.5 × 18 |
| LFO | 2 | go to section | 432.5 | 73 | 33.5 × 18 |
| ENV1 | 2 | go to section | 478.0 | 73 | 43.0 × 18 |
| ENV2 | 2 | go to section | 533.0 | 73 | 43.0 × 18 |
| ENV3 | 2 | go to section | 588.0 | 73 | 43.0 × 18 |
| GEN | 2 | go to section | 643.0 | 73 | 35.5 × 18 |
| S1 | 2 | edit this sound / oscillator | 1465.0 | 73 | 24.5 × 18 |
| S2 | 2 | edit this sound / oscillator | 1491.5 | 73 | 24.5 × 18 |
| S3 | 2 | edit this sound / oscillator | 1518.0 | 73 | 24.5 × 18 |
| S4 | 2 | edit this sound / oscillator | 1544.5 | 73 | 24.5 × 18 |
| O1 | 2 | edit this sound / oscillator | 1589.0 | 73 | 26.0 × 18 |
| O2 | 2 | edit this sound / oscillator | 1617.0 | 73 | 26.0 × 18 |
| TRACK | 3 | go to section | 143 | 91 | 52.0 × 18 |
| RANGE | 3 | go to section | 207.0 | 91 | 53.5 × 18 |
| RECEIVE | 3 | go to section | 272.5 | 91 | 63.0 × 18 |
| SLOTS | 3 | go to section | 347.5 | 91 | 50.0 × 18 |
| KNOBS | 3 | go to section | 409.5 | 91 | 54.0 × 18 |
| MOD | 3 | go to section | 475.5 | 91 | 39.5 × 18 |
| ALGO | 3 | go to section | 527.0 | 91 | 44.0 × 18 |
| SENDS | 3 | go to section | 583.0 | 91 | 52.5 × 18 |
| MIDI | 3 | go to section | 647.5 | 91 | 38.0 × 18 |
| PAGE | 3 | label only | 1439.0 | 91 | 42.0 × 18 |
| 1 | 3 | go to page | 1483.0 | 91 | 16.0 × 18 |
| 2 | 3 | go to page | 1501.0 | 91 | 16.0 × 18 |
| 3 | 3 | go to page | 1519.0 | 91 | 16.0 × 18 |
| 4 | 3 | go to page | 1537.0 | 91 | 16.0 × 18 |
| 5 | 3 | go to page | 1555.0 | 91 | 16.0 × 18 |
| 6 | 3 | go to page | 1573.0 | 91 | 16.0 × 18 |
| 7 | 3 | go to page | 1591.0 | 91 | 16.0 × 18 |
| 8 | 3 | go to page | 1609.0 | 91 | 16.0 × 18 |
| 9 | 3 | go to page | 1627.0 | 91 | 16.0 × 18 |

## Notes for the art

- The skin draws everything that changes (legends lit, ghosted or hovered, characters, underlines) on top; the art only needs the face, ears, bands and, optionally, the glass window.
- The main panel above is untouched; this unit is a separate image and a separate part of the skin.
