# Preset library: the LIBRARY area (v0.8.3 step 2)

Name-free design document. Every name the library shows comes from the user's OS and the user's own files at run
time: no OS names, preset data or dumps are in the repository, its tests or its documents.

## Model

The library is a set of `.syx` files. The synth's memory stays as the OS keeps it, and browsing never overwrites a
stored preset.

- A preset clicked in the library goes into the OS's **Compare** buffer, exactly as a single-preset dump arriving over
  MIDI does. To keep it, the user presses **Save** on the panel, as on the hardware.
- The plugin writes `.syx` files only when the user asks (SAVE, EXPORT, IMPORT) and when it makes the factory bank files.
- Not in this version: writing a bank into the synth from the library, renaming or deleting files, sound dumps.
  Host program changes are unchanged.

## Verified OS behaviour (real OS, v0.8.3 build)

| What | Result |
|---|---|
| Single-preset dump received | Goes to Compare: the display keeps `P nn`, the Compare LED (0x16) lights, the edit buffer (#0) has the received preset's name, and the stored presets (0x40 + n) are untouched. A second one replaces it. |
| Single-preset dump | 3,097 bytes: `F0 0F 11 01 <dev> 02 17 7F 7F ...`. The slot is not in the message. |
| Bank dump | One 102,420-byte message: `F0 0F 11 01 <dev> 0D <bank> ...`, where byte 6 (counting F0 as byte 0) is 0 for Bank 1 (presets 1-32) and 1 for Bank 2 (33-64). |
| Bank received | Overwrites only its own 32 stored presets. |
| Panel Dump list | Press Dump (0x0C); +/Yes (0x0D) and -/No (0x0E) then step through `P 01` … `P 64`, `bAn1`, `bAn2`; pressing Dump again sends. The list starts at the current preset. After a dump the display shows `donE` for a while: a Dump pressed then only clears `donE` and enters no list (+/Yes then does nothing at `P 64`). Once the display shows `P nn` again, Dump enters the list. |
| Split | A preset split from a bank (received, then dumped from the panel) differs from the running machine's own dump of the same preset at byte 3090 (04 vs 00) and in the checksum: the OS sets that byte once it has received a bank or run factory restore since boot. Verified to have no effect: the edit buffer, all 4 sounds, a note's audio and the stored preset after Save are identical. The library keeps every message exactly as the OS sends it. |
| Boot condition | A machine booted with the factory user memory (as v0.8.3 boots) dumps the factory presets with one data byte different from a machine booted blank and then factory-restored. Hidden machines boot the v0.8.3 way. |

## Hidden machines (`phyzo-emu/src/library/hidden_machine.*`)

- **What they are:** machines of their own, outside the plugin's audio, the host's MIDI and the UI. They run on the
  library's worker thread, never on the audio thread.
- **How they are driven:** only through the OS:
  - panel presses (Dump, +/Yes; each held 110 ms, then 250 ms before the next);
  - program changes;
  - SysEx into the MIDI input;
  - the editor's partial reads for names (type 7, offset 8, 16 bytes; #0 is the edit buffer, 0x40 + n is stored
    preset n).
- **Waiting:** every wait is on the OS (its display, its MIDI output, its answers), bounded by a limit, never a fixed
  delay. Before every Dump that enters the Dump list, the machine waits until the display shows `P nn` (not `donE`).
- **Fresh machines:** boot with the factory user memory seeded before boot (Step 1). The first boot's state is kept,
  and later fresh machines are restored from it.
- **Clones:** restore the running machine's state, taken the same way as for the host (a snapshot at a block
  boundary). The running machine is never paused, pressed or read for a clone. A clone shows what the panel showed
  (for example `donE` right after a panel Dump), so every clone job first waits until its display shows `P nn`.
- **One job at a time:** in the whole process (instances take turns). Jobs are queued and can be cancelled, and they
  stop when the plugin closes. SAVE, EXPORT and IMPORT go to the front of the queue.

## Files (`phyzo-emu/src/library/library.*`, `syx.*`)

- **Folders:**
  - `<data folder>/Library/`: the library, with any subfolders. The data folder is `~/Documents/Phyzo/`.
  - `Library/Factory/Factory Bank 1.syx` and `Factory Bank 2.syx`: made the first time the LIBRARY area opens, or
    when one is missing. A fresh hidden machine dumps `bAn1` and `bAn2`. They are the user's own files, made from
    their own OS on their machine; they are never shipped or committed.
  - `Library/Singles/`: presets saved with SAVE, and imported single-preset files.
- **Reading a file:** it is split into SysEx messages. Each is a single preset (`02 17`), a bank (`0D`) or anything
  else (counted and ignored). MIDI bytes outside SysEx and real-time bytes inside one are skipped, and a truncated
  message is dropped.
  - **Single preset:** a fresh hidden machine receives it and the name is read from #0. The message bytes are kept as
    they are. A preset the OS does not take (no Compare LED, no name) is listed under its file's name as not accepted.
  - **Bank:** a fresh hidden machine receives it. Then, for each of the bank's 32 presets: a program change, the
    stored name (0x40 + n), and the panel's Dump. The result is kept as single-preset messages.
