# Writing a Phyzo skin

A skin is the plugin window: an RmlUi document (RML, RCSS, Lua and images) that draws the front panel and binds
it to the synth. Skins are not part of this repository; each lives in its own folder on your Mac:

    ~/Documents/Phyzo/skins/<name>/<name>.rml      (plus its .rcss, .lua, images and fonts)

Every folder in `~/Documents/Phyzo/skins/` that contains `<folder name>.rml` is a skin and appears in the plugin
window's **SKIN** tab (right-click the background to open the window). The compiled-in skin **Built-in** is always
there too.

RmlUi's own documentation covers RML and RCSS: https://mikke89.github.io/RmlUiDoc/. This page describes only what
Phyzo adds.

## Files

- `href` and `src` are plain file names inside the skin folder (`rack.rcss`, `knob.png`). Sub-folders and paths
  outside the folder are refused (the refusal is logged).
- Images: PNG, JPEG or GIF.
- Fonts: every `.ttf` and `.otf` file in the skin folder is loaded when the skin loads. Use the font's family
  name in `font-family`.
- Scripts: `<script src="rack.lua"></script>` or inline `<script>`.
- The UI font (v0.9.0, optional): `<meta name="ui-font" content="PhyzoPanelExtended-SemiBold.ttf"/>` in the main
  document's `<head>` names a font file in the skin folder for the plugin window (SYSTEM, LIBRARY, SKIN, MIDI) and
  its menus. It is loaded when the skin loads and when it is reloaded (F5). Without it, when the file is missing (one
  skin-log line says so), or with Built-in, the window uses JUCE's default sans. RmlUi itself ignores the tag; the
  skin's own text uses `font-family` as before.

## Size and zoom

The document body is **1795 × 848 dp**. At 100 % the window is 3/4 of that in screen points (1346 × 636); at
**133 % (full size)** it is 1795 × 848 points, so on a Retina screen 1 dp = 2 px and art drawn at twice the dp size
(`resolution: 2x` in a `@spritesheet`) is shown at its native resolution. Below 133 % the art is scaled down; above
133 % it is enlarged beyond its native resolution. The size is 50-200 %, set by dragging the window's corner or with
Zoom in the plugin window's SKIN tab (50, 75, 100, 125, 133, 150, 175, 200 %). The dp ratio follows the window continuously; RmlUi
redraws the art at each size (no bitmap stretching of the window). The bottom-right corner (24 screen points) is the resize grip: keep controls out of it.

## The editor unit (optional)

A skin can have an editor unit (F-30, docs/editor/): `<name>-editor.rml` in the skin folder, a second document
**1795 × 394 dp** shown directly below the body when the user switches on **Editor unit** in the plugin window's SKIN
tab or clicks a `rack` legend `EDITOR` (the window grows from 848 to 1242 dp at the same scale). Without that file the
switch is greyed out. It shares the skin's Lua environment;
the geometry of the glass and every legend is in `docs/editor/EDITOR_PANEL_LAYOUT.md`.

- `<vfdrow row="0">` … `row="3"`: one row of the glass, 50 character cells. The element creates the cells:
  `<span class="cell">` holding `<span class="chr">` (sprite `<spriteprefix><NNN>` with NNN the character's ASCII code,
  e.g. `chr_065` for A; no sprite for a space), `<span class="ul">` (underline) and `<span class="dp">` (decimal point),
  the last two with class `lit` when on. A cell showing a space has class `ghost`; the focused value's cells have class
  `focus`, plus `blink` during the off half of the 2.5 Hz blink. Set `spriteprefix` on the `vfdrow` and lay the cells out
  in RCSS (cell pitch 30 dp, row pitch 55 dp in the layout table).
- `<legend action="…" value="…">`: a legend; actions and values as in `EDITOR_PANEL_LAYOUT.json` (`area`, `section`,
  `target`, `page`, `page_prev`, `page_next`, `toggle_follow`, `indicator`, `label`). Class `lit` when active, `avail`
  when clicking it leads somewhere; style hover with `:hover`. A click goes to the editor.
- Rack legends (v0.9.0): `<legend action="rack" value="EDITOR">` and `<legend action="rack" value="SAVE">`, usually
  in the **main (rack) document** as empty legends (invisible click areas over lettering in the art); they work in the
  editor document too. They do not depend on the editor's state or on any area of it:
  - `EDITOR` is `avail` when the skin has an editor unit and `lit` while it shows; a click shows or hides it.
  - `SAVE` is always `avail` and never `lit`; a click opens the plugin window on LIBRARY with Save with a name
    (`docs/editor/LIBRARY.md`).
  - `editor.click('rack', 'SAVE')` in Lua does the same as the click.
- The v0.8.3-v0.8.7 library controls (`<legend action="library" value="…">`) are obsolete: never `avail` or `lit`, a
  click does nothing, and the skin log notes once per load: `legend action 'library' is obsolete, use action='rack'
  value='EDITOR' / 'SAVE'`. The library is in the plugin window's LIBRARY tab.
