// Hidden machines for the preset library (v0.8.3 step 2; no plugin framework here). A hidden machine is a machine of
// its own that never reaches the audio, the host's MIDI or the UI. It runs on the library's worker thread and is driven
// only through the OS: panel presses (Dump, +/Yes), program changes, SysEx into its MIDI input and the editor's
// partial reads for names (type 7, offset 8, 16 bytes; #0 = the edit buffer, 0x40 + n = stored preset n).
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Machine;
struct OsImage;

namespace library {

// Panel ids (the raw codes of Engine::pressRaw) and the Compare LED code (Engine::led), from the rack skin's main
// document. The Compare LED is lit while the edit buffer differs from the stored preset: unsaved edits, or a received
// preset in Compare.
constexpr uint8_t kRawDump = 0x0C, kRawYes = 0x0D;
constexpr uint8_t kLedCompare = 0x16;

using Progress = std::function<void(size_t done, size_t total)>;

// What a library job asks of a hidden machine (HiddenMachine; tests use a fake). Every call waits on the OS (its
// display, its MIDI output) rather than a fixed delay, within a limit; false if the OS did not answer, with error().
class Ops {
public:
    virtual ~Ops() = default;
    // The message into the MIDI input; returns once the OS has taken all of it and answers again.
    virtual bool receive(const std::vector<uint8_t>& message, const Progress& progress) = 0;
    virtual bool programChange(int preset) = 0;              // 0-63; returns once the display shows P nn
    virtual bool readName(uint16_t number, std::string& name) = 0;   // trimmed
    virtual bool compareLit() = 0;
    // Waits until the display shows a preset (P nn): after a Dump the OS shows `donE` for a while, and a clone starts
    // with whatever the panel showed. A Dump pressed before that only clears `donE` and enters no Dump list.
    virtual bool waitForPreset() = 0;
    // The preset the display shows (0-63, after waitForPreset); -1 if it shows none.
    virtual int displayedPreset() { return -1; }
    // The panel's Dump of the current preset (Dump, Dump): its single-preset message. Waits for P nn first.
    virtual bool dumpPreset(std::vector<uint8_t>& message) = 0;
    // The panel's Dump of bAn1 (bank 0) or bAn2 (bank 1): its bank message. Waits for P nn first.
    virtual bool dumpBank(int bank, std::vector<uint8_t>& message, const Progress& progress) = 0;
    virtual std::string error() const = 0;
};

// Makes hidden machines.
class Machines {
public:
    virtual ~Machines() = default;
    // A machine booted the v0.8.3 way: the factory user flash seeded before it boots.
    virtual std::unique_ptr<Ops> fresh(const std::atomic<bool>& cancel, std::string& err) = 0;
    // A clone: a machine restored from the running machine's state (the running machine is not touched).
    virtual std::unique_ptr<Ops> clone(const std::vector<uint8_t>& machineState, const std::atomic<bool>& cancel, std::string& err) = 0;
};

// Hidden machines on the emulated hardware. The first fresh machine boots (several seconds); its state after the boot
// is kept and restored for the next ones.
class RealMachines : public Machines {
public:
    using UserFlash = std::shared_ptr<const std::vector<uint8_t>>;
    // `factoryUserFlash` is asked when the first fresh machine is made (the engine may still be making it when the
    // library starts); null: no fresh machine can be made (they must boot with it).
    RealMachines(std::string osPath, std::string wavePath, std::function<UserFlash()> factoryUserFlash);
    ~RealMachines() override;
    std::unique_ptr<Ops> fresh(const std::atomic<bool>& cancel, std::string& err) override;
    std::unique_ptr<Ops> clone(const std::vector<uint8_t>& machineState, const std::atomic<bool>& cancel, std::string& err) override;

private:
    std::unique_ptr<Machine> make(std::string& err);
    std::string osPath_, wavePath_;
    std::function<UserFlash()> factory_;
    std::unique_ptr<OsImage> os_;
    std::vector<uint8_t> booted_;                  // a fresh machine's state right after its boot
};

}  // namespace library