- **Index cache:** `<data folder>/cache/library/<file MD5>.idx` holds the names and single-preset messages, plus the
  OS image's MD5. A changed file is read again; an unchanged one never is.
- **Sources, in order:**
  1. FACTORY BANK 1 and FACTORY BANK 2;
  2. every bank or multi-preset file, by file name without `.syx`;
  3. SINGLES: every file that holds exactly one preset, wherever it is in the library.
- **File names:** the OS's name, trimmed, with the characters file systems reserve replaced by `_`. If the file
  exists, ` 2`, ` 3` … is added. A file is never overwritten.

## The LIBRARY area (approved mockup, with entries across the rows as on the PRESETS page)

- **Legends:**
  - row 1: LIBRARY (area), after REVERB;
  - row 2: SOURCES and PRESETS (sections);
  - row 3: SAVE, IMPORT, EXPORT B1, EXPORT B2 and FOLDER (`<legend action="library" value="SAVE">` and so on).
- **Behaviour:** PAGE 1-9, PREV and NEXT work as in the other areas.
- **Layout:** eight entries per page in two columns of 25, filled across the rows (01 02 / 03 04 / …) as on the
  PRESETS page. A bank is 4 pages.
- **SOURCES:** each source's title at the left of its half and its preset count at the right (the count ends 2 cells
  before the next half). While a source is being read, the count shows its progress (`12/32`); the factory rows show
  it while their files are made. Clicking a source opens it in PRESETS.
- **PRESETS:** `nn  name`, numbered in file order.
  - **Click:** sends that preset's stored single-preset message into the running machine's MIDI input, and nothing
    else (no program change, no write to stored memory). The OS puts it in Compare.
  - **Mark:** the clicked entry blinks, as the PRESETS area marks the current preset.
  - **Reading:** an opened source still being read shows `<title>  READING  n/32` until its presets are known.
- **Messages:** a message replaces row 4 for 3 seconds, then the entries come back. Messages from the library show one
  after another, and messages from while the editor was closed are dropped. They are cut to 50 characters; for a long
  name the number is dropped first, then the double space.

  ```
  02 Beta Arp IN COMPARE  SAVE ON PANEL TO KEEP
  07 Name  NOT ACCEPTED BY THE OS
  UNSAVED EDITS  SAVE ON THE PANEL FIRST
  SAVED  Singles/Beta Arp.syx
  EXPORTED BANK 1
  Bad.syx  NO PRESET OR BANK IN THIS FILE
  MAKING FACTORY BANKS (ONCE)  ABOUT 2 MINUTES
  IMPORTED 2 FILES
  ```
- **SAVE:** saves the current stored preset to `Library/Singles/<name>.syx`.
  - If the Compare LED is lit (unsaved edits, or a library preset in Compare), nothing is written and
    `UNSAVED EDITS  SAVE ON THE PANEL FIRST` shows.
  - Otherwise a clone dumps the current preset (panel Dump on `P nn`), and that message is written.
- **EXPORT B1 / B2:** a clone dumps `bAn1` / `bAn2`, so the export includes every preset saved on the panel. The clone
  first gets a program change to `P 64`, so `bAn1` and `bAn2` are the next two steps of the Dump list (from `P 01` it
  would be 64 steps, about 18 seconds). A save dialog follows; the default name is `Phyzo Bank 1.syx` /
  `Phyzo Bank 2.syx`.
- **IMPORT:** an open dialog for `.syx` files. Each file is copied into `Library/` (single-preset files into
  `Library/Singles/`), never overwriting, then read.
- **FOLDER:** opens `Library/` in Finder (or Explorer).
- **The area:**
  - rescans whenever it is opened;
  - while a job runs, its progress shows and everything else still works;
  - jobs keep running when the editor is closed;
  - the main panel's behaviour never changes because of it.

## Skin

The skin's editor document needs the new legends; the art has room for LIBRARY before ◀ PREV. Place them where the
art has the legend text:

```
<legend action="area" value="LIBRARY" style="left: …; top: 55dp; width: …;">LIBRARY</legend>
<legend action="section" value="SOURCES" style="left: …; top: 73dp; width: …;">SOURCES</legend>
<legend action="section" value="PRESETS" style="left: …; top: 73dp; width: …;">PRESETS</legend>
<legend action="library" value="SAVE" style="left: …; top: 91dp; width: …;">SAVE</legend>
<legend action="library" value="IMPORT" style="…">IMPORT</legend>
<legend action="library" value="EXPORT B1" style="…">EXPORT B1</legend>
<legend action="library" value="EXPORT B2" style="…">EXPORT B2</legend>
<legend action="library" value="FOLDER" style="…">FOLDER</legend>
```

The `library` legends are `avail` in the LIBRARY area only and are never `lit`.
