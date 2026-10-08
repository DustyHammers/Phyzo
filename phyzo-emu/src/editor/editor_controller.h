// The editor's screens (F-30; docs/editor/EDITOR_SPEC.md, docs/editor/EDITOR_ROUND2.md): areas, sections, the edited
// sound and oscillator, pages; reads the visible page with SysEx partial reads when it opens, after panel gestures and
// preset loads (debounced), and directly behind program changes, bank selects and dumps (the engine's standing read).
// Never polls. Never sends a panel message, never changes the panel's selection.
//
// Editing (round 2): only fields whose write path was verified live are editable, every other field is display only:
//  - oscillator single-byte fields (value types 0, 1, 3): NRPN; the wave: SysEx partial write of the 4-byte wave ID;
//  - sound settings and system: SysEx partial write of the byte (bit fields: read the byte, change the bits, write it);
//  - COMMON sound slots and knob values: NRPN MSB 1;
//  - ARP (docs/editor/ROUND2_REVIEW.md): exactly as hands on the panel do, with the panel's own buttons only (never a
//    knob, never a write into the arp state, never a whole-preset dump): the arp control's button (Tempo, Range, or Arp
//    Mode pressed until the OS's arp-control variable reads the control), a 1.4 s wait, then -/No or +/Yes once per
//    step (40 ms apart), reading the value back until it lands. Safety: -/No and +/Yes are pressed only right after the
//    arp-control variable was read as the wanted control. Arp on/off and keyboard: their own button, pressed once.
//  - PRESETS: a click on a name sends its program change; the current preset is marked (focused).
//  - LIBRARY (v0.8.3 step 2; docs/editor/LIBRARY.md): the preset library's sources and their presets, from
//    LibraryLink. A click on a preset sends its stored single-preset message, and nothing else, into the OS's MIDI
//    input: the OS puts it in Compare, as for a single-preset dump over MIDI. SAVE, IMPORT, EXPORT B1/B2 and FOLDER
//    are the library's (files only; SAVE refused while the Compare LED is lit). Messages replace row 4 for 3 seconds.
//  - INSERT and REVERB (docs/editor/EFFECTS_EDITOR_SPEC.md, round 3): effect parameter values by a raw-memory write of
//    the word into the edit buffer, then the OS's effect parameter message; the algorithm by NRPN MSB 1 LSB 30, 31,
//    then 32 = 0; variations by NRPN LSB 32 / 33; the insert modulation as one 10-byte partial write of the edit
//    buffer (offset 64) per gesture. After the modulation write or LSB 30-33 every effect send waits kFxHoldMs (the
//    OS rebuilds its effects). Under a variation the slot's values are those that play (the running performance); the
//    first edit of one of them in the editor copies the variation to PSEt (write path 4b, see VarCopy), then the edit
//    follows by its normal path.
// Every edit is clamped to the field's range, shown at once, sent (NRPN and partial writes paced like panel knob moves,
// the latest value winning; arp edits one at a time, the latest target winning) and read back after the last write.
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
    virtual void pressButton(int /*raw*/, bool /*down*/) {}   // a front-panel button, as the panel sends it (81 / 80 raw);
                                                              // counted in panelGestures like the panel's own presses
    virtual uint64_t externalMidi() const { return 0; }       // MIDI messages into the OS from the host / virtual port
    virtual bool panelButtonHeld() const = 0;        // a panel button is down now (latched by the skin, or not released)
};

