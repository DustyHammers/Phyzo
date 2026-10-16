# Preset library (v0.8.3 step 2; the plugin window's LIBRARY tab since v0.9.0)

Name-free design document. Every name the library shows comes from the user's OS and the user's own files at run
time: no OS names, preset data or dumps are in the repository, its tests or its documents.

## Model

The library is a set of `.syx` files. The synth's memory stays as the OS keeps it, and browsing never overwrites a
stored preset.

- A preset clicked in the library goes into the OS's **Compare** buffer, exactly as a single-preset dump arriving over
  MIDI does. To keep it, the user presses **Save** on the panel, as on the hardware.
- The synth's memory is written only by a **bank load** the user confirmed (v0.9.0): a bank file into Bank 1 or Bank
  2, backed up first.
- The plugin writes `.syx` files only when the user asks (Save, Save as file, Import, Rename, Delete, a bank load's
  backup) and when it makes the factory bank files.
- Not in this version: sound dumps. Host program changes are unchanged.

## Verified OS behaviour (real OS, v0.8.3 build)

| What | Result |
|---|---|
| Single-preset dump received | Goes to Compare: the display keeps `P nn`, the Compare LED (0x16) lights, the edit buffer (#0) has the received preset's name, and the stored presets (0x40 + n) are untouched. A second one replaces it. |
| Single-preset dump | 3,097 bytes: `F0 0F 11 01 <dev> 02 17 7F 7F ...`. The slot is not in the message. |
| Bank dump | One 102,420-byte message: `F0 0F 11 01 <dev> 0D <bank> ...`, where byte 6 (counting F0 as byte 0) is 0 for Bank 1 (presets 1-32) and 1 for Bank 2 (33-64). |
| Bank received | Overwrites only its own 32 stored presets. Rewriting byte 6 loads a bank file into the other bank (v0.9.0, verified by the owner). If the playing preset is in that bank its edit buffer is replaced; otherwise unsaved edits are kept. |
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
  stop when the plugin closes. Save, Save as file, Import and a bank load go to the front of the queue.

## Files (`phyzo-emu/src/library/library.*`, `syx.*`)

- **Folders:**
  - `<data folder>/Library/`: the library, with any subfolders. The data folder is `~/Documents/Phyzo/`.
  - `Library/Factory/Factory Bank 1.syx` and `Factory Bank 2.syx`: made the first time the LIBRARY tab opens, or
    when one is missing. A fresh hidden machine dumps `bAn1` and `bAn2`. They are the user's own files, made from
    their own OS on their machine; they are never shipped or committed.
  - `Library/Singles/`: presets saved with Save, and imported single-preset files.
  - `Library/Backups/`: `Bank N YYYY-MM-DD HHMM.syx`, the bank as it was before a bank load (v0.9.0).
- **Reading a file:** it is split into SysEx messages. Each is a single preset (`02 17`), a bank (`0D`) or anything
  else (counted and ignored). MIDI bytes outside SysEx and real-time bytes inside one are skipped, and a truncated
  message is dropped.
  - **Single preset:** a fresh hidden machine receives it and the name is read from #0. The message bytes are kept as
    they are. A preset the OS does not take (no Compare LED, no name) is listed under its file's name as not accepted.
  - **Bank:** a fresh hidden machine receives it. Then, for each of the bank's 32 presets: a program change, the
    stored name (0x40 + n), and the panel's Dump. The result is kept as single-preset messages.
- **Index cache:** `<data folder>/cache/library/<file MD5>.idx` holds the names and single-preset messages, plus the
  OS image's MD5. A changed file is read again; an unchanged one never is.
- **Sources, in order** (the window's groups):
  1. FACTORY: the two factory bank files;
  2. BANKS: every other bank or multi-preset file, by its name without `.syx` (a file in a subfolder as
     `folder/name`), case ignored;
  3. SINGLES: every file that holds exactly one preset, wherever it is in the library (one entry);
  4. BACKUPS: the files in `Library/Backups/`, by name (the group is left out while there are none).
- **File names:** the name typed (Save) or the OS's name, trimmed, with the characters file systems reserve replaced
  by `_`. If the file exists, ` 2`, ` 3` … is added. A file is never overwritten.

## The LIBRARY tab (v0.9.0, `plugin/Source/PhyzoWindow.*`)

The plugin window's LIBRARY tab replaces the editor unit's LIBRARY area of v0.8.3-v0.8.7. It works whether the editor
unit is open or not, and with any skin (Built-in included). Opening the tab rescans the library; the factory files
are made the first time.

- **FILES** (220 dp): the groups FACTORY, BANKS, SINGLES and BACKUPS. Each row: an accent tag, the name and the count
  at the right (`32`, or `12/32` while the file is read; the factory rows show their progress while they are made).
  At the bottom: the Library folder's path, **Import…** (an open dialog for `.syx` files: each is copied into
  `Library/`, single-preset files into `Library/Singles/`, never overwriting, then read) and **Folder** (Finder).
  - Right-click a bank or backup row: **Rename…**, **Delete**, **Show in Finder**. A factory row or the Singles row:
    **Show in Finder** only.
- **PRESETS** (the rest): the open file's name, then its presets in two columns of 16 (`nn  name`), 32 to a page (the
  wheel turns pages). Singles are listed by file name.
  - **Click:** the preset's stored single-preset message goes into the running machine's MIDI input, and nothing
    else (no program change, no write to stored memory). The OS puts it in Compare. An info dot marks the entry while
    the Compare LED is lit.
  - A preset the OS did not take is shown muted; clicking it says `nn  name  NOT ACCEPTED BY THE OS`.
  - Right-click a single: **Rename…**, **Delete**, **Show in Finder**.
