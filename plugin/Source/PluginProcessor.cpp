#include "PluginProcessor.h"
#include <chrono>
#include "PluginEditor.h"
#include "SkinSettings.h"
#include "os_image.h"
#include <algorithm>

namespace {
constexpr uint32_t kStateMagic = 0x505A4850;     // "PHZP" (little-endian)
constexpr uint32_t kStateVersion = 6;          // 2: + skin knob positions; 3: + window size; 4: + its scale's version;
                                                // 5: + the editor unit open; 6: + the MIDI learn bindings (older still read)
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
    port_.learn = &learn_;
    learn_.setBindings(learnDefaults());            // a new instance: the default bindings (none unless saved)
    // The factory user flash (what the OS's own factory restore writes): made once per OS image from the user's own
    // OS, cached in cache/ (never shipped). New instances start from it (Engine).
    engine_.setFactoryUserFlash(true, dataFolder_.getChildFile("cache").getFullPathName().toStdString());
    editor_.setLibrary(&libraryBridge_);
    scanRoms();
    applyMidiSettings();
    { std::lock_guard<std::mutex> lk(g_instancesMtx); g_instances.push_back(this); }
    startTimer(kTimerMs);
}

PhyzoProcessor::~PhyzoProcessor() {
    learn_.releaseHeld();                            // a learned button held: released first
    stopTimer();
    editorTicker_.stopTimer();
    chooser_.reset();
    library_.reset();                                // cancels the running library job and joins its thread
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

// ------------------------------------------------------------------ preset library (v0.8.3 step 2)

library::Service* PhyzoProcessor::libraryService() {
    if (!library_ && scan_.complete()) {
        auto machines = std::make_unique<library::RealMachines>(scan_.osPath, scan_.wavePath,
                                                                 [] { return Engine::factoryUserFlashFor(romid::kOsImageMd5); });
        library::Service::Config cfg{dataFolder_.getFullPathName().toStdString(), romid::kOsImageMd5,
                                     [this] { return engine_.machineState(); }};
        library_ = std::make_unique<library::Service>(std::move(cfg), std::move(machines));
    }
    return library_.get();
}

std::vector<editor::LibraryLink::Source> PhyzoProcessor::LibraryBridge::sources() const {
    std::vector<Source> out;
    if (proc.library_)
        for (const auto& s : proc.library_->sources()) out.push_back({s.title, s.count, s.done, s.total, s.busy});
    return out;
}

std::vector<editor::LibraryLink::Entry> PhyzoProcessor::LibraryBridge::entries(int source) const {
    std::vector<Entry> out;
    if (proc.library_)
        for (const auto& e : proc.library_->entries(source)) out.push_back({e.name, e.ready, e.accepted});
    return out;
}

bool PhyzoProcessor::LibraryBridge::takeNotice(std::string& text) {
    if (!proc.libraryNotices_.empty()) {
        text = proc.libraryNotices_.front();
        proc.libraryNotices_.pop_front();
        return true;
    }
    return proc.library_ && proc.library_->takeNotice(text);
}

// IMPORT: .syx files chosen in the open dialog are copied into the library (never overwriting) and read.
void PhyzoProcessor::chooseImport() {
    if (chooserBusy_ || !libraryService()) return;
    chooserBusy_ = true;
    chooser_ = std::make_unique<juce::FileChooser>("Import presets into the library", juce::File(), "*.syx");
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [this](const juce::FileChooser& fc) {
                              std::vector<std::string> paths;
                              for (const juce::File& f : fc.getResults()) paths.push_back(f.getFullPathName().toStdString());
                              if (!paths.empty()) if (library::Service* s = libraryService()) s->import(paths);
                              chooserBusy_ = false;
                          });
}

void PhyzoProcessor::showExportMenu() {
    if (auto* ed = dynamic_cast<PhyzoEditor*>(getActiveEditor())) ed->showExportMenu();
}

void PhyzoProcessor::openEditorUnit() {
    if (auto* ed = dynamic_cast<PhyzoEditor*>(getActiveEditor())) ed->openEditorUnit();
}