- Lua: `editor.isOpen()`, `editor.state()` (area, section, sound, osc, oscBoth (both oscillators edited), page, pages, follow, data, rows), `editor.click(action,
  value)`, `editor.onChange(fn)` (called when the glass or the legends change), and
  `editor.followLeds{sound = {…4 codes}, osc = {…2}, env = {pitch, filter, amp}, noise = c, lfo = c, filterKbd = c}`: the
  panel LEDs Follow watches (the rack skin's: sound 0x0B, 0x13, 0x03, 0x07; osc 0x06, 0x15; env 0x11, 0x0C, 0x0E; noise
  0x02, lfo 0x0F, filterKbd 0x08; the last three are optional and pick the page when Mod Speed or Filter Amount moves).
- `editor.knobLabels{"P", "H", "Y", "Z", "O"}`: the five performance knobs' labels on the COMMON page's KNOBS section:
  exactly 5 strings in perf1-perf5 order (perf1 = control 9, perf2 = 13, perf3 = 6, perf4 = 5, perf5 = 3), at most 4
  characters each (anything else is a Lua error and changes nothing). Call it from the editor unit's `onload`, like
  `editor.followLeds`. Without it the labels are PERF1-PERF5.
- `editor.state()` also has `edited` (an edit since the preset loaded) and `typing` (typed entry in the focused value).
- Editing needs nothing from the skin: the values on the `<vfdrow>` glass take the mouse (drag up/down, Shift fine;
  double-click for typed entry; right-click for Revert (when the value was edited) and "Open window…", else the plugin
window; a click
  on a preset name loads it). The
  mouse wheel never changes a value.
- Wave names (optional): `waves.txt` in the skin folder, one line per wave: number, a tab, the name. The wave value then
  shows "number name"; without the file, the number only.

The editor reads from the synth only while its unit is shown; with it closed, nothing is sent.

## Panel elements

### `<knob cc="N">`

An analog panel control (cc 0-25, see `phyzo-emu/docs/PANEL_CONTROLS.md`).

- RCSS properties: `frames: 61;` (number of frames) and `spriteprefix: knob_;`. The knob shows the sprite
  `<spriteprefix><NNN>` with NNN the three-digit frame number (`knob_000` … `knob_060`), through its `decorator`.
- Value: raw 0-1023. Frame = round(raw × (frames − 1) / 1023).
- Mouse: vertical drag (up = more; full range over 256 dp; Shift = a tenth as fast); mouse wheel (16 per notch,
  Shift: 1).