- **MEMORY · 2 BANKS** (300 dp): Bank 1 and Bank 2, each with an LED (lit while the playing preset, the display's
  `P nn`, is in that bank), its title (the file last loaded into it in this project, else `Presets 1–32` /
  `Presets 33–64`) and a muted detail (`presets 1–32 · playing P05`). Under each: **Load…** and **Save as file…**.
  The last-loaded names are saved with the project (processor state version 8; older projects: none).
- **Footer:** the window's messages (the library's, MIDI learn's), confirmations, progress and the Save name box,
  whatever tab shows. Messages arriving while no window shows them wait for it.

### Save with a name (the rack legend SAVE)

- The window opens on LIBRARY. If the Compare LED is lit (unsaved edits, or a library preset in Compare):
  `UNSAVED EDITS  SAVE ON THE PANEL FIRST`, and nothing more.
- Otherwise a clone checks the LED again, reads the current preset's number and name and dumps it; the footer asks
  `Save Pnn to Singles as [name] .syx` with **Save** and **Cancel**. The box starts with the OS's name, trimmed, and
  has the focus; Enter saves, Esc or Cancel cancels.
- The name is made safe (`_` for reserved characters); an empty name is the OS's name; an existing file is never
  overwritten (` 2` …). The file holds the dump's bytes unchanged. Then `SAVED  Singles/<name>.syx`.

### Rename and Delete

- **Rename** is inline: Enter commits, Esc (or leaving the box) cancels. Only the file name changes (the folder
  stays); it is made safe as Save's; an existing name is refused with a footer message. The index is keyed by the
  file's MD5, so nothing is read again.
- **Delete** asks in the footer: `Delete <name>? It goes to the Trash.` with **Delete** and **Cancel**; the file is
  moved to the Trash.
- The factory files are never renamed or deleted.

### Load a bank into Bank 1 or Bank 2

1. **Load…** opens a menu: the header `Load into Bank N`, every bank file (factory, banks, backups), then **Other
   file…** (an open dialog; the file must hold exactly one bank, else `<file> has no bank`).
2. The footer asks: `Load "<file>" into Bank N (presets a–b)? Bank N is backed up first.`, adding
   `Pnn will change; unsaved edits on it will be lost.` when the playing preset is in Bank N; **Load** and **Cancel**.
3. **Backup:** a clone dumps Bank N to `Library/Backups/Bank N YYYY-MM-DD HHMM.syx` (never overwriting). If the
   backup fails, nothing is loaded. The footer shows `Backing up Bank N…` with **Cancel** (cancelling sends nothing).
4. **Load:** the file's bank message with byte 6 set to the target bank and nothing else changed goes into the
   running machine's MIDI input the way host SysEx does (`Engine::libraryInput`): whole, at cable speed (about 33 s),
   with MIDI arriving meanwhile waiting behind it as on a cable. The room for it is made beforehand: the audio thread
   neither allocates nor waits. The footer shows `Loading <file> into Bank N…` (no Cancel).
5. **Done** when Bank N's 32 stored names (read in a clone) equal the file's names from its index:
   `LOADED <file> INTO BANK N`, the Memory column's title, and the new backup in BACKUPS. If they do not match within
   30 s after the OS has read the whole message (at most 120 s for that): `Bank N didn't confirm the load`.
6. One bank job at a time; audio and the panel keep working meanwhile.

### Save as file (EXPORT)

For the bank, a clone dumps `bAn1` / `bAn2`, so the file includes every preset saved on the panel. The clone first
gets a program change to `P 64`, so `bAn1` and `bAn2` are the next two steps of the Dump list (from `P 01` it would be
64 steps, about 18 seconds). A save dialog follows; the default name is `Phyzo Bank 1.syx` / `Phyzo Bank 2.syx`.

### Messages

```
nn  name  NOT ACCEPTED BY THE OS
UNSAVED EDITS  SAVE ON THE PANEL FIRST
SAVED  Singles/Beta Arp.syx
SAVED BANK 1  Phyzo Bank 1.syx
LOADED My Bank INTO BANK 1
Bank 2 didn't confirm the load
Bad.syx  NO PRESET OR BANK IN THIS FILE
MAKING FACTORY BANKS (ONCE)  ABOUT 2 MINUTES
IMPORTED 2 FILES
```

## Skin

A skin reaches the library through two **rack legends** in its main document (`docs/SKINS.md`), usually empty
legends over lettering in the art:

```
<legend action="rack" value="EDITOR" style="position: absolute; left: …; top: …; width: …; height: …;"/>
<legend action="rack" value="SAVE" style="…"/>
```

- `EDITOR` shows or hides the editor unit (available when the skin has one, `lit` while it shows).
- `SAVE` opens the window's LIBRARY tab with Save with a name (always available, never `lit`).
- The v0.8.3-v0.8.7 `action="library"` legends are obsolete: never available, a click does nothing, and the skin log
  says so once per load. Everything else of the library is in the window (right-click the background).
