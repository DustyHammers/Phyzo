// The editor's screens (F-30; docs/editor/EDITOR_SPEC.md, docs/editor/EDITOR_ROUND2.md): areas, sections, the edited
// sound and oscillator, pages; reads the visible page with SysEx partial reads when it opens, after panel gestures and
// preset loads (debounced), and directly behind program changes, bank selects and dumps (the engine's standing read).
// Never polls. Never sends a panel message, never changes the panel's selection.
//
// Editing (round 2): only fields whose write path was verified live are editable, every other field is display only:
//  - oscillator single-byte fields (value types 0, 1, 3): NRPN; the wave: SysEx partial write of the 4-byte wave ID;
//  - sound settings and system: SysEx partial write of the byte (bit fields: read the byte, change the bits, write it);
//  - COMMON sound slots and knob values: NRPN MSB 1;
//  - ARP: a whole-preset dump into the edit buffer, built from a fresh whole read of the edit buffer with the live arp
//    state folded into its arp block, then the edited fields; loading an arp preset replaces the block's body with the
//    arp preset's record body, keeping held, keyboard and both key-range pairs. At most one dump in flight.
//  - PRESETS: a click on a name sends its program change.
// Every edit is clamped to the field's range, shown at once, sent (NRPN and partial writes paced like panel knob moves,
// the latest value winning; dumps coalesced) and read back after the last write.
//
// No plugin framework here. Threads: everything runs on one thread (the plugin's message thread); view() may be
// called from any thread (the skin's render thread).
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>
#include "editor_fields.h"
#include "editor_pages.h"
#include "editor_proto.h"
#include "editor_tables.h"

namespace editor {

// What the controller needs from the engine.
struct Link {
    virtual ~Link() = default;
    virtual void setOpen(bool on) = 0;
    virtual bool send(const std::vector<uint8_t>& m) = 0;
    virtual void setStandingRead(const std::vector<std::vector<uint8_t>>& msgs) = 0;
    virtual bool popReply(std::vector<uint8_t>& m) = 0;
    virtual uint64_t ledStateMessages() const = 0;
    virtual uint64_t presetTriggers() const = 0;
    virtual uint64_t panelGestures() const = 0;
    virtual uint64_t panelControlMoves() const { return 0; }   // panel knob moves only
    virtual int lastPanelControl() const { return -1; }        // the last panel control moved (0-25)
    virtual int ledMode(int code) const = 0;          // 0 off, 1 on, 2 flash, 3 beat flash
    virtual std::array<char, 4> display() const = 0;
    virtual bool running() const = 0;                // the OS is running (requests can be answered)
};

// What the skin shows (a copy; legends by their text as in docs/editor/EDITOR_PANEL_LAYOUT.json).
struct View {
    edpages::Screen screen;                          // screen.values: every value's cells and reference (clicks)
    std::string area, section;                       // current area legend (e.g. "OSC") and section ("FILTER")
    int sound = 0, osc = 0;                          // the edited sound (0-3) and oscillator (0-1)
    bool oscBoth = false;                            // both oscillators are edited (the panel's OSC LEDs both solid)
    bool soundTargets = false, oscTargets = false;   // S1-S4 / O1 / O2 apply to the current area
    int page = 0, pages = 0;                         // current page in the section (1-based), pages in the section
    bool prev = false, next = false;                 // a page to go to
    bool follow = true, data = false, edited = false;
    int focusRow = -1, focusCol = 0, focusLen = 0;   // the focused value's cells (blink); row -1: none
    bool typing = false;                             // typed entry in the focused value
    std::set<std::string> areas, sections;           // legends that lead somewhere (lit or ghosted otherwise)
    uint32_t serial = 0;                             // changes whenever anything above changes
};

// The right-click list of a value: the OS's value or name list (empty for plain numbers), grouped when long, and
// whether revert applies.
struct Choice {
    std::string label;
    int64_t value = 0;
    bool current = false;
};
struct Menu {
    int ref = -1;
    std::vector<Choice> items;                                   // short lists
    std::vector<std::pair<std::string, std::vector<Choice>>> groups;   // long lists, in groups
    bool revert = false;                                         // the value differs from the one before the edits
    std::string revertLabel;                                     // the value revert goes back to
};

class Controller {
public:
    explicit Controller(Link& link);

