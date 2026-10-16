// The emulated machine as a real-time audio engine (no plugin framework here; plugin/ wraps it).
//
// Threads: the host's audio thread calls prepare()/process(); the message thread calls setRoms(), getState(),
// setState(), pressButton() and reads the status. Booting and restoring a saved state run on a worker thread on a
// machine of their own, which then replaces the running one.
//
// Timing: host sample H of the current run corresponds to machine sample anchor + H * 44100 / hostRate. Machine
// output is converted to the host rate without delay (Resampler); MIDI events are delivered to the emulated MIDI
// port at their exact host-sample time plus a constant latency(), which the plugin reports to the host.
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "editor/editor_proto.h"
#include "midi_stream.h"
#include "resampler.h"

class Machine;
struct OsImage;

class Engine {
public:
    enum class State { NoRoms, Booting, Running, Stopped };
    struct MidiEvent { int offset; const uint8_t* data; int size; };   // offset in host samples within the block

    Engine();
    ~Engine();

    // ---- message thread
    void setRoms(const std::string& osPath, const std::string& wavePath, const std::string& osMd5, const std::string& waveMd5);
    // Engine state blob (empty if there is nothing to save). While audio runs, the audio thread takes the snapshot
    // at a block boundary (about 0.05 ms, no allocation), so the host asking for the state never blocks the audio.
    std::vector<uint8_t> getState();
    void setState(const std::vector<uint8_t>& blob); // restores now, or as soon as the ROMs are known
    // Factory user flash (v0.8.3): what the OS's own factory restore writes, made once per OS image in a hidden
    // machine (userflash::factoryRestore) and cached in `cacheDir` by the OS image's MD5 (empty: this process only).
    // A new machine gets it before it boots (a new unit with factory presets); a restored state keeps its own (one
    // saved before v0.8.3 has none: it loads exactly as it did then, erased). Off by default: machines then start with
    // erased user flash, as before v0.8.3. With it the OS indexes its user memory at boot (about 0.2 s longer), so the
    // audio is not byte-identical to an erased machine's (LFO, arp and random timing); presets and sounds are the same.
    // Message thread, before setRoms.
    void setFactoryUserFlash(bool on, const std::string& cacheDir);
    using UserFlash = std::shared_ptr<const std::vector<uint8_t>>;
    // The factory user flash this process made or read for an OS image (null: not yet, or it could not be made).
    static UserFlash factoryUserFlashFor(const std::string& osMd5);
    // The running machine's state alone (getState without the engine's wrapping): what a library clone restores.
    // Taken the same way (a snapshot at a block boundary); the machine is not paused, pressed or read otherwise.
    std::vector<uint8_t> machineState();
    // Front-panel button press/release by OS button id (0 = -/No, 1 = +/Yes). Lock-free and allocation-free, any
    // thread (the button queue takes any number of producers).
    void pressButton(int osButton, bool down);
    // The panel id (raw) the running OS gives an OS button (0 = -/No, 1 = +/Yes), from the image's own button table;
    // -1 until a machine runs. pressButton(b) and pressRaw(osButtonRaw(b)) send the OS the same message.
    int osButtonRaw(int osButton) const { return osButton == 1 ? rawPlusPub_.load() : osButton == 0 ? rawMinusPub_.load() : -1; }
    // An analog panel control moved (docs/PANEL_CONTROLS.md): cc 0-25, raw 0-1023. Lock-free and allocation-free,
    // any thread, the audio thread included (MIDI learn, docs/MIDI_LEARN.md).
    // Like the real panel's serial link, each control sends at most one Bx cc vv per kControlIntervalMs (the latest
    // value wins) and only while the link is not backed up; buttons are never dropped or merged. The position is kept
    // (saved with the machine state) and is what the panel answers to the OS's F4 request when a machine boots.
    void setControl(int cc, int raw);
    static constexpr double kControlIntervalMs = 10;
    static constexpr size_t kPanelBacklogBytes = 9;  // knob messages wait while more bytes than this are unread
    int controlPosition(int cc) const { return cc >= 0 && cc < 26 ? positions_[size_t(cc)].load() : 0; }
    // A front-panel button by its panel id (raw): sends 81 raw (down) or 80 raw (up). Lock-free and allocation-free,
    // any thread, the audio thread included (learned buttons go through here exactly as the skin's).
    void pressRaw(int raw, bool down);
    // A panel button is down now (pressed and not yet released, or latched by the skin), by either call above.
    bool anyButtonHeld() const;

