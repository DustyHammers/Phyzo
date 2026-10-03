// Phyzo audio processor: the emulated machine (Engine) as a VST3/AU instrument.
// Data folder: ~/Documents/Phyzo/ with roms/ (the two ROM files, found by checksum) and skins/.
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "engine.h"
#include "rom_id.h"
#include "skin_port.h"
#include "VirtualMidiPort.h"

class PhyzoProcessor : public juce::AudioProcessor, private juce::Timer {
public:
    PhyzoProcessor();
    ~PhyzoProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    using AudioProcessor::processBlock;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

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
        void sendControl(int cc, int raw) override { engine.setControl(cc, raw); }
        int controlPosition(int cc) override { return engine.controlPosition(cc); }
        void sendButton(int raw, bool down) override { engine.pressRaw(raw, down); }
        skin::LedReport led(int code) override {
            const Engine::LedInfo i = engine.led(code);
            return {skin::LedMode(i.mode), i.beats, i.beatIntervalMs};
        }
        std::array<uint8_t, 4> segments() override { return engine.segments(); }
        bool knobPosition(const std::string& id, int& raw) override {
            std::lock_guard<std::mutex> lk(mtx);
            auto it = knobs.find(id);
            if (it == knobs.end()) return false;
            raw = it->second;
            return true;
        }
        void setKnobPosition(const std::string& id, int raw) override { std::lock_guard<std::mutex> lk(mtx); knobs[id] = raw; }
        Engine& engine;
        std::mutex mtx;
        std::map<std::string, int> knobs;
    };

    void timerCallback() override;
    void scanRoms();
    void applyMidiSettings();
    static void virtualBytes(void* self, const uint8_t* bytes, size_t n) { static_cast<PhyzoProcessor*>(self)->engine_.virtualMidiIn(bytes, n); }

    Engine engine_;
    Port port_{engine_};
    VirtualMidiPort vport_{engine_.virtualMidiOut(), &PhyzoProcessor::virtualBytes, this};   // after engine_: closed first
    juce::File dataFolder_;
    romid::ScanResult scan_;
    int timerTicks_ = 0;
    std::atomic<double> hostRate_{0};
    std::vector<Engine::MidiEvent> events_;
    // meter accumulation (audio thread only)
    double winTime_ = 0, winBudget_ = 0, winPeak_ = 0;
    std::array<double, Meter::kParts> winPart_{}, winPartPeak_{};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhyzoProcessor)
};