// The preset library as the editor sees it (the plugin wraps library::Service and its file dialogs). Message thread.
struct LibraryLink {
    struct Source { std::string title; size_t count = 0, done = 0, total = 0; bool busy = false; };
    struct Entry { std::string name; bool ready = false, accepted = true; };   // ready: read by the OS
    virtual ~LibraryLink() = default;
    virtual void open() = 0;                         // the area was opened: rescan, make or read what is missing
    virtual uint64_t serial() const = 0;             // changes whenever the lists below change
    virtual std::vector<Source> sources() const = 0;
    virtual std::vector<Entry> entries(int source) const = 0;
    virtual bool message(int source, int entry, std::vector<uint8_t>& out) const = 0;   // the stored single-preset message
    virtual bool takeNotice(std::string& text) = 0;  // the library's messages, in order
    virtual void save() = 0;                         // SAVE (the Compare LED was checked here)
    virtual void exportBank(int bank) = 0;           // EXPORT B1 (0) / B2 (1): then the save dialog
    virtual void importFiles() = 0;                  // IMPORT: the open dialog
    virtual void reveal() = 0;                       // FOLDER: Library/ in the file browser
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
    // The preset library: adds the LIBRARY area (null: none). The link outlives the controller's use of it.
    void setLibrary(LibraryLink* library);
    void setOpen(bool open);
    bool isOpen() const { return open_; }
    // Legends: action "area" | "section" | "target" | "page" | "page_prev" | "page_next" | "toggle_follow" with the
    // legend's text (as in the layout table); "library" with SAVE, IMPORT, EXPORT B1, EXPORT B2 or FOLDER. The glass (text = a value's reference from View::screen.values):
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
    // LIBRARY: 8 entries per page; a message on row 4 for this long; the references of its picks on the screen.
    static constexpr int kLibraryPerPage = 8;
    static constexpr double kLibraryNoticeMs = 3000;
    static constexpr int kLibrarySourceRef = 1 << 20, kLibraryEntryRef = 1 << 21;
    static constexpr int kCompareLed = 0x16;         // lit: unsaved edits, or a received preset in Compare
    static constexpr int kDragDpPerStep = 4, kFineDragDpPerStep = 16;
    // The rebuild hold (docs/editor/ROUND3_ANSWERS.md 4): after the modulation block write or NRPN LSB 30-33 the OS
    // rebuilds the running effects and loses effect edits from the sound for about 280 ms; every effect send waits this
    // long after one of them (the latest queued value is sent then), and the read-back follows the hold.
    static constexpr double kFxHoldMs = 350;
    // Copying a variation to PSEt: the read-back must equal the copy within this long, else one rewrite, then give up.
    static constexpr double kCopyVerifyMs = 1000, kCopyRereadMs = 100;

    // Tests
    std::vector<std::vector<uint8_t>> pageReadsForTest() const { return pageReads(); }
    int refForTest(char kind, int field) const { return refOf(kind, field); }
    int focusForTest() const { return focus_; }
    bool arpBusyForTest() const { return jobActive_ || !pendingArp_.empty(); }
    // The OS's panel mode read before the last arp edit pressed any button (-1: none yet). After an arp edit the panel
    // stays in the arp menu; the editor does not try to return it (docs/editor/ROUND2_REVIEW.md, to be settled).
    int panelModeBeforeArpEdit() const { return modeBeforeArpEdit_; }
    // Messages for the user (message thread): the latest text and a serial that changes with each new one. The next
    // edit clears it (an empty text with a new serial).
    const std::string& notice() const { return notice_; }
    uint32_t noticeSerial() const { return noticeSerial_; }

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
    // 'n' preset name (field = preset number), 'm' COMMON / 'a' ARP preset field (field = index in its list), 'x' an
    // effect field (field = index in fx_).
    struct Ref { const edtables::Table* table; int field; char kind; };
    struct Rmw { ObjKey key; uint32_t offset; int bit, width, value; };
    // An effect field: the insert algorithm and variation, the reverb variation, the insert mix (word 136, or 292 with
    // a variation), routing-shell slot n, insert or reverb parameter slot n, insert modulation field n (record order).
    struct FxRef {
        enum class K { Algo, InsertVar, ReverbVar, InsertMix, Routing, Insert, Reverb, Mod } k;
        int n = 0;
    };