    // LEDs as the OS last set them (panel messages 90 off, 91 on, 92 flash, 9D beat flash).
    enum LedMode : uint8_t { LedOff = 0, LedOn = 1, LedFlash = 2, LedBeat = 3 };
    struct LedInfo { LedMode mode; uint32_t beats; float beatIntervalMs; };   // beats: count of 9D messages so far
    LedInfo led(int code) const;
    // The display's segment bytes, leftmost digit (panel message 96) first.
    std::array<uint8_t, 4> segments() const;
    // UI change counter: bumped only when an LED's state, a beat flash, the display or the Input Clip LED actually
    // changed (any number of panel messages between two reads count as one change). A window redraws only when it
    // moves. panelMessages() counts the OS's LED and display messages as they come (for the UI rate log).
    uint32_t uiSerial() const { return uiSerial_.load(std::memory_order_relaxed); }
    uint64_t panelMessages() const { return panelMessages_.load(std::memory_order_relaxed); }

    // Call regularly on the message thread: restarts the machine after the OS asked for a reboot.
    void service();

    State state() const { return state_.load(); }
    std::string message() const;                     // plain-language status (stop reason, restore notes)
    std::array<char, 4> display() const;             // the 4-character display (best-guess characters)
    uint8_t displayDots() const { return uint8_t(dots_.load()); }

    // ---- audio thread
    // audioIn: the plugin's Audio In bus is active. Its signal is converted to 44.1 kHz (same converter quality as
    // the output) and fed to the ESP2's serial input (SER0 L/R, 24-bit, 0 dBFS = full scale), aligned like MIDI:
    // host sample H reaches the machine at anchor + H * 44100/hostRate + the same constant latency. The converter's
    // look-ahead can make that latency a little longer than without the input (reported by latencySamples()).
    void prepare(double hostRate, int maxBlock, bool audioIn = false);
    void process(float* left, float* right, int n, const MidiEvent* events, int numEvents,
                 const float* inLeft = nullptr, const float* inRight = nullptr);
    // Input Clip LED: lit while the input reaches the shell's clip level (|word| >= 0x7FFFF0), held 100 ms.
    bool inputClip() const { return inputClip_.load(std::memory_order_relaxed); }
    static constexpr int32_t kClipWord = 0x7FFFF0;
    static constexpr uint64_t kClipHoldSamples = 4410;   // 100 ms at 44.1 kHz
    int latencySamples() const { return latency_.load(); }   // host samples, valid after prepare()

    // Time spent in the last process() call by part (seconds; audio thread, read right after process()).
    // cpu = 68k and its devices (timers, serial, DMA); voice = voice chip; esp2 = ESP2 core and its sample edge;
    // resample = conversion to the host rate; queue = UI-to-audio events (buttons, knobs, MIDI scheduling).
    struct BlockTimes { double cpu = 0, voice = 0, esp2 = 0, resample = 0, queue = 0; };
    const BlockTimes& lastBlockTimes() const { return times_; }
    void setProfiling(bool on) { profiling_ = on; }   // per-part timing (a few clock reads per sample)

    // ---- idle runner (F-63): the machine runs only inside host blocks, and some hosts stop calling process() while
    // the window is open (Logic Pro renders an instrument only while its track is live or playing; REAPER can stop
    // with the transport). With setIdleRun(true), a background thread runs the machine in real time once no host
    // block came for kIdleQuietMs: panel buttons and knobs, the virtual port in and out, the editor's requests and
    // replies (and its standing read), LEDs and the display, reboot requests. Its audio is discarded, Audio In is
    // silence and the OS's MIDI output for the host is dropped. It never waits for the lock (try_lock; a step is
    // skipped when the host or a restore has it), holds it for at most kIdleSliceMs of machine time at a time, and
    // never runs while a machine boots, restores or is stopped. The next host block retimes. Off by default (offline
    // renders and the tests stay as they were); the plugin turns it on only while its window is open, the host renders
    // in real time and prepare() has run. Any thread.
    void setIdleRun(bool on);
    bool idleRunning() const { return idling_.load(); }
    uint64_t idleSlicesForTest() const { return idleSlices_.load(); }
    static constexpr int64_t kIdleQuietMs = 250, kIdlePollMs = 10, kIdleMaxStepMs = 50, kIdleSliceMs = 10;