    // Message thread
    void setTables(const edtables::OsTables& tables); // from the user's OS image (empty tables: no pages)
    void setWaveNames(const std::vector<std::string>& names);   // index n-1 = wave n; from the skin folder
    // The LEDs Follow watches (supplied by the skin): Sound 1-4, OSC 1/2, envelope Pitch / Filter / Amp; -1: none.
    void setFollowLeds(const std::array<int, 4>& sound, const std::array<int, 2>& osc, const std::array<int, 3>& env);
    // More LEDs for Follow on panel knobs: Noise, LFO, Filter Keyboard (-1: none).
    void setFollowExtraLeds(int noise, int lfo, int filterKbd);
    // The performance knobs' labels (perf1-perf5, at most 4 characters; empty: PERF1-PERF5).
    void setKnobLabels(const std::array<std::string, 5>& labels);
    void setBaseChannel(int channel) { channel_ = channel & 0x0F; }   // NRPN and program changes (0-15)
    void setOpen(bool open);
    bool isOpen() const { return open_; }
    // Legends: action "area" | "section" | "target" | "page" | "page_prev" | "page_next" | "toggle_follow" with the
    // legend's text (as in the layout table). The glass (text = a value's reference from View::screen.values):
    // "focus", "pick" (a name to pick: its program change), "drag_begin", "drag" (text = steps from the start),
    // "drag_end", "type" (typed entry). Unknown or unavailable targets do nothing.
    void command(const std::string& action, const std::string& text);
    // Keys while the editor has the focus: "left", "right", "up", "down", "plus", "minus", "enter", "escape",
    // "backspace" or one printable character. True if the editor used it.
    bool key(const std::string& k);
    Menu menu(int ref) const;                        // the right-click list of a value
    void choose(int ref, int64_t value);             // a choice from the list (the arp preset field: load it)
    void revert(int ref);
    void tick(double nowMs);                         // regularly (~every 10-50 ms): replies, triggers, sends, debounce

    // Any thread
    View view() const;
    uint32_t serial() const { std::lock_guard<std::mutex> lk(viewMtx_); return view_.serial; }

    static constexpr double kDebounceMs = 50, kDisplayFallbackMs = 600, kReplyTimeoutMs = 3000;
    static constexpr double kPaceMs = 10;            // at most one write per 10 ms (like panel knob moves)
    static constexpr double kCableMsPerByte = 0.32;  // 31250 baud, 10 bits per byte
    static constexpr double kReadBackMs = 60;        // read the page back this long after the last write
    static constexpr double kOwnLedsMs = 1000;       // LED messages this soon after a write are the edit's own
    static constexpr int kPresets = 64, kPresetsPerPage = 8;
    static constexpr int kDragDpPerStep = 4, kFineDragDpPerStep = 16;

    // Tests
    std::vector<std::vector<uint8_t>> pageReadsForTest() const { return pageReads(); }
    int refForTest(char kind, int field) const { return refOf(kind, field); }
    int focusForTest() const { return focus_; }

private:
    struct Section { std::string name; std::vector<edpages::Page> pages; };
    struct Area { std::string name; std::vector<Section> sections; };
    struct ObjKey {
        uint8_t type; uint16_t number; uint8_t subtype; uint16_t subnumber;
        bool operator<(const ObjKey& o) const {
            return std::tie(type, number, subtype, subnumber) < std::tie(o.type, o.number, o.subtype, o.subnumber);
        }
        bool operator==(const ObjKey& o) const { return !(*this < o) && !(o < *this); }
    };
    struct Obj { std::vector<uint8_t> bytes; std::vector<bool> valid; };
    // A page item's source. kind: 's' system, 'p' sound settings, 'c' sound common, 'o' oscillator (table fields),
    // 'n' preset name (field = preset number), 'm' COMMON / 'a' ARP preset field (field = index in its list).
    struct Ref { const edtables::Table* table; int field; char kind; };
    struct Rmw { ObjKey key; uint32_t offset; int bit, width, value; };

    void build();
    void render();
    void readPage();
    std::vector<std::vector<uint8_t>> pageReads() const;
    ObjKey keyFor(char kind) const;
    void handleReply(const std::vector<uint8_t>& m);
    void followLeds();
    void followKnob(int cc);
    Area* currentArea();
    const Area* currentArea() const;
    void go(int area, int section, int page);
    bool goToRef(int ref);                           // the page holding the reference, focused
    // The reference of a table field (kind 's', 'p', 'c', 'o') or a preset field (kind 'm', 'a': index in its field
    // list); -1: none.
    int refOf(char kind, int field) const;