// FOLDER: the Library folder opened in Finder (or Explorer).
void PhyzoProcessor::revealLibrary() {
    const juce::File lib = dataFolder_.getChildFile("Library");
    lib.createDirectory();
    lib.startAsProcess();
}

// EXPORT B1 / B2: once the clone has dumped the bank, the save dialog (default name Phyzo Bank n.syx).
void PhyzoProcessor::serviceLibrary() {
    if (chooser_ && !chooserBusy_) chooser_.reset();   // a dialog that has closed
    int bank = 0;
    std::vector<uint8_t> bytes;
    if (chooserBusy_ || !library_ || !library_->takeExport(bank, bytes)) return;
    chooserBusy_ = true;
    const juce::String name = "Phyzo Bank " + juce::String(bank + 1) + ".syx";
    chooser_ = std::make_unique<juce::FileChooser>("Export Bank " + juce::String(bank + 1),
                                                   dataFolder_.getChildFile("Library").getChildFile(name), "*.syx");
    auto data = std::make_shared<std::vector<uint8_t>>(std::move(bytes));
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, bank, data](const juce::FileChooser& fc) {
                              juce::File f = fc.getResult();
                              if (f != juce::File()) {
                                  if (!f.hasFileExtension("syx")) f = f.withFileExtension("syx");
                                  libraryNotice(f.replaceWithData(data->data(), data->size())
                                                    ? "EXPORTED BANK " + std::to_string(bank + 1)
                                                    : std::string("NOT EXPORTED  CANNOT WRITE THE FILE"));
                              }
                              chooserBusy_ = false;
                          });
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

void PhyzoProcessor::runEditorQueue() {
    std::vector<std::function<void(editor::Controller&)>> q;
    { std::lock_guard<std::mutex> lk(editorQueueMtx_); q.swap(editorQueue_); }
    for (auto& f : q) f(editor_);
}

void PhyzoProcessor::editorTick() {
    runEditorQueue();
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
    out.open = proc.editorActive_.load();
    out.serial = v.serial;
}

void PhyzoProcessor::timerCallback() {
    engine_.service();
    serviceLearn();
    // The skin's editor calls while the editor unit is closed: the library controls in the main document (LIBRARY
    // opens the unit; SAVE, IMPORT, EXPORT and FOLDER work without it). While it is open, its own ticker runs them.
    if (!editorActive_.load()) runEditorQueue();
    serviceLibrary();
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
    // MIDI learn: bound messages (and the one that completes a learn) drive the panel and are taken out; everything
    // else passes as it came. No lock, no allocation (with no bindings, nothing is even looked at).
    events_.resize(size_t(learn_.process(events_.data(), int(events_.size()), learnClockMs_)));
    learnClockMs_ += n * 1000.0 / std::max(1.0, hostRate_.load());
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
// open (version 5), then the MIDI learn bindings (version 6: count, then for each: control id (int), channel (0 =
// Any), CC or note number, kind (0 CC, 1 note), 14-bit seen; an empty list means no bindings).
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
    const std::vector<midilearn::Binding> bindings = learn_.bindings();
    out.writeInt(int(bindings.size()));
    for (const midilearn::Binding& b : bindings) {
        out.writeInt(b.control);
        out.writeByte(char(b.channel));
        out.writeByte(char(b.number));
        out.writeByte(b.note ? 1 : 0);
        out.writeByte(b.fine ? 1 : 0);
    }
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
    // MIDI learn: the project's bindings (version 6, possibly none); an older project gets the default bindings. A
    // learned button held now is released first.
    std::vector<midilearn::Binding> bindings;
    if (version >= 6) {
        const int count = in.getNumBytesRemaining() >= 4 ? in.readInt() : 0;
        for (int i = 0; i < count && i < 1024 && in.getNumBytesRemaining() >= 8; ++i) {
            midilearn::Binding b;
            b.control = in.readInt();
            b.channel = uint8_t(in.readByte());
            b.number = uint8_t(in.readByte());
            b.note = in.readByte() != 0;
            b.fine = in.readByte() != 0;
            bindings.push_back(b);
        }
    } else {
        bindings = learnDefaults();
    }
    learn_.setBindings(bindings);
    { std::lock_guard<std::mutex> lk(port_.mtx); port_.knobs = std::move(knobs); }
    engine_.setState(engineState);
}

