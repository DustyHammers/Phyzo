// The user flash's factory contents (v0.8.3): what the OS's own factory restore writes, made on the user's machine
// from the user's own OS image and cached there (never shipped, never committed). No plugin framework here.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Machine;

namespace userflash {

// The OS's factory restore writes this many bytes to the user flash through the flash-write service (verified on the
// real OS).
constexpr uint64_t kFactoryRestoreBytes = 164352;
// The panel buttons it uses, by panel id (the raw codes of Engine::pressRaw).
constexpr uint8_t kRawSave = 0x0B, kRawCompare = 0x20, kRawYes = 0x0D;
// What the display shows on the way (the segment font shows S as 5).
constexpr const char* kConfirmText = "r5t?";
constexpr const char* kBurnText = "burn";
constexpr const char* kPresetText = "P 01";
// Longest waits (machine time): for the confirmation, and for the writing to end. They only bound a wait: every step
// goes on as soon as its condition holds, never after a fixed delay.
constexpr double kConfirmLimitMs = 2000, kWriteLimitMs = 8000;

// What the factory restore needs from a booted machine (MachinePanel; tests use a fake).
struct Panel {
    enum class Run { Time, Shown, Stopped };   // ran the whole time / `until` became true / the OS stopped or cancelled
    virtual ~Panel() = default;
    virtual void press(uint8_t raw, bool down) = 0;
    // Runs the machine for up to `ms` of machine time, stopping as soon as `until(display text)` is true (checked as
    // the machine runs, not only at the end). No `until`: runs the whole time.
    virtual Run run(double ms, const std::function<bool(const std::string&)>& until) = 0;
    virtual uint64_t serviceBytes() const = 0;   // bytes written through the flash-write service so far
    virtual std::string stopReason() const { return std::string(); }
};

// The OS's own factory restore, played on the panel as hands do (verified on the real OS): Save down, 200 ms,
// Compare down, 150 ms, Compare up, 200 ms, Save up; the display asks `r5t?` (waited for); +/Yes down for 110 ms, up;
// the display shows `burn` while the OS writes, then `P 01`. Finished (never after a fixed delay) once the display is
// back on `P 01` and the OS has written kFactoryRestoreBytes through the flash-write service; exactly that many, or it
// fails. On success the machine's user flash holds the factory user flash.
bool factoryRestore(Panel& p, std::string& err);

// The Panel of a real machine. `cancel` (optional) stops a run.
class MachinePanel : public Panel {
public:
    MachinePanel(Machine& m, const std::atomic<bool>* cancel) : m_(m), cancel_(cancel) {}
    void press(uint8_t raw, bool down) override;
    Run run(double ms, const std::function<bool(const std::string&)>& until) override;
    uint64_t serviceBytes() const override;
    std::string stopReason() const override { return reason_; }
private:
    Machine& m_;
    const std::atomic<bool>* cancel_;
    std::string reason_;
};

// The cache file for an OS image: <dir>/factory-user-flash-<osMd5>.dat, holding a header, the OS image's MD5, the
// user flash and its MD5. readCache accepts only a whole, undamaged file for that OS image.
std::string cachePath(const std::string& dir, const std::string& osMd5);
bool readCache(const std::string& path, const std::string& osMd5, std::vector<uint8_t>& out);
// Written to a temporary file first, then renamed over the old one (another instance never reads half a file).
bool writeCache(const std::string& path, const std::string& osMd5, const std::vector<uint8_t>& data);

}  // namespace userflash
