# Installing a Phyzo build on your Mac

Every push to `main` and every pull request builds the plugin on GitHub Actions (Apple Silicon, VST3 and AU,
ad-hoc signed). This guide takes you from that build to Phyzo loaded in REAPER.

## 1. Download the build from GitHub

You must be signed in to GitHub to download builds.

1. Open https://github.com/DustyHammers/Phyzo-/actions
2. In the left column, click **CI**.
3. Click the newest run with a green tick for the branch you want: `main` for merged work, or the pull request
   you are testing (its title is shown on the run).
4. On the run's page, scroll down to **Artifacts** and click the file named like
   `Phyzo-0.4.0-macOS-1a2b3c4.zip`. The last part is the commit it was built from.

From a pull request page you can also get there with the **Checks** tab, then **CI** on the left, then **Summary**.

Builds are kept for 30 days. If the newest run has a red cross, its plugin was not built; use an older green run.

## 2. Unzip

Double-click the zip in your Downloads folder (Safari may already have unzipped it). You get a folder containing
`Phyzo.vst3`, `Phyzo.component` and `INSTALL.txt`.

## 3. Install

1. Quit REAPER.
2. In Finder, choose **Go > Go to Folder…** (Shift-Command-G), paste `~/Library/Audio/Plug-Ins/VST3` and press Return.
   If Finder says the folder does not exist, go to `~/Library/Audio/Plug-Ins` instead and create a folder named `VST3`.
3. Drag `Phyzo.vst3` into it. If an older Phyzo is there, choose **Replace**.
4. Do the same for `Phyzo.component` with the folder `~/Library/Audio/Plug-Ins/Components`.

## 4. Clear the quarantine flag

macOS marks everything downloaded from the internet as "quarantined" and refuses to load plugins that are not
notarised by Apple. Phyzo is ad-hoc signed, not notarised, so the mark must be removed:

1. Open **Terminal** (Applications > Utilities > Terminal).
2. Type the following, **ending with a space**, and do not press Return yet:

       sudo xattr -r -d com.apple.quarantine 

3. Drag **Phyzo.vst3** from Finder onto the Terminal window. Terminal adds its exact location, whichever Library
   folder it is in. Press Return and type your Mac password if asked (nothing shows while you type).
   No message means it worked.
4. Repeat steps 2 and 3 for **Phyzo.component** if you installed the AU.
5. For the AU only: paste this line and press Return, so macOS re-reads its Audio Unit list
   ("No matching processes" is fine):

       killall -9 AudioComponentRegistrar

If Terminal prints "Not enough arguments for option -d", the space before the file location is missing.
If it prints "No such file", the path is wrong: drag the plugin onto Terminal instead of typing it.

Plugins work from either `~/Library/Audio/Plug-Ins/...` (your user folder) or `/Library/Audio/Plug-Ins/...` (all
users); Finder's Go to Folder can land in either.

Repeat step 4 every time you install a new build.

## 5. Load it in REAPER

1. Start REAPER. It scans new plugins at start-up.
2. Insert a track, click its **FX** button and type `Phyzo` in the filter box. You should see
   **VST3i: Phyzo (DHammers)** and an **AUi** entry for Phyzo.
3. If Phyzo is missing: **REAPER > Settings… > Plug-ins > VST**, click **Re-scan** (or **Clear cache/re-scan**), and
   restart REAPER. For the AU, check **Plug-ins > AU** is enabled.

If REAPER reports that the plugin failed to load, repeat step 4 and restart REAPER.

## 6. Put your ROM files in place

Phyzo looks for its two ROM files in one fixed folder: **Documents > Phyzo > roms**
(`/Users/<you>/Documents/Phyzo/roms/`). The plugin creates the folder the first time it opens. Copy the OS and the
native wave image into it. For the OS, either the decoded OS image or the original manufacturer's OS update file v1.10
as you have it (`.mid` or `.syx`) works: Phyzo decodes the update file in memory each time it starts, so no
conversion step is needed. File names do not matter: each file is recognised by its checksum. While a file is
missing, the plugin window says which one and checks the folder again every two seconds, so the synth starts by
itself a moment after you copy the files in; there is no need to reload the plugin.

## 7. Skins

The plugin window shows a **skin**. Your skins live in **Documents > Phyzo > skins**, one folder per skin: the
folder `rack` with `rack.rml` in it is the skin "rack" (with its .rcss, .lua, images and fonts). The compiled-in skin
**Built-in** is always available. Phyzo starts with "rack" if it is there, otherwise with Built-in.

Right-click anywhere in the window for the menu (the same in every skin):