// ------------------------------------------------------------------ MIDI learn (F-55, docs/MIDI_LEARN.md)

std::vector<midilearn::Binding> PhyzoProcessor::learnDefaults() {
    return midilearn::Learn::fromText(SkinSettings::midiLearnDefaults().toStdString());
}

// Timer: what the audio thread heard. A learned message becomes a binding on the channel it came on (moving it from
// another control if one held it); a learned button that sent only one edge gets a warning (the binding stays). A
// learned button left down with no binding (it was moved or forgotten while a message was on its way) is released.
void PhyzoProcessor::serviceLearn() {
    midilearn::Event e;
    while (learn_.takeEvent(e)) {
        const midilearn::PerfLabels labels = perfLabels();
        midilearn::Binding b;
        if (e.kind == midilearn::Event::Learned) {
            std::vector<int> moved;
            if (learn_.commit(e, &moved) && learn_.bindingOf(e.control, b))
                learnMessage(moved.empty() ? midilearn::msgBound(b, labels) : midilearn::msgMoved(b, moved, labels));
        } else if (e.kind == midilearn::Event::OneEdge && learn_.bindingOf(e.control, b)) {
            learnMessage(midilearn::msgOneEdge(b, labels));
        }
    }
    learn_.releaseOrphans();
}

// Row 4 of the editor unit while it shows (one message at a time, 3 s each, as the library's), else the window's
// banner for 3 s; with no window, nowhere.
void PhyzoProcessor::learnMessage(const std::string& text) {
    if (editorActive_.load()) { libraryNotice(text); return; }
    if (auto* ed = dynamic_cast<PhyzoEditor*>(getActiveEditor())) ed->showLearnMessage(juce::String::fromUTF8(text.c_str()));
}

void PhyzoProcessor::armLearn(int control) {
    if (!midilearn::learnable(control)) return;
    if (learn_.armed() == control) { learn_.arm(midilearn::kNone); return; }   // MIDI learn again: cancelled
    learn_.arm(control);
    learnMessage(midilearn::msgLearn(control, perfLabels()));
}

bool PhyzoProcessor::cancelLearn() {
    if (learn_.armed() == midilearn::kNone) return false;
    learn_.arm(midilearn::kNone);
    return true;
}

void PhyzoProcessor::setLearnCc(int control, int cc) {
    midilearn::Binding b, nb;
    nb.control = control; nb.number = cc;
    if (learn_.bindingOf(control, b)) nb.channel = b.channel;   // its channel; Any if it had none
    std::vector<int> moved;
    if (!learn_.bind(nb, &moved) || !learn_.bindingOf(control, b)) return;
    const midilearn::PerfLabels labels = perfLabels();
    learnMessage(moved.empty() ? midilearn::msgBound(b, labels) : midilearn::msgMoved(b, moved, labels));
}

void PhyzoProcessor::setLearnChannel(int control, int channel) {
    midilearn::Binding b;
    if (!learn_.bindingOf(control, b) || b.channel == channel) return;
    b.channel = channel;
    std::vector<int> moved;
    if (!learn_.bind(b, &moved, false) || !learn_.bindingOf(control, b)) return;
    const midilearn::PerfLabels labels = perfLabels();
    learnMessage(moved.empty() ? midilearn::msgBound(b, labels) : midilearn::msgMoved(b, moved, labels));
}

void PhyzoProcessor::forgetLearn(int control) {
    midilearn::Binding b;
    if (!learn_.bindingOf(control, b) || !learn_.forget(control)) return;
    learnMessage(midilearn::msgForgotten(b, perfLabels()));
}

void PhyzoProcessor::forgetAllLearn() { learn_.forgetAll(); }

void PhyzoProcessor::saveLearnDefaults() { SkinSettings::setMidiLearnDefaults(juce::String(midilearn::Learn::toText(learn_.bindings()))); }

void PhyzoProcessor::clearLearnDefaults() { SkinSettings::clearMidiLearnDefaults(); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PhyzoProcessor(); }