    // ---- virtual MIDI port (the plugin's CoreMIDI binding)
    // MIDI in: raw bytes as they arrive, from one producer thread (the port's receive thread). They are cut into whole
    // messages here and passed to the audio thread through a lock-free queue; each block delivers the waiting
    // messages to the emulated MIDI port at the block's start (ahead of that block's host events, timed like them),
    // never inside another message. Running status is expanded; SysEx stays whole.
    void virtualMidiIn(const uint8_t* bytes, size_t n);
    // MIDI out: the OS's MIDI output as whole messages, for one consumer thread (the port's sender).
    MessageRing& virtualMidiOut() { return vout_; }
    void setVirtualPort(bool on) { virtualOn_ = on; }
    void setMidiToHost(bool on) { hostOutOn_ = on; }
    // The OS's MIDI output of the last process() for the host (empty unless MIDI to host is on). Audio thread, read
    // right after process(): whole messages at sample offsets within the block.
    struct MidiOutEvent { int offset; uint32_t start, size; };
    const std::vector<MidiOutEvent>& hostMidiEvents() const { return hostEvents_; }
    const uint8_t* hostMidiBytes() const { return hostBytes_.data(); }
    uint64_t virtualInDropped() const { return vinDropped_.load(); }   // incomplete, oversized or no room
    uint64_t virtualOutDropped() const { return voutDropped_.load(); }
    static constexpr size_t kMaxMidiMessage = size_t(512) << 10;   // longest SysEx passed whole (512 KB)
    static constexpr size_t kVirtualBacklog = 16384;   // virtual messages wait while this many port bytes are unread

    // ---- the plugin window's library (v0.9.0): a preset's stored message (into Compare) or a bank load. Whole
    // messages for the OS's MIDI input, as host SysEx goes in (message thread). The audio thread (or the idle runner)
    // queues each whole at a block's start, once the MIDI port's input queue has room for all of it
    // (Serial::kMidiRxRoom, made beforehand): nothing goes inside one, MIDI arriving meanwhile waits behind it as on a
    // cable (a bank takes about 33 s), and the audio thread neither allocates nor waits. False (nothing queued): the
    // message is empty or larger than kLibraryMessageMax, or the queue (256 KB) is full.
    bool libraryInput(const uint8_t* m, size_t n);
    // Messages queued or not yet all read by the OS.
    bool libraryInputBusy() const { return libPushed_.load() != libTaken_.load() || libInFlight_.load(); }
    static constexpr size_t kLibraryMessageMax = 102420;

    // ---- editor (docs/editor/EDITOR_SPEC.md, "MIDI coexistence"). Nothing here runs while the editor is closed.
    // Message thread. Closing drops what the editor still had queued; replies already requested are still kept out
    // of the MIDI output when they arrive (each request until kEditorReplyTimeoutSamples).
    void setEditorOpen(bool on);
    bool editorOpen() const { return editorOpen_.load(); }
    // A whole message for the OS's MIDI input (message thread). The audio thread queues it at a block's start, after
    // the virtual port's messages, whole and at cable speed like them; never inside another message. False when closed
    // or the queue is full.
    bool editorSend(const uint8_t* m, size_t n);
    // The read that refreshes the visible page (whole messages; message thread). The audio thread queues it directly
    // behind every program change, bank select or SysEx (a dump) going into the OS from the host or the virtual port,
    // so the read sees the new preset. Empty: none.
    void setEditorStandingRead(const std::vector<std::vector<uint8_t>>& msgs);
    // The OS's replies to the editor's requests (partial read 0x44 by the address it echoes, whole object 0x41 and the
    // OS's error message 0x7F in order), taken out of the MIDI output and passed here instead (audio thread -> message thread). All other output passes.
    MessageRing& editorReplies() { return edReplies_; }
    // Counters for the editor's re-read triggers (any thread):
    uint64_t ledStateMessages() const { return ledStateMessages_.load(); }   // LED on/off/flash (never beat flash 9D)
    uint64_t presetTriggers() const { return presetTriggers_.load(); }       // program change, bank select, SysEx into the OS
    uint64_t panelGestures() const { return panelGestures_.load(); }         // panel knob moves and button presses
    int lastPanelControl() const { return lastPanelControl_.load(); }        // the last panel control moved (0-25), -1: none
    uint64_t panelControlMoves() const { return panelControlMoves_.load(); } // panel knob moves only (Follow on knobs)
    uint64_t externalMidi() const { return externalMidi_.load(); }           // messages from the host and the virtual port (not clock)
    uint64_t editorRequestsForTest() const { return editorRequests_; }
    static constexpr uint64_t kEditorReplyTimeoutSamples = 5 * 44100;
    static constexpr size_t kStandingReadBytes = 4096;