    // values (by reference)
    const edfields::Field* presetField(const Ref& r) const;
    bool bytesOf(char kind, const Obj*& o) const;
    bool valueOf(int ref, int64_t& d) const;         // the edit domain value as the editor holds it
    bool rangeOf(int ref, int64_t& lo, int64_t& hi) const;
    bool editable(int ref) const;
    std::string textOf(int ref, int64_t d) const;
    bool parseTyped(int ref, const std::string& s, int64_t& d) const;
    std::vector<Choice> listOf(int ref) const;       // the OS's values for a field with a list (empty: none)

    // writing
    void write(int ref, int64_t d);                  // clamp, show, queue
    void apply(int ref, int64_t d);                  // into the bytes the editor holds (shown at once)
    void pump();                                     // paced sends
    void sendWrite(int ref, int64_t d);
    void startDump();
    void finishDump();
    void afterWrite();
    void presetChanged();                            // a new preset: no revert values, EDITED off
    void moveFocus(int dr, int dc);
    std::vector<edpages::Screen::Span> visibleSpans() const;

    Link& link_;
    edtables::OsTables tables_;
    std::vector<edfields::Field> common_, arp_;
    std::array<std::string, 5> knobLabels_;
    edpages::Waves waves_;
    std::vector<std::string> waveNames_;
    std::array<int, 4> soundLeds_{-1, -1, -1, -1};
    std::array<int, 2> oscLeds_{-1, -1};
    std::array<int, 3> envLeds_{-1, -1, -1};
    int noiseLed_ = -1, lfoLed_ = -1, filterKbdLed_ = -1;
    int channel_ = 0;
    bool open_ = false;

    std::vector<Area> areas_;
    std::vector<Ref> refs_;                          // page item reference -> source
    int area_ = 0, section_ = 0, page_ = 0;
    int sound_ = 0, osc_ = 0;
    bool oscBoth_ = false;
    bool follow_ = true;
    std::map<ObjKey, Obj> objs_;
    std::array<std::string, kPresets> presetNames_;
    std::array<uint8_t, edfields::kArpBlockSize> live_{};   // the live arp state
    bool liveValid_ = false;
    std::map<int, std::string> patternNames_;        // arp pattern number -> name
    std::set<int> patternRequested_;
    std::array<std::string, edfields::kArpPresets> arpPresetNames_;
    int arpNamesRequested_ = 0;
    bool wavesRequested_ = false;

    // editing
    int focus_ = -1;                                 // the focused value's reference
    bool typing_ = false;
    std::string typed_;
    int dragRef_ = -1;
    int64_t dragStart_ = 0, dragValue_ = 0;
    std::map<int, int64_t> pending_;                 // NRPN / partial writes waiting for their turn (latest wins)
    std::vector<Rmw> rmw_;                           // bit fields waiting for a fresh read of their byte
    std::map<int, int64_t> pendingArp_;              // ARP edits for the next dump (by reference)
    int pendingArpPreset_ = -1;                      // an arp preset to load with the next dump
    enum class Dump { Idle, Reading } dump_ = Dump::Idle;
    double dumpAt_ = 0;
    bool dumpBuffer_ = false, dumpLive_ = false, dumpRecord_ = false;
    std::vector<uint8_t> dumpObject_, dumpBody_;
    std::map<int, int64_t> before_;                  // values before the edits since the preset loaded (revert)
    bool edited_ = false;
    double nextSendAt_ = 0, lastWriteAt_ = -1e9, readBackAt_ = -1;

    // triggers
    uint64_t lastLeds_ = 0, lastGestures_ = 0, lastTriggers_ = 0, lastMoves_ = 0;
    std::array<char, 4> lastDisplay_{};
    double now_ = 0, readDue_ = -1, displayChangedAt_ = -1, lastLedChangeAt_ = -1;
    int outstanding_ = 0;
    double lastSendAt_ = 0;

    mutable std::mutex viewMtx_;
    View view_;
};

// The optional wave-name file (one line per wave: number, tab, name). Returns names by number (index n-1).
std::vector<std::string> parseWaveNames(const std::string& text);

}  // namespace editor
