#include "PluginProcessor.h"
#include <chrono>
#include "PluginEditor.h"
#include "SkinSettings.h"
#include "os_image.h"
#include <algorithm>

namespace {
constexpr uint32_t kStateMagic = 0x505A4850;     // "PHZP" (little-endian)
constexpr uint32_t kStateVersion = 5;          // 2: + skin knob positions; 3: + window size; 4: + its scale's version;
                                                // 5: + the editor unit open (older still read)
constexpr int kTimerMs = 100, kScanEveryTicks = 20;   // rescan the ROM folder every 2 s while something is missing

std::mutex g_instancesMtx;
std::vector<PhyzoProcessor*> g_instances;            // open instances (for global settings)
}

PhyzoProcessor::PhyzoProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Audio In", juce::AudioChannelSet::stereo(), false)   // ESP2 line input; off by default
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)) {
    dataFolder_ = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("Phyzo");
    dataFolder_.getChildFile("roms").createDirectory();
    dataFolder_.getChildFile("skins").createDirectory();
    events_.reserve(4096);
    port_.ed = &editorPort_;
    // The factory user flash (what the OS's own factory restore writes): made once per OS image from the user's own
    // OS, cached in cache/ (never shipped). New instances start from it; old projects get it (Engine).
    engine_.setFactoryUserFlash(true, dataFolder_.getChildFile("cache").getFullPathName().toStdString());
    scanRoms();
    applyMidiSettings();
    { std::lock_guard<std::mutex> lk(g_instancesMtx); g_instances.push_back(this); }
    startTimer(kTimerMs);
}

PhyzoProcessor::~PhyzoProcessor() {
    stopTimer();
    editorTicker_.stopTimer();
    port_.ed = nullptr;
    { std::lock_guard<std::mutex> lk(g_instancesMtx); g_instances.erase(std::find(g_instances.begin(), g_instances.end(), this)); }
    vport_.close();
}

// Virtual port on/off and MIDI to host, from the global settings (message thread).
void PhyzoProcessor::applyMidiSettings() {
    if (SkinSettings::virtualMidiPort()) vport_.open(); else vport_.close();
    engine_.setVirtualPort(vport_.isOpen());
    engine_.setMidiToHost(SkinSettings::midiToHost());
}

void PhyzoProcessor::applyMidiSettingsToAll() {
    std::lock_guard<std::mutex> lk(g_instancesMtx);
    for (PhyzoProcessor* p : g_instances) p->applyMidiSettings();
}

void PhyzoProcessor::scanRoms() {
    scan_ = romid::scanFolder(romFolder().getFullPathName().toStdString());
    if (scan_.complete()) {
        engine_.setRoms(scan_.osPath, scan_.wavePath, romid::kOsImageMd5, romid::kWaveImageMd5);
        loadEditorTables();
    }
}

// ------------------------------------------------------------------ editor unit (F-30)

// The editor's tables, read from the user's OS (decoded in memory if it is the update file), once per OS file.
void PhyzoProcessor::loadEditorTables() {
    if (scan_.osPath == editorTablesFor_) return;
    editorTablesFor_ = scan_.osPath;
    OsImage os;
    std::string err;
    edtables::OsTables t;
    if (os.load(scan_.osPath, err)) t = edtables::readAll(os.bytes);
    editor_.setTables(t);
}

void PhyzoProcessor::setEditorActive(bool on) {
    if (on == editorActive_.load()) return;
    editorActive_ = on;
    editor_.setOpen(on);
    if (on) editorTicker_.startTimer(20);            // replies, triggers, the 50 ms debounce
    else editorTicker_.stopTimer();
}

void PhyzoProcessor::postToEditor(std::function<void(editor::Controller&)> f) {
    std::lock_guard<std::mutex> lk(editorQueueMtx_);
    editorQueue_.push_back(std::move(f));
}

void PhyzoProcessor::editorTick() {
    std::vector<std::function<void(editor::Controller&)>> q;
    { std::lock_guard<std::mutex> lk(editorQueueMtx_); q.swap(editorQueue_); }
    for (auto& f : q) f(editor_);
    editor_.tick(juce::Time::getMillisecondCounterHiRes());
}

void PhyzoProcessor::loadWaveNames(const juce::File& skinFolder) {
    const juce::File f = skinFolder.getChildFile(kWaveNamesFile);
    editor_.setWaveNames(f.existsAsFile() ? editor::parseWaveNames(f.loadFileAsString().toStdString()) : std::vector<std::string>());
}

