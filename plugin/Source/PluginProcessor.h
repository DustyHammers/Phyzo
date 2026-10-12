// Phyzo audio processor: the emulated machine (Engine) as a VST3/AU instrument.
// Data folder: ~/Documents/Phyzo/ with roms/ (the two ROM files, found by checksum), skins/, Library/ (the preset
// library's .syx files) and cache/ (the factory user flash made from the user's own OS, by its MD5; the library's
// index).
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "editor/editor_controller.h"
#include "engine.h"
#include "library/service.h"
#include "midi_learn.h"
#include "rom_id.h"
#include "skin_port.h"
#include "VirtualMidiPort.h"

class PhyzoProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    PhyzoProcessor();
    ~PhyzoProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlockBypassed;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;   // logged, then JUCE's own

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;

    // ---- for the editor (message thread)
    Engine& engine() { return engine_; }
    const romid::ScanResult& romScan() const { return scan_; }
    juce::File romFolder() const { return dataFolder_.getChildFile("roms"); }
    juce::File skinsFolder() const { return dataFolder_.getChildFile("skins"); }
    // The synth as the skins see it (thread-safe: used by the skin render thread).
    skin::PanelPort& panelPort() { return port_; }
    double hostRate() const { return hostRate_.load(); }
    // MIDI port settings (global): applied to every open instance.
    static void applyMidiSettingsToAll();
    const VirtualMidiPort& virtualPort() const { return vport_; }
    bool showDebug = true;
    std::atomic<float> editorScale{0};               // window size of this project (0: not set); saved with it
    std::atomic<bool> editorScaleLegacy{false};      // editorScale is on the old scale (a version 3 project, not yet
                                                     // converted by an editor); kept so with the project until then

    // ---- the editor unit (F-30), message thread
    // Wanted: the project's choice (right-click > Editor), saved with it. Active: a window shows the unit; only then
    // does the editor read from the OS (with no window, or the unit closed, nothing is sent).
    bool editorWanted() const { return editorWanted_.load(); }
    void setEditorWanted(bool on) { editorWanted_ = on; }
    void setEditorActive(bool on);
    uint32_t editorSerial() const { return editor_.serial(); }
    // The editor unit's value lists and keys (message thread; the plugin window's right-click and keyboard).
    editor::Controller& editorController() { return editor_; }
    void loadWaveNames(const juce::File& skinFolder);    // the optional wave-name file of the skin
    static constexpr const char* kWaveNamesFile = "waves.txt";

    // ---- MIDI learn (F-55, docs/MIDI_LEARN.md), message thread. A controller is a second front panel: learned CCs
    // (or notes, for buttons) from the host's MIDI drive panel controls through the same calls as the mouse. The
    // bindings are plugin settings (saved with the project, defaults in Phyzo.settings), never synth or host parameters.
    midilearn::Learn& learn() { return learn_; }
    midilearn::PerfLabels perfLabels() const { std::lock_guard<std::mutex> lk(labelsMtx_); return perfLabels_; }
    void armLearn(int control);                      // MIDI learn: arms it, or cancels if it was armed
    bool cancelLearn();                              // Esc: true if a learn was armed (and is now cancelled)
    void setLearnCc(int control, int cc);            // Set CC (two clicks): on its channel, Any if it had none
    void setLearnChannel(int control, int channel);  // 0 = Any, 1-16
    void forgetLearn(int control);
    void forgetAllLearn();
    void saveLearnDefaults();                        // for new instances and older projects
    void clearLearnDefaults();
    static std::vector<midilearn::Binding> learnDefaults();
    struct Meter {                                   // CPU per block, written by the audio thread
        std::atomic<float> avg{0}, peak{0}, last{0}, lastMs{0}, blockMs{0};
        std::atomic<int> blockSize{0};
        std::atomic<uint64_t> overruns{0};
        std::atomic<float> worstMs{0};               // longest block since the last prepare (or reset)
        std::atomic<uint64_t> stateRequests{0};      // host requests for the plugin state
        // Per part, share of the block's real-time budget over the last second: average and the worst block.
        // 0 68k + devices, 1 voice chip, 2 ESP2, 3 rate conversion, 4 UI-to-audio queue.
        static constexpr int kParts = 5;
        std::array<std::atomic<float>, kParts> partAvg{}, partPeak{};
        std::array<std::atomic<float>, 8> history{}; // one value per second, oldest first after `head`
        std::atomic<uint32_t> head{0};
    } meter;