- It sends one panel message `Bx cc vv` (the absolute raw value) only when it is moved, never on its own.
- Several knobs may share a cc (stacked knobs, e.g. one per Envelope / Modulation / Filter page). Each knob element
  keeps its own position, stored by its `id` in the project. Give every knob a unique `id`. A knob seen for the
  first time starts at the value last sent for its cc (the synth's fresh positions in a new instance).
- When the synth boots it is told, for each cc, the value last sent for that cc.
- Right-click on a knob (cc 0-22) opens MIDI learn's menu for its panel control, then "Open window…"
  (docs/MIDI_LEARN.md); stacked knobs are one control. A knob a learned controller moves shows the new position as
  `panel.setKnob` would (the visible knob of that cc; positions saved; `panel.onKnob` is called), but nothing is
  sent again. The wheels and pressure (cc 23-25) are never learnable.

### `<pbutton raw="0xNN" led="0xNN">`

A front-panel button by its panel id. `led` is optional.

- Left mouse button down sends `81 raw`, release sends `80 raw` (also when released outside the button). Class
  `pressed` while down.
- Alt/Option-click: latching hold. It sends `81 raw` and keeps the button held (class `held`) until it is
  clicked again. Esc releases all held buttons.
- Right-click never presses (it opens MIDI learn's menu for this panel button, then "Open window…",
  docs/MIDI_LEARN.md).
- With `led`: class `lit` follows that LED (see LEDs).

### `<led code="0xNN">` and `<led source="audioclip">`

Class `lit` follows the LED. `audioclip` is the Input Clip LED: lit while the plugin's Audio In reaches the synth's
clip level (24-bit word at or beyond 0x7FFFF0), held 100 ms; unlit while Audio In is off.

### `<vfdigit pos="0..3">`

One display digit: pos 0 is the leftmost (panel message 96), pos 3 the rightmost (93). RCSS property
`spriteprefix: vfd_;`. The element shows the sprite `<spriteprefix><NNN>` with NNN = the OS's segment byte & 0x7F
(three digits, `vfd_000` … `vfd_127`). Segment bits 0-6: g (middle), c, b, a (top), f, e, d (bottom).

### LEDs

The synth's OS sets each LED with a panel message: `91 code` on, `90 code` off, `92 code` flash, `9D code rate` beat
flash. Class `lit` is set:

| OS state | `lit` |
|---|---|
| on | always |
| off | never |
| flash | blinking at 2.5 Hz, 50 % duty; all flashing LEDs in step |
| beat flash | for min(100 ms, half the time since the previous `9D` for that LED), restarted by each `9D` |

(The flash and beat timings are assumptions, kept as named constants in `plugin/skin/src/skin_view.h`.)

## Lua

Scripts run in Lua 5.4 with the base (without `dofile` and `loadfile`; `load` accepts text only), coroutine,
table, string, math and utf8 libraries; no file, OS or debug access. Any single entry into Lua (an event handler,
a callback, a frame) may run for at most 3 seconds and is then stopped. Errors never stop the plugin; they are
written to the skin log.

Each skin window has its own global variables; two Phyzo instances never see each other's.

### `panel`

| Function | Does |
|---|---|
| `panel.led(code)` | 0 off, 1 on, 2 flash (beat flash counts as flash) |
| `panel.onLed(code, fn(state))` | calls `fn` when the LED's state (0/1/2) changes |
| `panel.onBeat(code, fn())` | calls `fn` on every beat-flash message for the LED |
| `panel.knob(cc)` | the value last sent for the cc (0-1023) |
| `panel.setKnob(cc, raw)` | sends the value and moves the visible knob(s) of that cc |
| `panel.onKnob(cc, fn(raw))` | calls `fn` whenever a value is sent for the cc (mouse or Lua) |
| `panel.press(raw)`, `panel.release(raw)` | button down / up (`81 raw` / `80 raw`) |
| `panel.isHeld(raw)` | true while the button is latched (Alt-click) |
| `panel.onButton(raw, fn(down))` | calls `fn` for every press and release: mouse, latching hold and Lua |
| `panel.display()` | the four segment bytes, leftmost first |
| `panel.onDisplay(fn(b0, b1, b2, b3))` | calls `fn` when the display changes (and once when the skin loads) |

### `plugin`

`plugin.name` ("Phyzo"), `plugin.vendor` ("DHammers"), `plugin.version` (e.g. "0.4.0").

### Document events and RmlUi's Lua API

- `<body onload="...">` runs once the document is loaded; `<body onframe="...">` runs every displayed frame, at most
  60 times a second; any element with `onshow="..."` gets a `show` event when the skin is shown.
- Frames are drawn only when something changed: an LED, the display, a knob, input, a flashing or beat LED's next
  step, or an RmlUi animation or transition. A skin with a non-empty `onframe` is drawn up to 60 times a second for as long as it
  is shown (use it only for animation the panel cannot express). Nothing is drawn while the window is hidden,
  minimised or covered.
- The panel is read once per frame and only its latest state counts: `panel.onLed`, `onDisplay`, `onKnob` and
  `onButton` are called only when the value changed, never once per panel message.
- The skin log notes the UI rates for each second in which the panel was active (panel messages, panel changes,
  frames drawn, Lua callbacks) and what asked for the frames: input, a panel change, a flashing or beat LED, `onframe`,
  an RmlUi animation or transition (an `animation` with `infinite` in the skin's RCSS redraws for as long as the skin
  is shown), or JUCE/the system on its own.
- Inline event code receives `event`, `element` and `document`. Pass `document` on if a function in a script file
  needs it: `<body onload="onLoad(document)">`.
- RmlUi's Lua API is available (`document:GetElementById`, `element:SetClass`, `element.inner_rml`, …), and
  `Log.Message(Log.logtype.info, "text")` writes to the skin log.

Example: stacked knobs that follow the page LEDs.

```lua
local pages = { [0x20] = "env", [0x21] = "mod", [0x22] = "flt" }   -- LED code -> knob id suffix

function showPage(doc)
  for code, page in pairs(pages) do
    doc:GetElementById("cutoff_" .. page):SetClass("hidden", panel.led(code) == 0)
  end
end

function onLoad(doc)
  for code in pairs(pages) do panel.onLed(code, function() showPage(doc) end) end
  showPage(doc)
end
```

## Working on a skin

- Right-click the background (anywhere that is not a learnable control) to open the plugin window; Esc, its round
  close button or a click on the dimmed area around it closes it. It is drawn 1000 x 610 dp of the skin (smaller only
  when the plugin area less 14 dp on each side is too small), centred on the panel (and the editor unit), over a
  backdrop that dims the rest; it never changes the window's size. While it is open the skin gets no mouse events,
  except in the bottom-right 24-point corner (the resize grip).
- **F5** (with the plugin focused, the window open or not) or **Reload skin (F5)** in the SKIN tab reloads the skin
  from disk, its UI font included; knob positions are kept.
- **RmlUi debugger** (SYSTEM tab, Developer; file skins only) shows RmlUi's element inspector and event log.
- **Write skin log** (SYSTEM tab, Developer; off by default; a global setting for all instances) writes the skin log,
  `~/Documents/Phyzo/skin-log.txt`: warnings (missing files, CSS problems), Lua errors with their traceback,
  `Log.Message` output and the UI rates. Turn it on while writing a skin; with it off no file is created.
- If a skin is missing or cannot be loaded, the plugin shows "rack" instead (then Built-in) and says why.

## Testing without a skin

`plugin/skin/tests/skins/test/` is a tiny text-only skin (coloured boxes, no images) that uses every element and
the Lua API; `plugin/skin/tests/skin_test.cpp` drives it in CI. Never commit skins, images or fonts: CI rejects them.
