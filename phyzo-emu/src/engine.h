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
#include "midi_stream.h"
#include "resampler.h"

class Machine;

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
    // Front-panel button press/release by OS button id (0 = -/No, 1 = +/Yes); lock-free, message thread.
    void pressButton(int osButton, bool down);
    // An analog panel control moved (docs/PANEL_CONTROLS.md): cc 0-25, raw 0-1023. Lock-free, any UI thread.
    // Like the real panel's serial link, each control sends at most one Bx cc vv per kControlIntervalMs (the latest
    // value wins) and only while the link is not backed up; buttons are never dropped or merged. The position is kept
    // (saved with the machine state) and is what the panel answers to the OS's F4 request when a machine boots.
    void setControl(int cc, int raw);
    static constexpr double kControlIntervalMs = 10;
    static constexpr size_t kPanelBacklogBytes = 9;  // knob messages wait while more bytes than this are unread
    int controlPosition(int cc) const { return cc >= 0 && cc < 26 ? positions_[size_t(cc)].load() : 0; }
    // A front-panel button by its panel id (raw): sends 81 raw (down) or 80 raw (up). Lock-free for the audio thread.
    void pressRaw(int raw, bool down);

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

    // ---- editor (docs/editor/EDITOR_SPEC.md, "MIDI coexistence"). Nothing here runs while the editor is closed.
    // Message thread. Closing drops what the editor still had queued; replies already requested are still kept out
    // of the MIDI output when they arrive (or until kEditorReplyTimeoutSamples).
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
    // The OS's replies to the editor's requests (partial read 0x44, whole object 0x41 while a request is pending),
    // taken out of the MIDI output and passed here instead (audio thread -> message thread). All other output passes.
    MessageRing& editorReplies() { return edReplies_; }
    // Counters for the editor's re-read triggers (any thread):
    uint64_t ledStateMessages() const { return ledStateMessages_.load(); }   // LED on/off/flash (never beat flash 9D)
    uint64_t presetTriggers() const { return presetTriggers_.load(); }       // program change, bank select, SysEx into the OS
    uint64_t panelGestures() const { return panelGestures_.load(); }         // panel knob moves and button presses
    int lastPanelControl() const { return lastPanelControl_.load(); }        // the last panel control moved (0-25), -1: none
    uint64_t panelControlMoves() const { return panelControlMoves_.load(); } // panel knob moves only (Follow on knobs)
    uint64_t externalMidi() const { return externalMidi_.load(); }           // messages from the host and the virtual port
    uint64_t editorRequestsForTest() const { return editorRequests_; }
    static constexpr uint64_t kEditorReplyTimeoutSamples = 5 * 44100;
    static constexpr size_t kStandingReadBytes = 4096;

    // ---- tests
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
    void startJob(std::vector<uint8_t> state);
    void runJob(std::string osPath, std::string wavePath, std::vector<uint8_t> state, uint64_t generation);
    void stopJob();
    void install(std::unique_ptr<Machine> m, uint64_t generation, uint8_t rawPlus, uint8_t rawMinus);
    void publishDisplay(Machine& m);
    void setMessage(const std::string& s);
    void fail(const std::string& reason);
    void drainButtons(Machine& m);
    void publishPositions(const Machine& m);
    void push(uint32_t entry);
    void attachLeds(Machine& m);
    void sendControls(Machine& m);
    void serveSnapshot(Machine& m);
    void publishLeds(const Machine& m);
    void midiOut(Machine& m, int n);
    void emitMidiOut(const uint8_t* d, size_t size, int offset);
    void queueEditor(Machine& m, const uint8_t* d, size_t n, uint64_t at);
    void queueStandingRead(Machine& m, uint64_t at);

    // ROMs (message thread)
    std::string osPath_, wavePath_, osMd5_, waveMd5_;
    std::vector<uint8_t> pendingState_;              // a saved state waiting for the ROMs

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

    // editor: requests in (message thread -> audio thread), replies out (audio thread -> message thread)
    std::atomic<bool> editorOpen_{false};
    std::mutex edSendMtx_;                           // editorSend may be called from more than one UI thread
    MessageRing edOut_{size_t(1) << 16}, edReplies_{size_t(1) << 20};
    // the standing read: a seqlock-guarded copy (message thread writes, audio thread copies at a trigger)
    std::mutex standingMtx_;
    std::atomic<uint32_t> standingSeq_{0};
    std::array<std::atomic<uint8_t>, kStandingReadBytes> standing_{};   // [len hi, len lo, bytes]... then a 0 length
    std::array<uint8_t, kStandingReadBytes> standingCopy_{};            // audio thread
    // Audio thread: who sent each request the OS has not answered yet (1 = the editor, 0 = the host or the virtual
    // port), oldest first; the OS answers in order, so each reply goes to the sender of the oldest request.
    std::array<uint8_t, 256> replyOwners_{};
    size_t ownersHead_ = 0, ownersCount_ = 0;
    void noteRequest(bool editor, uint64_t sample);
    uint64_t lastRequestSample_ = 0;
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

    // panel controls: latest value per cc (bit 15 = new), sent by the audio thread, paced
    std::array<std::atomic<uint16_t>, 26> pendingControl_{};
    std::array<uint64_t, 26> controlSentAt_{};       // machine cycle of each control's last message (audio thread)
    uint64_t controlMessages_ = 0, buttonMessages_ = 0;

    // panel buttons: ring (UI threads) -> audio thread, never merged
    static constexpr int kButtonRing = 1024;
    std::array<std::atomic<uint32_t>, kButtonRing> buttons_{};   // (kind << 24) | payload
    std::mutex pushMtx_;                                         // producers: message thread and skin render thread
    std::array<std::atomic<uint16_t>, 26> positions_{};          // current control positions (for the UI)
    std::mutex controlsMtx_;
    std::array<uint16_t, 26> controls_{};                          // positions for the next cold boot
    std::atomic<uint32_t> btnHead_{0}, btnTail_{0};
};
