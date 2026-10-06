# Editor screens (F-30)

Name-free design documents for the editor unit, from the project chat:

- `EDITOR_SPEC.md`: the specification (revision 4).
- `EDITOR_PANEL_LAYOUT.md` / `.json`: the editor unit's geometry and every legend's click box and action. Where it
  differs from the specification, the layout is authoritative: the unit is 1795 x 394 dp and the window grows from
  848 to 1242 dp (the specification's "340 dp / 1188" line is out of date).
- `PR_B_ANSWERS.md`: the OS table layouts (descriptors, field records, value lists, the system table, the RAM wave list),
  the LED codes the rack skin passes to Follow, and the scope of step 2.
- `IMPLEMENTATION_STEPS.md`: the plan.

## What step 2 (read-only) implements

- `phyzo-emu/src/editor/`: messages (`editor_proto`), the OS tables read from the user's OS at run time
  (`editor_tables`), pages (`editor_pages`) and the controller (`editor_controller`): areas SYSTEM, PRESETS, SOUND and
  OSC. COMMON and ARP wait for the preset-level field list (PR_B_ANSWERS.md section 5); INSERT and REVERB for V-15.
- `Engine` (`setEditorOpen`, `editorSend`, `setEditorStandingRead`, `editorReplies`): the editor's messages enter the
  OS's MIDI input as whole messages at cable speed; the read of the visible page goes in directly behind every program
  change, bank select and SysEx dump; replies to the editor's own requests are taken out of the MIDI output, all other
  output passes. Nothing at all while the editor is closed.
- The skin runtime's `<vfdrow>` and `<legend>` elements, the Lua `editor` API and `<name>-editor.rml` (docs/SKINS.md).
- The plugin: right-click > Editor, the window growing by the unit's height, the open state saved with the project.

`editor_tables <OS image or update file>` (built with phyzo-emu) prints the tables as decoded, to check them against
the hardware; it writes nothing.

Assumptions to confirm with the real OS (project chat): note names with 60 = C4; names shown in capitals; the section
a field belongs to is chosen from its name (`edpages::oscillatorSection`, `soundSection`; `editor_tables` prints it).