    // ---- tests
    // The process's factory user flash for an OS image MD5: set it (no hidden machine is made then), and count the
    // hidden machines made so far.
    static void setFactoryUserFlashForTest(const std::string& osMd5, UserFlash data);
    static uint64_t factoryRunsForTest();
    std::vector<uint8_t> machineStateForTest();
    uint32_t peekForTest(uint32_t addr, int size);
    std::vector<uint64_t> midiQueueForTest();        // scheduled machine cycles of the MIDI events so far (last 64)
    uint64_t lateMidiForTest() const { return lateMidi_; }   // events that could not be scheduled on time
    uint64_t controlMessagesForTest() const { return controlMessages_; }   // Bx messages sent to the machine
    uint64_t buttonMessagesForTest() const { return buttonMessages_; }     // 80/81 messages sent to the machine
    // Longest time the audio thread waited for the engine lock (microseconds), since prepare().
    double maxLockWaitUs() const { return maxLockWaitUs_.load(); }
    uint64_t anchorForTest();
    void withMachineForTest(const std::function<void(Machine&)>& f);
    uint64_t inputUnderrunsForTest() const;
    double cpuHzForTest();

    static constexpr double kBootLimitMs = 8000, kSettleMs = 500;
    static constexpr const char* kBootDisplay = "P 01";

private:
    struct Job {
        std::string osPath, wavePath, osMd5;
        bool factoryOn = false;
        std::string factoryDir;
        UserFlash keep;                              // a reboot: the user flash the machine had (else the factory's)
    };
    void startJob(std::vector<uint8_t> state, UserFlash keep = nullptr);
    void runJob(Job job, std::vector<uint8_t> state, uint64_t generation);
    bool coldBoot(Machine& m, bool publish, std::string& err);
    UserFlash factoryUserFlash(const Job& job, const OsImage& os);
    void stopJob();
    void install(std::unique_ptr<Machine> m, uint64_t generation, uint8_t rawPlus, uint8_t rawMinus);
    void publishDisplay(Machine& m);
    void setMessage(const std::string& s);
    void fail(const std::string& reason);
    void drainButtons(Machine& m);
    void publishPositions(const Machine& m);
    bool push(uint32_t entry);                       // false: the queue is full (the edge is dropped)
    void attachLeds(Machine& m);
    void sendControls(Machine& m);
    void serveSnapshot(Machine& m);
    void publishLeds(const Machine& m);
    void midiOut(Machine& m, int n, bool toHost = true);
    void emitMidiOut(const uint8_t* d, size_t size, int offset, bool toHost = true);
    void takeVirtualIn(Machine& m, uint64_t at, bool editor);
    void takeEditorOut(Machine& m, uint64_t at);
    void takeLibraryIn(Machine& m, uint64_t at, bool editor);
    void expireRequests(const Machine& m);
    void machineStopped(Machine& m, int stop);       // a Machine::Stop of Trap or HardStall: why, then Stopped
    void idleLoop();
    bool idleSlice(int64_t ms);                      // under mtx_; false: not now (host back, not running)
    void queueEditor(Machine& m, const uint8_t* d, size_t n, uint64_t at);
    void queueStandingRead(Machine& m, uint64_t at);