- **Skin**: Built-in, then every skin in your skins folder (the current one is ticked).
- **Reload skin** (or press **F5** with the window focused): reads the skin again from disk after you edit it.
- **Zoom**: 50, 75, 100, 125, 133 (full size), 150, 175 or 200 % (the current size is ticked). You can also drag the
  window's bottom-right corner (the small grip) to any size from 50 to 200 %; the skin keeps its proportions. At
  133 % a file skin's art is shown at its native resolution; above that it is enlarged beyond it. 100 % is the size
  that used to be called 75 %; projects saved with the old sizes reopen at the same size on screen. The Built-in
  skin's sizes are unchanged. The window never gets larger than your screen: sizes that don't fit are greyed out,
  the corner drag stops at the screen's edge, and a project saved at a size that doesn't fit (for example with the
  editor unit open) opens at the largest size that does. On a 1080-point display the window with the editor unit fits
  up to about 110 %.
- **Editor**: attaches the editor unit below the panel (the window grows; the same item closes it; remembered per
  project). Available when the skin has an editor unit (`<name>-editor.rml`); greyed out otherwise.

In the editor unit:

- The legends choose what is shown: the area (SYSTEM, PRESETS, COMMON, SOUND, OSC, ARP), the section, the sound (S1-S4)
  and oscillator (O1/O2), the page (PAGE, PREV / NEXT). **FOLLOW** makes the editor follow the panel: the sound and
  oscillator being edited there, and, when you turn a panel knob, the page with that knob's value.
- **Edit a value**: click and hold it, then drag up or down (one step per small movement; hold Shift for fine steps).
  **Double-click** it to type a value (Enter sends, Esc cancels). **Right-click** it for the synth's list of values (or
  the waves, notes, arp presets) and **Revert** (back to the value before your edits since the preset loaded). With a
  value selected (it blinks), the arrow keys move to the next value and **+** / **-** step it. The mouse wheel never
  changes values.
- Values that can't be edited yet ignore the mouse. **EDITED** lights after an edit until another preset is loaded.
- **ARP**: the editor changes arp settings exactly as you would on the panel, with the panel's own buttons (the arp
  control's button, then -/No and +/Yes; the main display shows the setting meanwhile). It takes a second or two per
  setting; the arp keeps playing. It never moves a knob. Arp on/off and keyboard are switched with their own buttons.
  Don't touch the panel and don't play while it does this: any panel input or incoming MIDI stops the edit at once
  (nothing more is pressed). If the panel was in its normal mode, the editor returns it there afterwards (display and
  LEDs as before); if it was already in the arp menu, it stays. While the panel is in a sound, oscillator, MIDI or
  effects edit, arp settings can't be changed from the editor (a message says so and nothing is pressed). Any panel
  message shifts the arp's timing very slightly, as on the hardware.
- **PRESETS**: the current preset is marked (it blinks); click a preset's name to load it.
- The synth's main panel is never changed by the editor; save the preset on the panel as usual to keep your edits.
- **Developer > RmlUi debugger**: RmlUi's inspector, for working on a skin.

The skin is a global setting (every Phyzo window and project). The window size is remembered twice: globally, for
new instances, and in each project, so a project reopens at the size it was saved with.
If the chosen skin is missing or cannot be loaded, Phyzo shows "rack" instead (then Built-in) with a message saying
why. With **Developer > Write skin log** on (off by default), skin messages and script errors are written to
`~/Documents/Phyzo/skin-log.txt`. How to write a skin:
`docs/SKINS.md` in the repository.

In a skin: drag a knob up or down (hold Shift for fine steps) or use the mouse wheel; click a button to press it;
**Alt/Option-click** a button to latch it held until you click it again; **Esc** releases all latched buttons.
Right-click never presses anything. Knob positions are part of the synth's panel and are saved with the project.

The **Built-in** skin shows:

- The synth's 4-character display, drawn from the OS's segment data.
- **◀ −** and **+ ▶**: the synth's own −/No and +/Yes buttons; on the preset display they step to the previous or
  next preset. Holding one repeats, as on the hardware.
- **Status**: running, with the host sample rate, or a plain-language message if something stopped. While a ROM
  file is missing (or has the wrong checksum) it says which one and where to put it.
- **Debug**: CPU use per audio block (average and peak over the last second, one bar per second), the block size and
  length, the last block's processing time, and **overruns** (blocks that took longer than their real-time budget;
  any overrun can be heard as a dropout). The third line: the **worst block** time and the longest **audio lock
  wait** since playback started, and how many times the host asked for the plugin's **state** (REAPER does this
  after clicks in a plugin window). These reset when the host restarts audio. The fourth line splits the CPU use per block by part, as
  average/peak percent of the block's real-time budget over the last second: **68k+dev** (the synth's CPU running its
  OS, with timers, serial ports and DMA), **voice** (voice chip), **ESP2** (effects chip), **rate** (conversion from
  44.1 kHz to the host rate) and **queue** (passing knob, button and MIDI events to the synth).

Play Phyzo from a MIDI track. Everything the synth does (including the selected preset, any edits made over
MIDI, the knob positions and the presets you saved on the panel) is saved with the REAPER project and restored when
you open it.

