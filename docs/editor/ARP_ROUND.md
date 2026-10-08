# ARP area: page order, riff, feel names, arp preset numbering (v0.8.3.5)

Name-free design document. Every name, label format and list the ARP area shows comes from the user's OS at run time:
no OS names or table contents are in the repository, its tests or its documents (the names below are invented).

## Page layout (owner-approved mockup)

The fields follow the order the panel's Arp Mode button steps through (PrSt, nrES, ndur, FEEL, the intervals, Lo n,
Hi n, tch, CEnt, ntLt, HELd). PATTERN and RIFF take the row under the preset's; the other arp controls (on/off,
keyboard, tempo, range up and down) follow, then the arp settings name. The labels are the OS's codes as before; RIFF
is the only new one. The arp preset, PATTERN, RIFF and FEEL take two grid columns (a whole half of the row).

```
ARP  page 1/2
PRST AP01 Kappa Steps     NRES 16th   NDUR 80
PATTERN Theta Run         RIFF NONE
FEEL 50 Lambda Feel       -3RD OFF    3RD  OFF
4TH  OFF    5TH  OFF      OCT  OFF    LO N A0

ARP  page 2/2
HI N C8     TCH  OFF      CENT OFF    NTLT 0
HELD On     ARP  OFF      KBD  OFF    TMPO t140
UP   Oct1   DOWN Oct1
NAME Kappa Steps
```

The grid has four columns per row (at 0, 12, 26 and 38); a two-column item starts a half, shows its label in full and
its value up to the half's end.

## The objects an arp preset refers to

| Arp block offset (preset offset) | What | OS object type | Shown |
|---|---|---|---|
| 390, word | note pattern | 9 | PATTERN: its name |
| 392, word | feel (groove template) | 10 | FEEL: its number and name |
| 394, word | riff (a stored note or controller sequence) | 11 | RIFF: its name |

`0xFFFF` means none. Names are read with a partial read (type, number = the value, offset 8, 16 bytes), once per
number, in the ARP area only.

- **PATTERN and RIFF** are display only. None shows `NONE`; a number the OS gives no name for shows the number.
- **FEEL** keeps its value, range (-1 = OFF to 999) and editing through the panel's buttons. It shows the number and
  the feel's name; OFF for none; the number alone when the OS has no name for it (no answer, or an empty name).

## Arp presets: numbering and the list

- **Numbering:** every arp preset is labelled with the OS's own number (the OS display counts from 0) through the OS's
  label format. The value shows the number and the arp preset's name.
- **The list ends where the OS stops answering name reads** (no count in the code). The names are read one at a time
  from 0 while the ARP area shows. When the OS does not answer one (3 s), the last answered one is read once more (the
  probe):
  - the probe answered: the OS answers, so the arp presets end there. No name past them is ever asked for again;
  - the probe not answered either: the OS answers nothing now (e.g. its demo), so no end is taken. The read waits for
    the OS's next answer to anything, then is tried again (never repeated every tick).
  The one read past the last arp preset is how the end is found; nothing after it is requested.
- **Range:** the arp preset field's drag, typed entry and right-click list go from the preset's own settings (`Snd`)
  to the last arp preset. Until the end is known they reach the last name answered (or the current arp preset, if
  higher).
- A pattern, riff or feel the OS gives no name for never holds the arp preset names up.