    // ROMs (message thread)
    std::string osPath_, wavePath_, osMd5_, waveMd5_;
    bool factoryOn_ = false;
    std::string factoryDir_;
    std::vector<uint8_t> pendingState_;              // a saved state waiting for the ROMs
    std::vector<uint8_t> jobState_;                  // the machine state the current job restores (under getStateMtx_):
                                                     // what getState returns until the machine runs

    // worker
    std::thread worker_;
    std::atomic<bool> cancel_{false};
    std::atomic<uint64_t> generation_{0};

    // running machine (audio thread), guarded by mtx_
    std::mutex mtx_;
    std::unique_ptr<Machine> live_;
    uint8_t rawPlus_ = 0, rawMinus_ = 0;             // panel ids of the +/Yes and -/No buttons (from the OS image)
    Resampler resampler_;
    double hostRate_ = 0;
    bool retime_ = true;                             // re-anchor host and machine time on the next block
    uint64_t anchorSample_ = 0;                      // machine sample at host sample 0 of this run
    int64_t hostPos_ = 0;
    int marginIn_ = 48;                              // input frames of slack for MIDI scheduling
    std::vector<float> inL_, inR_;
    std::array<uint64_t, 64> midiTimes_{};           // test record of scheduled MIDI times
    BlockTimes times_;
    std::atomic<bool> profiling_{false};
    uint64_t midiCount_ = 0, lateMidi_ = 0;

    // Audio In: host rate -> 44.1 kHz -> a FIFO the machine takes one frame from per sample period
    struct InputFifo;
    std::unique_ptr<InputFifo> fifo_;
    Resampler inRes_;
    bool inputActive_ = false;
    int prefill_ = 0;
    std::vector<float> inConvL_, inConvR_;
    std::atomic<bool> inputClip_{false};

    // MIDI ports: virtual port in (producer thread -> audio thread), OS output (audio thread -> sender, host)
    MidiParser vinParser_{kMaxMidiMessage};           // producer thread
    MessageRing vin_{size_t(1) << 20}, vout_{size_t(1) << 20};
    MidiParser outParser_{kMaxMidiMessage};           // audio thread
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(size_t(1) << 20);
    std::vector<uint8_t> hostBytes_ = std::vector<uint8_t>(size_t(1) << 20);
    std::vector<MidiOutEvent> hostEvents_;
    size_t hostUsed_ = 0;
    std::atomic<bool> virtualOn_{false}, hostOutOn_{false};
    std::atomic<uint64_t> vinDropped_{0}, voutDropped_{0}, hostOutDropped_{0};

    // the window's bank load (message thread -> audio thread)
    std::mutex libInMtx_;
    MessageRing libIn_{size_t(1) << 18};
    std::atomic<uint64_t> libPushed_{0}, libTaken_{0};   // messages queued here / passed to the MIDI port
    std::atomic<bool> libInFlight_{false};           // the last one passed is not yet all read by the OS
    uint64_t libDoneAt_ = 0;                         // audio thread: the port's read count once its last byte is read

    // editor: requests in (message thread -> audio thread), replies out (audio thread -> message thread)
    std::atomic<bool> editorOpen_{false};
    std::mutex edSendMtx_;                           // editorSend may be called from more than one UI thread
    MessageRing edOut_{size_t(1) << 16}, edReplies_{size_t(1) << 20};
    // the standing read: a seqlock-guarded copy (message thread writes, audio thread copies at a trigger)
    std::mutex standingMtx_;
    std::atomic<uint32_t> standingSeq_{0};
    std::array<std::atomic<uint8_t>, kStandingReadBytes> standing_{};   // [len hi, len lo, bytes]... then a 0 length
    std::array<uint8_t, kStandingReadBytes> standingCopy_{};            // audio thread
    // Audio thread: the requests the OS has not answered yet, oldest first, each with its sender (the editor, or the
    // host or the virtual port). A partial-read reply (0x44) goes to the oldest request with the address it echoes;
    // a whole-object reply (0x41) to the oldest whole read; the OS's error message (0x7F) answers the oldest request
    // of any kind. Each request expires on its own after kEditorReplyTimeoutSamples (some are never answered).
    struct PendingRequest {
        uint64_t sample = 0;
        bool editor = false;
        uint8_t command = 0;                         // kPartialRead or kWholeRead
        edproto::Address address;                    // partial read: the whole address; whole read: type and number
    };
    std::array<PendingRequest, 256> pending_{};
    size_t pendingCount_ = 0;
    void noteRequest(bool editor, const uint8_t* d, size_t n, uint64_t sample);
    // The sender of the request this OS output answers (consumed), or -1 if it answers none: 1 the editor, 0 another.
    int replyOwner(const uint8_t* d, size_t n);
    uint64_t editorRequests_ = 0;
    std::atomic<uint64_t> ledStateMessages_{0}, presetTriggers_{0}, panelGestures_{0};
    std::atomic<int> lastPanelControl_{-1};
    std::atomic<uint64_t> panelControlMoves_{0};
    std::atomic<uint64_t> externalMidi_{0};          // counted only (the editor stops an arp edit when MIDI arrives)