**Saving presets** on the synth's panel works as on the hardware: the saved presets go to the synth's user memory,
which Phyzo keeps with the project (each project, and each Phyzo in it, has its own). A new Phyzo starts with the
factory presets only, like a new unit. **Factory restore** on the panel works too (hold Save, press Compare; the display
asks `rSt?`; press +/Yes; after a moment the first preset shows again): it puts the factory presets back in that Phyzo.
Sending the synth a bank dump over MIDI replaces its stored presets, as on the hardware. Projects saved with a
version before 0.8.3 open exactly as they did then; their synth can't save presets on the panel (it checks for that
only when it starts), so add a new Phyzo to save presets.

A new Phyzo takes about 0.2 s longer to start than in 0.8.2, because the synth indexes its user memory, as a new unit
does. So LFO, arpeggiator and random timings differ from 0.8.2 renders; every preset and sound is identical.

**Preset library** (five controls on the rack: LIBRARY, SAVE, IMPORT, EXPORT, FOLDER; see `docs/editor/LIBRARY.md`).
The library is the `.syx` files in **Documents > Phyzo > Library** (any subfolders).
- **LIBRARY:** shows the list of sources in the editor unit (opening the unit if it is closed): the factory banks, your
  bank files and SINGLES (every file with one preset). Click a source to see its presets; press LIBRARY again to go
  back to the list.
- **A preset:** click it to hear it. It plays in **Compare**, as a preset sent to the synth over MIDI does, and nothing
  in the synth's memory changes. Press **Save** on the panel to keep it in a slot.
- **SAVE, IMPORT, EXPORT and FOLDER** work from any editor page; their messages show on the glass's last row for
  3 seconds.
  - **SAVE:** writes the current stored preset to `Library/Singles/<name>.syx`. It refuses while the Compare LED is lit
    (save on the panel first).
  - **EXPORT:** a menu, Bank 1 (presets 1-32) or Bank 2 (presets 33-64); writes that bank, including your panel saves,
    to a file you choose.
  - **IMPORT:** copies `.syx` files into the library.
  - **FOLDER:** opens the Library folder.
- **The factory bank files:** the first time the area opens, Phyzo makes them once from your own OS (about 2 minutes,
  reading both banks included; the factory rows show the progress) in `Library/Factory/`. They're made from your OS, so don't share them.
- **Reading new files:** a new or changed file is read once by a hidden copy of the synth (a bank takes about half a
  minute); the result is kept in `cache/library/`.
- **Skin:** your skin's main document needs the five controls as empty legends over the art's lettering
  (`docs/editor/LIBRARY.md`, "Skin").

The first time Phyzo starts with your OS, it makes the factory presets once from the OS itself (a few extra seconds
while it boots) and keeps them in **Documents > Phyzo > cache**. That file is made from your
own OS, so don't share it. Deleting it is harmless: it is made again when needed.

## 8. MIDI ports

Each Phyzo instance opens a virtual MIDI port pair named **Phyzo** (a second instance: **Phyzo 2**, then
**Phyzo 3**). Other apps on the Mac see it as a MIDI device: send to the **Phyzo** destination to play or edit the
synth, receive from the **Phyzo** source to get what the synth sends (arpeggiator notes, knob CCs, SysEx replies and
dumps). MIDI from REAPER and from the virtual port are merged message by message; a SysEx dump stays whole.

The synth's MIDI output also goes to REAPER's MIDI out of the track when **Send MIDI to host** is on (off by default).
VST3 does not carry SysEx reliably; use the virtual port or the AU version for dumps.

Both are switched in the right-click menu under **MIDI**: **Virtual port "Phyzo"** (the name shows which port this
window has) and **Send MIDI to host**. They are remembered for all projects and apply to every open Phyzo.

## 9. Audio input

Phyzo has an optional stereo input, **Audio In**, that feeds the synth's effects processor the way the hardware's line
input did. The synth's own programs decide what to do with it (for example the vocoder presets); Phyzo does not
route it anywhere else. While the input is in use the plugin's latency grows a little (about 0.5 ms at 48 kHz) and
REAPER compensates for it. The **Input Clip** LED of a skin lights while the input reaches full scale.

To send a track (for example speech) into Phyzo in REAPER:

1. On the speech track, click **Route** and add a send to the Phyzo track.
2. In the send, set **Audio** to **1/2 → 3/4** (the speech arrives on the Phyzo track's channels 3/4).
3. On the Phyzo track, click **Route** and set the track to **4 channels**.
4. In Phyzo's FX window, click **2 in 2 out** (the pin connector) and connect **Audio In L / R** to channels **3 / 4**.
   Leave the outputs on 1/2.

## Uninstalling

Quit REAPER and move `Phyzo.vst3` and `Phyzo.component` (in `~/Library/Audio/Plug-Ins/...` or
`/Library/Audio/Plug-Ins/...`) to the Bin.