    void build();
    void render();
    // LIBRARY
    bool libraryArea() const;
    void renderLibrary(View& v);
    bool libraryCommand(const std::string& action, const std::string& text);
    void libraryNotice(const std::string& text);     // row 4 for kLibraryNoticeMs, from now
    int libraryPages(int section) const;
    int librarySource() const;                       // the open source's index in the current list (-1: none)
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
    void write(int ref, int64_t d, bool exact = false);   // clamp (not `exact`: revert), show, queue
    void apply(int ref, int64_t d);                  // into the bytes the editor holds (shown at once)
    void pump();                                     // paced sends
    void sendWrite(int ref, int64_t d);
    void arpPump();                                  // the arp edit in progress, through the panel's buttons
    // The arp edit ends: `done` if it landed; `interrupted` by panel input or other MIDI (or the editor closing):
    // nothing more is pressed. Otherwise, after the last queued edit of a run that began in normal mode, the panel is
    // returned from the arp menu with the bank button (once, only while the mode reads the arp menu).
    void arpFinish(bool done, bool interrupted = false);
    void arpEnd();
    void press(int button, bool down);
    void rawRead(uint32_t address, uint32_t length);
    void updateCurrentPreset();                      // the stored preset whose name the edit buffer holds
    void afterWrite();
    void presetChanged();                            // a new preset: no revert values, EDITED off
    void moveFocus(int dr, int dc);
    std::vector<edpages::Screen::Span> visibleSpans() const;

    // effects
    int fxRefOf(FxRef::K k, int n) const;
    int lockSlot(const FxRef& e) const;              // the slot a variation locks this field in: 0 insert, 1 reverb, -1 none
    bool variationPlays(int slot) const;             // a variation (or an unknown selection) plays this slot
    bool copyPossible(int slot) const;               // the variation can be copied to PSEt now
    bool startCopy(int slot);                        // write path 4b: the running values into the edit buffer
    void copyWrite(int slot);                        // the copy's raw writes, then its read-back
    void copyRead(int slot);
    void copyPump();                                 // PSEt once the read-back equals the copy; one rewrite; give up
    void copyAbort(int slot, bool failed);           // stop: nothing more is sent; the slot's queued edits dropped
    bool copying() const { return copy_[0].active || copy_[1].active; }
    std::vector<edpages::Page> insertAlgoPages();     // INSERT ALGO: page 1, then the running algorithm's parameters
    void checkFxAlgo();                              // the running insert algorithm changed: its parameter pages
    int runningAlgo() const;                         // the running insert algorithm's index (bytes 330/331), -1: unknown
    bool fxByte(uint32_t off, int& b) const;         // a preset byte (edit buffer)
    bool fxRaw(uint32_t off, int size, bool running, int64_t& v) const;
    bool insertVariation(int& v) const;              // -1: PSEt
    bool reverbVariation(int& v) const;
    bool fxLocked(const FxRef& e) const;             // a variation plays this field's value: display only
    uint32_t fxOffset(const FxRef& e) const;         // preset offset
    int fxSize(const FxRef& e) const;
    const edtables::FxParam* fxParam(const FxRef& e) const;   // the definition its value is shown and ranged by
    int fxWidth(const FxRef& e) const;
    bool lockedHint(int ref);                        // a locked effect field: tell the user, true
    void pumpMod();                                  // the insert modulation block write, when due

    // Copying a variation to PSEt (docs/editor/EFFECTS_EDITOR_SPEC.md write path 4b), per slot (0 insert, 1 reverb):
    // the running bytes written into the edit buffer with raw writes, read back until they equal the copy (one
    // rewrite after kCopyVerifyMs), then NRPN LSB 32 / 33 = -1 (PSEt). Meanwhile the slot behaves as under PSEt in the
    // editor and every effect send waits; after PSEt the rebuild hold, then the queued edits (latest value wins).
    struct VarCopy {
        bool active = false;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> ranges;   // preset offset, the bytes written there
        int attempt = 0;                             // 1 the first write, 2 the rewrite
        double writtenAt = 0, rereadAt = -1;
        int readsOut = 0;                            // read-back replies still to come
        bool mismatch = false, matched = false;
    };
    std::array<VarCopy, 2> copy_;
    std::array<bool, 2> copyFailed_{};               // a copy gave up: the slot stays locked until the variation changes
    double runningAt_ = -2e9;                        // when the running performance's effect block was last read

    // modulation block write (one per gesture)
    std::array<uint8_t, 10> modBlock_{};
    bool modPending_ = false, modReady_ = false;
    double fxHoldUntil_ = -1e9;                      // effect sends wait until then (the rebuild hold)

