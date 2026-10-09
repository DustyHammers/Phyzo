// Global skin settings (all instances and projects; not saved in projects and not synth parameters):
// the chosen skin and the zoom. Stored in ~/Library/Application Support/Phyzo/Phyzo.settings.
#pragma once
#include <juce_data_structures/juce_data_structures.h>
#include <atomic>
#include "WindowScale.h"

namespace SkinSettings {

inline const juce::String kBuiltIn = "Built-in";
inline const juce::String kDefaultSkin = "rack";

// Opened for each read or write (rare), so no file object outlives the plugin.
inline juce::PropertiesFile::Options options() {
    juce::PropertiesFile::Options o;
    o.applicationName = "Phyzo";
    o.filenameSuffix = "settings";
    o.folderName = "Phyzo";
    o.osxLibrarySubFolder = "Application Support";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    return o;
}
inline juce::String get(const char* key) { return juce::PropertiesFile(options()).getValue(key); }
inline void set(const char* key, const juce::var& v) {
    juce::PropertiesFile f(options());
    f.setValue(key, v);
    f.saveIfNeeded();
}

// Empty when never chosen (then: rack if present, else Built-in).
inline juce::String skin() { return get("skin"); }
inline void setSkin(const juce::String& name) { set("skin", name); }

// The last window size (winscale: 0.5-2.0 of the skin's 100 % size), for instances without a size of their own.
// Stored under a new key: sizes from the old scale ("scale", "zoom") are not carried over, so it starts at 100 %.
inline float scale() {
    const juce::String s = get("windowScale");
    const float v = s.isNotEmpty() ? s.getFloatValue() : 1.0f;
    return v >= winscale::kMin && v <= winscale::kMax ? v : 1.0f;
}
inline void setScale(float v) { set("windowScale", juce::String(v, 4)); }

// MIDI ports (plugin settings, not synth parameters): the virtual port pair (default on) and the OS's MIDI output
// to the host (default off).
inline bool virtualMidiPort() { const juce::String s = get("midiVirtualPort"); return s.isEmpty() || s.getIntValue() != 0; }
inline void setVirtualMidiPort(bool on) { set("midiVirtualPort", on ? 1 : 0); }
inline bool midiToHost() { return get("midiToHost").getIntValue() != 0; }
inline void setMidiToHost(bool on) { set("midiToHost", on ? 1 : 0); }

// MIDI learn's default bindings (docs/MIDI_LEARN.md): what a new instance, or a project from before v0.8.4, starts with.
// Compact text (midilearn::Learn::toText); empty unless saved with MIDI > MIDI learn > Save as default.
inline juce::String midiLearnDefaults() { return get("midiLearnDefaults"); }
inline void setMidiLearnDefaults(const juce::String& text) { set("midiLearnDefaults", text); }
inline void clearMidiLearnDefaults() {
    juce::PropertiesFile f(options());
    f.removeValue("midiLearnDefaults");
    f.saveIfNeeded();
}

// Developer > Write skin log (default off): ~/Documents/Phyzo/skin-log.txt is written only while this is on. Kept in
// memory after the first read (the log is written from the GL thread), shared by all instances in the process.
inline std::atomic<int> skinLogCache{-1};
inline bool skinLog() {
    int v = skinLogCache.load();
    if (v < 0) { v = get("skinLog").getIntValue() != 0 ? 1 : 0; skinLogCache = v; }
    return v != 0;
}
inline void setSkinLog(bool on) { skinLogCache = on ? 1 : 0; set("skinLog", on ? 1 : 0); }

}  // namespace SkinSettings