void PhyzoProcessor::EditorPort::editorState(skin::EditorState& out) {
    const editor::View v = proc.editor_.view();
    for (int r = 0; r < skin::EditorState::kRows; ++r) {
        out.rows[size_t(r)] = v.screen.rowText(r);
        out.underline[size_t(r)] = v.screen.underline[size_t(r)];
        out.dot[size_t(r)] = v.screen.dot[size_t(r)];
    }
    out.focusRow = v.focusRow; out.focusCol = v.focusCol; out.focusLen = v.focusLen;
    out.typing = v.typing;
    out.spans.clear();
    for (const auto& s : v.screen.values) out.spans.push_back({s.row, s.col, s.len, s.ref, s.pick});
    out.area = v.area; out.section = v.section;
    out.sound = v.sound; out.osc = v.osc; out.oscBoth = v.oscBoth;
    out.soundTargets = v.soundTargets; out.oscTargets = v.oscTargets;
    out.page = v.page; out.pages = v.pages;
    out.prev = v.prev; out.next = v.next; out.follow = v.follow; out.data = v.data; out.edited = v.edited;
    out.areas = v.areas; out.sections = v.sections;
    out.serial = v.serial;
}

void PhyzoProcessor::timerCallback() {
    engine_.service();
    if (!scan_.complete() && ++timerTicks_ >= kScanEveryTicks) {
        timerTicks_ = 0;
        if (!romFolder().isDirectory()) romFolder().createDirectory();
        scanRoms();
    }
}

void PhyzoProcessor::prepareToPlay(double sampleRate, int samplesPerBlock) {
    hostRate_ = sampleRate;
    meter.worstMs = 0; meter.overruns = 0;
    const auto* in = getBus(true, 0);
    inputActive_ = in != nullptr && in->isEnabled() && in->getNumberOfChannels() > 0;
    inL_.assign(size_t(std::max(1, samplesPerBlock)), 0.0f); inR_.assign(inL_.size(), 0.0f);
    engine_.prepare(sampleRate, samplesPerBlock, inputActive_);
    engine_.setProfiling(true);                      // per-part times for the debug overlay (output unchanged)
    setLatencySamples(engine_.latencySamples());
}

// Output stereo; Audio In off, mono (fed to both sides) or stereo.
bool PhyzoProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto in = layouts.getMainInputChannelSet();
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() &&
           (in.isDisabled() || in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void PhyzoProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) {
    juce::ScopedNoDenormals noDenormals;
    const auto t0 = std::chrono::steady_clock::now();
    const int n = buffer.getNumSamples();
    events_.clear();
    for (const auto meta : midi)
        if (events_.size() < events_.capacity()) events_.push_back({meta.samplePosition, meta.data, meta.numBytes});
    midi.clear();
    // Audio In shares the buffer with the output (in place): copy it out first.
    const float* inL = nullptr; const float* inR = nullptr;
    if (inputActive_ && n <= int(inL_.size())) {
        const auto inBus = getBusBuffer(buffer, true, 0);
        if (inBus.getNumChannels() > 0) {
            std::copy(inBus.getReadPointer(0), inBus.getReadPointer(0) + n, inL_.begin());
            const float* r = inBus.getReadPointer(inBus.getNumChannels() > 1 ? 1 : 0);
            std::copy(r, r + n, inR_.begin());
            inL = inL_.data(); inR = inR_.data();
        }
    }
    if (buffer.getNumChannels() >= 2) {
        engine_.process(buffer.getWritePointer(0), buffer.getWritePointer(1), n, events_.data(), int(events_.size()), inL, inR);
        for (const auto& e : engine_.hostMidiEvents())   // the OS's MIDI output (only when MIDI to host is on)
            midi.addEvent(engine_.hostMidiBytes() + e.start, int(e.size), e.offset);
    } else buffer.clear();

    // CPU per block: processing time against the block's real-time duration.
    const double used = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const double budget = n / std::max(1.0, hostRate_.load());
    const double load = budget > 0 ? used / budget : 0;
    meter.last = float(load); meter.lastMs = float(used * 1000); meter.blockMs = float(budget * 1000); meter.blockSize = n;
    if (load > 1.0) ++meter.overruns;
    if (float(used * 1000) > meter.worstMs.load()) meter.worstMs = float(used * 1000);
    winTime_ += used; winBudget_ += budget; winPeak_ = std::max(winPeak_, load);
    const Engine::BlockTimes& bt = engine_.lastBlockTimes();
    const double parts[Meter::kParts] = {bt.cpu, bt.voice, bt.esp2, bt.resample, bt.queue};
    for (int i = 0; i < Meter::kParts; ++i) {
        winPart_[size_t(i)] += parts[i];
        if (budget > 0) winPartPeak_[size_t(i)] = std::max(winPartPeak_[size_t(i)], parts[i] / budget);
    }
    if (winBudget_ >= 1.0) {
        const float avg = float(winTime_ / winBudget_);
        meter.avg = avg; meter.peak = float(winPeak_);
        for (size_t i = 0; i < size_t(Meter::kParts); ++i) {
            meter.partAvg[i] = float(winPart_[i] / winBudget_);
            meter.partPeak[i] = float(winPartPeak_[i]);
            winPart_[i] = winPartPeak_[i] = 0;
        }
        const uint32_t h = meter.head.load();
        meter.history[h % meter.history.size()] = avg;
        meter.head = h + 1;
        winTime_ = winBudget_ = winPeak_ = 0;
    }
}