    Link& link_;
    LibraryLink* lib_ = nullptr;
    std::string libSource_;                          // the open source (by title: positions change on a rescan)
    int libSourceIndex_ = 0;                         // where it was
    int libPage_[2] = {0, 0};                        // SOURCES, PRESETS
    std::string libMarkedSource_;                    // the last preset sent from here (marked)
    int libMarked_ = -1;
    std::string libNoticeText_;                      // row 4 until libNoticeUntil_
    double libNoticeUntil_ = -1;
    std::vector<std::string> libNotices_;            // the library's messages waiting for row 4
    uint64_t libSerial_ = 0;
    edtables::OsTables tables_;
    std::vector<edfields::Field> common_, arp_;
    std::vector<FxRef> fx_;
    int fxAlgo_ = -1;                                // the running insert algorithm the INSERT ALGO pages show
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
    std::map<int, int64_t> pendingArp_;              // ARP edits waiting for their turn (by reference; latest wins)
    // The arp edit in progress (one at a time).
    struct ArpJob {
        enum class Stage { ReadMode, Select, SelectUp, ReadControl, Settle, Check, PreStep, Step, StepUp, StepGap, ToggleUp,
                           ToggleRead, Leave, LeaveUp, LeaveCheck } stage;
        int ref = -1;
        int control = -1;                            // the arp-control number; -1: a flag switched by its own button
        int64_t target = 0;
        double at = 0;                               // when the stage started
        bool asked = false;                          // the stage's reads are out
        int selects = 0, rounds = 0, presses = 0, dir = 0;
        uint64_t gestures = 0, midi = 0;             // the panel's and the MIDI input's counts when the edit began
        int ownPresses = 0;                          // button edges the editor itself sent since then
        int64_t last = 0;
    };
    bool jobActive_ = false;
    ArpJob job_;
    int heldButton_ = -1;                            // a button the editor is holding down (released on close)
    int arpControl_ = -1;                            // the OS's arp-control variable, as last read
    int panelMode_ = -1;                             // the OS's panel mode, as last read
    int modeBeforeArpEdit_ = -1;                     // the panel mode read before the last arp edit pressed anything
    int runMode_ = -1;                               // the panel mode when the current run of arp edits began
    std::string notice_;                             // a message for the user (the plugin shows it), with its serial
    uint32_t noticeSerial_ = 0;
    void setNotice(const std::string& text) { notice_ = text; ++noticeSerial_; }
    bool arpControlFresh_ = false, panelModeFresh_ = false, liveFresh_ = false, arpPresetFresh_ = false, flagsFresh_ = false;
    int64_t currentArpPreset_ = -1;                  // the OS's current arp preset (-1: the preset's own settings)
    bool arpPresetValid_ = false;
    std::array<uint8_t, 2> arpFlags_{};              // the OS's arp on / keyboard flags
    bool flagsValid_ = false;
    // PRESETS: the current preset (marked), the last one picked here (breaks ties between equal names)
    int currentPreset_ = -1, lastPicked_ = -1;
    std::array<bool, kPresets> presetNameRead_{};
    int presetNamesRequested_ = 0;
    std::map<int, int64_t> before_;                  // values before the edits since the preset loaded (revert)
    bool edited_ = false;
    double nextSendAt_ = 0, lastWriteAt_ = -1e9, readBackAt_ = -1;

    // triggers
    uint64_t lastLeds_ = 0, lastGestures_ = 0, lastTriggers_ = 0, lastMoves_ = 0;
    std::array<char, 4> lastDisplay_{};
    double now_ = 0, readDue_ = -1, displayChangedAt_ = -1, lastLedChangeAt_ = -1;
    int outstanding_ = 0;
    bool unanswered_ = false;                        // requests timed out with no reply since: no name reads
    double lastSendAt_ = 0;

    mutable std::mutex viewMtx_;
    View view_;
};

// The optional wave-name file (one line per wave: number, tab, name). Returns names by number (index n-1).
std::vector<std::string> parseWaveNames(const std::string& text);

}  // namespace editor
