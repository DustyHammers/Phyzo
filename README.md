Project-P
Research 

Startup log for troubleshooting: `~/Documents/Phyzo/Phyzo-log.txt` (see docs/INSTALL_MAC.md, Troubleshooting).

**Sync to host tempo** (right-click menu, MIDI): Phyzo sends the synth MIDI clock at the host's tempo, so the
arpeggiator, tempo-synced LFOs and noise, and tempo-based effects follow the project. While playing (the default)
sends clock only while the transport plays, so the panel Tempo works again when stopped; Always sends it whenever the
host has a tempo; Off sends none. While clock arrives, the Tempo display shows the host tempo and panel Tempo edits
don't stick, as on the original hardware. Details: docs/INSTALL_MAC.md, section 8.

## What's new

- 0.8.7: Sync to host tempo (MIDI clock from the host's playhead; Off, While playing, Always; saved with the project).
- 0.8.6: startup log, the panel answers while the host is not playing (idle runner), universal build (Apple Silicon and
  Intel), CI builds the plugin only on a manual run.
