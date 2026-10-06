-- Test skin script: records what the Lua API reports, for skin_test to read back.
events = {}
local function note(s) events[#events + 1] = s end

function onLoad()
	note("load")
	note("plugin " .. plugin.name .. " " .. plugin.vendor .. " " .. plugin.version)
	panel.onLed(0x21, function(state) note("led21 " .. state) end)
	panel.onBeat(0x23, function() note("beat23") end)
	panel.onKnob(11, function(raw) note("knob11 " .. raw) end)
	panel.onButton(0x13, function(down) note("button13 " .. tostring(down)) end)
	panel.onDisplay(function(a, b, c, d) note(string.format("display %02X %02X %02X %02X", a, b, c, d)) end)
	Log.Message(Log.logtype.info, "test skin loaded")
end

frames = 0
function onFrame() frames = frames + 1 end

function lastEvent() return events[#events] end
function hasEvent(s)
	for _, e in ipairs(events) do if e == s then return true end end
	return false
end

-- The editor unit (test-editor.rml)
function editorLoaded()
	note("editor load")
	editor.followLeds{sound = {0x0B, 0x13, 0x03, 0x07}, osc = {0x06, 0x15}, env = {0x11, 0x0C, 0x0E},
	                  noise = 0x02, lfo = 0x0F, filterKbd = 0x08}
	editor.knobLabels{"P", "H", "Y", "Z", "O"}
	editor.onChange(function()
		local s = editor.state()
		note("editor " .. s.area .. " " .. s.section .. " " .. s.page .. "/" .. s.pages)
	end)
end