    // status
    std::atomic<State> state_{State::NoRoms};
    std::atomic<bool> rebootRequested_{false};
    std::atomic<int> latency_{0};
    std::atomic<uint32_t> chars_{0x20202020};
    std::atomic<uint32_t> segs_{0};
    std::array<std::atomic<uint8_t>, 256> ledMode_{};
    std::array<std::atomic<uint32_t>, 256> beats_{};
    std::array<std::atomic<float>, 256> beatIntervalMs_{};
    std::atomic<uint32_t> dots_{0};
    std::atomic<uint32_t> uiSerial_{0};
    std::atomic<uint64_t> panelMessages_{0};
    mutable std::mutex msgMtx_;
    std::string message_;

    // state snapshots for getState (taken by the audio thread)
    std::mutex getStateMtx_;                         // one getState at a time
    std::atomic<uint32_t> snapRequest_{0}, snapDone_{0};
    std::vector<uint8_t> snap_;                      // written by the audio thread between request and done
    std::atomic<int64_t> lastProcessMs_{-100000};    // when process() last ran (steady clock, ms)
    std::atomic<double> maxLockWaitUs_{0};

    // idle runner: its thread starts with the first setIdleRun(true) and stops in the destructor, before the machine
    std::mutex idleMtx_;
    std::thread idle_;
    std::atomic<bool> idleOn_{false}, idleStop_{false}, idling_{false};
    std::atomic<uint64_t> idleSlices_{0};

    // panel controls: latest value per cc (bit 15 = new), sent by the audio thread, paced
    std::array<std::atomic<uint16_t>, 26> pendingControl_{};
    std::array<uint64_t, 26> controlSentAt_{};       // machine cycle of each control's last message (audio thread)
    uint64_t controlMessages_ = 0, buttonMessages_ = 0;

    // panel buttons: a bounded lock-free queue, never merged. Any number of producers (the message thread, the skin's
    // render thread, the audio thread's learned buttons) reserve a cell with one compare-and-swap; the audio thread
    // consumes the cells in reservation order (a cell still being written waits for the next block). Entries:
    // (kind << 24) | payload.
    static constexpr uint32_t kButtonRing = 1024;    // a power of two
    struct ButtonCell { std::atomic<uint32_t> seq{0}; uint32_t value = 0; };
    std::array<ButtonCell, kButtonRing> buttons_;
    std::atomic<uint32_t> btnEnqueue_{0};
    uint32_t btnDequeue_ = 0;                         // audio thread
    std::array<std::atomic<uint32_t>, 4> heldRaw_{};  // the raw buttons down now (one bit each)
    std::atomic<uint32_t> heldOs_{0};                 // pressButton's -/No and +/Yes down now
    std::array<std::atomic<uint16_t>, 26> positions_{};          // current control positions (for the UI)
    // Positions for the next cold boot (what the panel answers to the OS's F4), one atomic per control: setControl
    // writes the new position from any thread without a lock; a new machine copies them one by one. Each element is
    // always a whole position; a move that lands while a new machine copies them is still pending (pendingControl_)
    // and goes to that machine once it runs, and sendControls writes back each position it sent, so they converge on
    // the last position set.
    std::array<std::atomic<uint16_t>, 26> controls_{};
    std::atomic<int> rawPlusPub_{-1}, rawMinusPub_{-1};   // rawPlus_ / rawMinus_ for other threads (-1: no machine yet)
};
