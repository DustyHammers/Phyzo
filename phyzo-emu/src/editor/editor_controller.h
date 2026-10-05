// The editor's screens, read-only (F-30 step 2; docs/editor/EDITOR_SPEC.md): areas, sections, the edited sound and
// oscillator, pages; reads the visible page with SysEx partial reads when it opens, after panel gestures and preset
// loads (debounced), and directly behind program changes, bank selects and dumps (the engine's standing read). Never
// polls. Never sends a panel message, never changes the panel's selection.
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
    virtual int ledMode(int code) const = 0;          // 0 off, 1 on, 2 flash, 3 beat flash
    virtual std::array<char, 4> display() const = 0;
    virtual bool running() const = 0;                // the OS is running (requests can be answered)
};

// What the skin shows (a copy; legends by their text as in docs/editor/EDITOR_PANEL_LAYOUT.json).
struct View {
    edpages::Screen screen;
    std::string area, section;                       // current area legend (e.g. "OSC") and section ("FILTER")
    int sound = 0, osc = 0;                          // the edited sound (0-3) and oscillator (0-1)
    bool soundTargets = false, oscTargets = false;   // S1-S4 / O1 / O2 apply to the current area
    int page = 0, pages = 0;                         // current page in the section (1-based), pages in the section
    bool prev = false, next = false;                 // a page to go to
    bool follow = true, data = false, edited = false;
    std::set<std::string> areas, sections;           // legends that lead somewhere (lit or ghosted otherwise)
    uint32_t serial = 0;                             // changes whenever anything above changes
};

class Controller {
public:
    explicit Controller(Link& link);

    // Message thread
    void setTables(const edtables::OsTables& tables); // from the user's OS image (empty tables: no pages)
    void setWaveNames(const std::vector<std::string>& names);   // index n-1 = wave n; from the skin folder
    // The LEDs Follow watches (supplied by the skin): Sound 1-4, OSC 1/2, envelope Pitch / Filter / Amp; -1: none.
    void setFollowLeds(const std::array<int, 4>& sound, const std::array<int, 2>& osc, const std::array<int, 3>& env);
    void setOpen(bool open);
    bool isOpen() const { return open_; }
    // A legend was clicked: action "area" | "section" | "target" | "page" | "page_prev" | "page_next" | "toggle_follow"
    // with the legend's text (as in the layout table). Unknown or unavailable legends do nothing.
    void command(const std::string& action, const std::string& text);
    void tick(double nowMs);                         // regularly (~every 10-50 ms): replies, triggers, debounce

    // Any thread
    View view() const;
    uint32_t serial() const { std::lock_guard<std::mutex> lk(viewMtx_); return view_.serial; }

    static constexpr double kDebounceMs = 50, kDisplayFallbackMs = 600, kReplyTimeoutMs = 3000;
    static constexpr int kPresets = 64, kPresetsPerPage = 8;

    // Tests
    std::vector<std::vector<uint8_t>> pageReadsForTest() const { return pageReads(); }

private:
    struct Section { std::string name; std::vector<edpages::Page> pages; };
    struct Area { std::string name; std::vector<Section> sections; };
    struct ObjKey {
        uint8_t type; uint16_t number; uint8_t subtype; uint16_t subnumber;
        bool operator<(const ObjKey& o) const {
            return std::tie(type, number, subtype, subnumber) < std::tie(o.type, o.number, o.subtype, o.subnumber);
        }
    };
    struct Obj { std::vector<uint8_t> bytes; std::vector<bool> valid; };
    // A page item's source: the table and field, and which object it lives in.
    struct Ref { const edtables::Table* table; int field; char kind; };   // kind: 's' system, 'p' sound settings, 'c' sound common, 'o' oscillator, 'n' preset name

    void build();
    void render();
    void readPage();
    std::vector<std::vector<uint8_t>> pageReads() const;
    ObjKey keyFor(char kind) const;
    void handleReply(const std::vector<uint8_t>& m);
    void followLeds();
    Area* currentArea();
    const Area* currentArea() const;
    void go(int area, int section, int page);
    void bump() { ++view_.serial; }

    Link& link_;
    edtables::OsTables tables_;
    edpages::Waves waves_;
    std::vector<std::string> waveNames_;
    std::array<int, 4> soundLeds_{-1, -1, -1, -1};
    std::array<int, 2> oscLeds_{-1, -1};
    std::array<int, 3> envLeds_{-1, -1, -1};
    bool open_ = false;

    std::vector<Area> areas_;
    std::vector<Ref> refs_;                          // page item reference -> source
    int area_ = 0, section_ = 0, page_ = 0;
    int sound_ = 0, osc_ = 0;
    bool follow_ = true;
    std::map<ObjKey, Obj> objs_;
    std::array<std::string, kPresets> presetNames_;
    bool wavesRequested_ = false;

    // triggers
    uint64_t lastLeds_ = 0, lastGestures_ = 0, lastTriggers_ = 0;
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