juce::AudioProcessorEditor* PhyzoProcessor::createEditor() { return new PhyzoEditor(*this); }

// State: magic, version, UI flags, the engine state (complete machine, gzip-compressed), then the skin's knob
// element positions (count, then id and raw for each), then the window size (winscale) and whether it is still on
// the old scale (version 3 projects store sizes relative to the full body size), then whether the editor unit is
// open (version 5).
void PhyzoProcessor::getStateInformation(juce::MemoryBlock& dest) {
    ++meter.stateRequests;
    const std::vector<uint8_t> engineState = engine_.getState();
    juce::MemoryOutputStream out(dest, false);
    out.writeInt(int(kStateMagic));
    out.writeInt(int(kStateVersion));
    out.writeInt(showDebug ? 1 : 0);
    juce::MemoryBlock packed;
    {
        juce::MemoryOutputStream raw(packed, false);
        juce::GZIPCompressorOutputStream gz(raw, 6);
        gz.write(engineState.data(), engineState.size());
    }
    out.writeInt64(juce::int64(engineState.size()));
    out.writeInt64(juce::int64(packed.getSize()));
    out.write(packed.getData(), packed.getSize());
    std::lock_guard<std::mutex> lk(port_.mtx);
    out.writeInt(int(port_.knobs.size()));
    for (const auto& kv : port_.knobs) { out.writeString(juce::String::fromUTF8(kv.first.c_str())); out.writeInt(kv.second); }
    out.writeFloat(editorScale.load());
    out.writeByte(editorScaleLegacy.load() ? 1 : 0);
    out.writeByte(editorWanted_.load() ? 1 : 0);
}

void PhyzoProcessor::setStateInformation(const void* data, int size) {
    juce::MemoryInputStream in(data, size_t(size), false);
    if (uint32_t(in.readInt()) != kStateMagic) return;
    const int version = in.readInt();
    if (version < 1 || version > int(kStateVersion)) return;
    showDebug = in.readInt() != 0;
    const juce::int64 rawSize = in.readInt64(), packedSize = in.readInt64();
    if (rawSize <= 0 || rawSize > (juce::int64(256) << 20) || packedSize <= 0 || packedSize > in.getNumBytesRemaining()) return;
    juce::MemoryBlock packed;
    in.readIntoMemoryBlock(packed, packedSize);
    juce::MemoryInputStream packedIn(packed, false);
    juce::GZIPDecompressorInputStream gz(packedIn);
    std::vector<uint8_t> engineState(static_cast<size_t>(rawSize));
    if (gz.read(engineState.data(), int(rawSize)) != int(rawSize)) return;
    std::map<std::string, int> knobs;
    if (version >= 2) {
        const int n = in.readInt();
        for (int i = 0; i < n && i < 4096 && !in.isExhausted(); ++i) {
            const juce::String id = in.readString();
            knobs[id.toStdString()] = juce::jlimit(0, 1023, in.readInt());
        }
    }
    if (version >= 3 && !in.isExhausted()) {
        editorScale = in.readFloat();
        editorScaleLegacy = version == 3 || (!in.isExhausted() && in.readByte() != 0);
    }
    if (version >= 5 && !in.isExhausted()) editorWanted_ = in.readByte() != 0;
    { std::lock_guard<std::mutex> lk(port_.mtx); port_.knobs = std::move(knobs); }
    engine_.setState(engineState);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PhyzoProcessor(); }