private:
    // Skin access to the engine, and the positions of the skin's knob elements (per element id; stacked knobs
    // share a cc). The positions are the panel's physical state, saved with the project, never host parameters.
    class Port : public skin::PanelPort {
    public:
        explicit Port(Engine& e) : engine(e) {}
        void sendControl(int cc, int raw) override {
            int dropped;
            if (learn) learn->takeKnobMove(cc, dropped);   // the mouse (or Lua) moved it after the controller
            engine.setControl(cc, raw);
        }
        bool takeLearnedKnob(int cc, int& raw) override { return learn && learn->takeKnobMove(cc, raw); }
        int controlPosition(int cc) override { return engine.controlPosition(cc); }
        void sendButton(int raw, bool down) override { engine.pressRaw(raw, down); }
        skin::LedReport led(int code) override {
            const Engine::LedInfo i = engine.led(code);
            return {skin::LedMode(i.mode), i.beats, i.beatIntervalMs};
        }
        std::array<uint8_t, 4> segments() override { return engine.segments(); }
        bool audioClip() override { return engine.inputClip(); }
        skin::EditorPort* editor() override { return ed; }
        skin::EditorPort* ed = nullptr;
        bool knobPosition(const std::string& id, int& raw) override {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = knobs.find(id);
            if (it == knobs.end()) return false;
            raw = it->second;
            return true;
        }
        void setKnobPosition(const std::string& id, int raw) override { std::lock_guard<std::mutex> lk(mtx); knobs[id] = raw; }
        Engine& engine;
        midilearn::Learn* learn = nullptr;
        std::mutex mtx;
        std::map<std::string, int> knobs;
    };

    // The editor's view of the engine (editor::Link) and the skin's view of the editor (skin::EditorPort).
    class EditorLink : public editor::Link {
    public:
        explicit EditorLink(Engine& e) : engine(e) {}
        void setOpen(bool on) override { engine.setEditorOpen(on); }
        bool send(const std::vector<uint8_t>& m) override { return engine.editorSend(m.data(), m.size()); }
        void setStandingRead(const std::vector<std::vector<uint8_t>>& msgs) override { engine.setEditorStandingRead(msgs); }
        bool popReply(std::vector<uint8_t>& m) override {
            if (!engine.editorReplies().peekSize()) return false;
            m.clear();
            engine.editorReplies().pop([&](uint8_t b) { m.push_back(b); });
            return true;
        }
        uint64_t ledStateMessages() const override { return engine.ledStateMessages(); }
        uint64_t presetTriggers() const override { return engine.presetTriggers(); }
        uint64_t panelGestures() const override { return engine.panelGestures(); }
        uint64_t panelControlMoves() const override { return engine.panelControlMoves(); }
        int lastPanelControl() const override { return engine.lastPanelControl(); }
        int ledMode(int code) const override { return int(engine.led(code).mode); }
        std::array<char, 4> display() const override { return engine.display(); }
        bool running() const override { return engine.state() == Engine::State::Running; }
        // The editor's arp edits: the panel's own buttons, as the skin presses them (81 / 80 raw).
        void pressButton(int raw, bool down) override { engine.pressRaw(raw, down); }
        uint64_t externalMidi() const override { return engine.externalMidi(); }
        bool panelButtonHeld() const override { return engine.anyButtonHeld(); }
        Engine& engine;
    };
    class EditorPort : public skin::EditorPort {
    public:
        explicit EditorPort(PhyzoProcessor& p) : proc(p) {}
        bool editorOpen() override { return proc.editorActive_.load(); }
        uint32_t editorSerial() override { return proc.editor_.serial(); }
        void editorState(skin::EditorState& out) override;
        void editorCommand(const std::string& action, const std::string& text) override {
            if (action == "library" && text == "LIBRARY") {   // from any page, and from the main document: the editor
                PhyzoProcessor* p = &proc;                   // unit opens on the library list if it was closed
                proc.postToEditor([p](editor::Controller& c) { p->openEditorUnit(); c.command("library", "LIBRARY"); });
                return;
            }
            proc.postToEditor([action, text](editor::Controller& c) { c.command(action, text); });
        }
        void editorFollowExtraLeds(int noise, int lfo, int filterKbd) override {
            proc.postToEditor([noise, lfo, filterKbd](editor::Controller& c) { c.setFollowExtraLeds(noise, lfo, filterKbd); });
        }
        void editorKnobLabels(const std::array<std::string, 5>& labels) override {
            { std::lock_guard<std::mutex> lk(proc.labelsMtx_); proc.perfLabels_ = labels; }   // MIDI learn's names
            proc.postToEditor([labels](editor::Controller& c) { c.setKnobLabels(labels); });
        }
        void editorFollowLeds(const std::array<int, 4>& s, const std::array<int, 2>& o, const std::array<int, 3>& e) override {
            proc.postToEditor([s, o, e](editor::Controller& c) { c.setFollowLeds(s, o, e); });
        }
        PhyzoProcessor& proc;
    };
    struct EditorTicker : juce::Timer {
        explicit EditorTicker(PhyzoProcessor& p) : proc(p) {}
        void timerCallback() override { proc.editorTick(); }
        PhyzoProcessor& proc;
    };
    // The preset library (v0.8.3 step 2) for the editor's LIBRARY area: library::Service (made when the area first
    // opens, with the ROMs known) and the plugin's file dialogs. Message thread.
    class LibraryBridge : public editor::LibraryLink {
    public:
        explicit LibraryBridge(PhyzoProcessor& p) : proc(p) {}
        void open() override { if (library::Service* s = proc.libraryService()) s->open(); }
        uint64_t serial() const override { return proc.library_ ? proc.library_->serial() : 0; }
        std::vector<Source> sources() const override;
        std::vector<Entry> entries(int source) const override;
        bool message(int source, int entry, std::vector<uint8_t>& out) const override {
            return proc.library_ && proc.library_->message(source, entry, out);
        }
        bool takeNotice(std::string& text) override;
        void save() override { if (library::Service* s = proc.libraryService()) s->save(); }
        void exportBank(int bank) override { if (library::Service* s = proc.libraryService()) s->exportBank(bank); }
        void exportMenu() override { proc.showExportMenu(); }
        void importFiles() override { proc.chooseImport(); }
        void reveal() override { proc.revealLibrary(); }
        PhyzoProcessor& proc;
    };
    library::Service* libraryService();
    void chooseImport();
    void revealLibrary();
    void showExportMenu();                           // the window's menu of the two banks
    void openEditorUnit();                           // LIBRARY with the editor unit closed
    void runEditorQueue();                           // the skin's editor calls (message thread)
    void serviceLibrary();                           // timer: an exported bank waiting for its save dialog
    void libraryNotice(const std::string& text) { libraryNotices_.push_back(text); }
    // MIDI learn: what the audio thread heard (timer); a message on the editor's row 4 (unit open) or the window's
    // banner (unit closed), 3 s.
    void serviceLearn();
    void learnMessage(const std::string& text);
    // The engine as the learned messages drive it: the mouse's calls (lock-free, the audio thread included).
    struct LearnPanel : midilearn::Panel {
        explicit LearnPanel(Engine& e) : engine(e) {}
        void control(int cc, int raw) override { engine.setControl(cc, raw); }
        void button(int raw, bool down) override { engine.pressRaw(raw, down); }
        Engine& engine;
    };

    // Skin threads hand their editor calls to the message thread through this queue.
    void postToEditor(std::function<void(editor::Controller&)> f);
    void editorTick();
    void loadEditorTables();

    void timerCallback() override;
    void scanRoms();
    void applyMidiSettings();
    static void virtualBytes(void* self, const uint8_t* bytes, size_t n) { static_cast<PhyzoProcessor*>(self)->engine_.virtualMidiIn(bytes, n); }

    Engine engine_;
    LearnPanel learnPanel_{engine_};
    midilearn::Learn learn_{learnPanel_};
    double learnClockMs_ = 0;                        // audio thread: the one-edge warning's clock
    mutable std::mutex labelsMtx_;
    midilearn::PerfLabels perfLabels_;               // the skin's perf knob letters (editor.knobLabels)
    Port port_{engine_};
    VirtualMidiPort vport_{engine_.virtualMidiOut(), &PhyzoProcessor::virtualBytes, this};   // after engine_: closed first
    EditorLink editorLink_{engine_};
    editor::Controller editor_{editorLink_};
    LibraryBridge libraryBridge_{*this};
    std::unique_ptr<library::Service> library_;       // after engine_: stopped before it (its clones ask the engine)
    std::unique_ptr<juce::FileChooser> chooser_;      // the import / export dialog (released by the timer once done)
    bool chooserBusy_ = false;                        // a dialog is open
    std::deque<std::string> libraryNotices_;          // the plugin's own library messages (exports)
    EditorPort editorPort_{*this};
    EditorTicker editorTicker_{*this};
    std::atomic<bool> editorWanted_{false}, editorActive_{false};
    std::mutex editorQueueMtx_;
    std::vector<std::function<void(editor::Controller&)>> editorQueue_;
    std::string editorTablesFor_;                    // the OS file the editor's tables were read from
    juce::File dataFolder_;
    romid::ScanResult scan_;
    int timerTicks_ = 0;
    std::atomic<double> hostRate_{0};
    std::vector<Engine::MidiEvent> events_;
    bool inputActive_ = false;                       // Audio In bus enabled by the host (set in prepareToPlay)
    std::vector<float> inL_, inR_;                   // Audio In copied out of the in-place buffer
    // startup log (Diag.h): the audio thread only sets these flags; the timer writes the lines
    std::atomic<bool> processed_{false}, bypassed_{false};
    bool processedLogged_ = false, bypassedLogged_ = false;
    std::atomic<bool> prepared_{false};              // prepareToPlay has run (the idle runner needs the host rate)
    bool idleLogged_ = false;                        // the idle runner's state last logged
    int loggedState_ = -1;                           // the engine state last logged
    juce::String loggedRoms_;                        // the ROM scan result last logged
    void logRoms();
    void logEngineState();
    // meter accumulation (audio thread only)
    double winTime_ = 0, winBudget_ = 0, winPeak_ = 0;
    std::array<double, Meter::kParts> winPart_{}, winPartPeak_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhyzoProcessor)
};
