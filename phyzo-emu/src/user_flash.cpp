#include "user_flash.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include "machine.h"
#include "os_image.h"

namespace userflash {

bool factoryRestore(Panel& p, std::string& err) {
    auto stopped = [&](const char* when) {
        err = std::string("the OS stopped ") + when + (p.stopReason().empty() ? "" : " (" + p.stopReason() + ")");
        return false;
    };
    auto shows = [](const char* text) { return [text](const std::string& d) { return d == text; }; };
    const uint64_t before = p.serviceBytes();

    // Hold Save, press Compare: the OS asks for confirmation.
    p.press(kRawSave, true);
    if (p.run(200, nullptr) == Panel::Run::Stopped) return stopped("after Save");
    p.press(kRawCompare, true);
    if (p.run(150, nullptr) == Panel::Run::Stopped) return stopped("after Compare");
    p.press(kRawCompare, false);
    if (p.run(200, nullptr) == Panel::Run::Stopped) return stopped("after Compare");
    p.press(kRawSave, false);
    switch (p.run(kConfirmLimitMs, shows(kConfirmText))) {
    case Panel::Run::Stopped: return stopped("before the confirmation");
    case Panel::Run::Time: err = std::string("the display did not show ") + kConfirmText; return false;
    case Panel::Run::Shown: break;
    }

    // +/Yes: the OS writes the factory data (the display shows `burn`), then shows the preset again. Done once the
    // display is back and every byte is written. `burn` may show while +/Yes is held, and may be gone before the next
    // display update reaches the panel (the service takes no machine time), so it is watched for, not required.
    const auto written = [&] { return p.serviceBytes() - before; };
    bool burn = false;
    p.press(kRawYes, true);
    if (p.run(110, [&](const std::string& d) { burn |= d == kBurnText; return false; }) == Panel::Run::Stopped)
        return stopped("after +/Yes");
    p.press(kRawYes, false);
    const auto done = [&](const std::string& d) { burn |= d == kBurnText; return d == kPresetText && written() >= kFactoryRestoreBytes; };
    if (p.run(kWriteLimitMs, done) == Panel::Run::Stopped) return stopped(burn ? "while writing" : "after +/Yes");
    if (written() != kFactoryRestoreBytes) {
        err = "the factory restore wrote " + std::to_string(written()) + " bytes, not " + std::to_string(kFactoryRestoreBytes);
        return false;
    }
    if (!done(kPresetText)) { err = std::string("the display did not come back to ") + kPresetText; return false; }
    return true;
}

// ------------------------------------------------------------------ a real machine

void MachinePanel::press(uint8_t raw, bool down) {
    m_.panel.inject({uint8_t(down ? 0x81 : 0x80), raw}, m_.cycles(), std::string());
}

Panel::Run MachinePanel::run(double ms, const std::function<bool(const std::string&)>& until) {
    const uint64_t end = m_.cycles() + m_.cyclesFromMs(ms);
    while (m_.cycles() < end) {
        if (cancel_ && cancel_->load()) { reason_ = "cancelled"; return Run::Stopped; }
        const uint64_t step = std::min(end, m_.cycles() + m_.cyclesFromMs(20));
        const Machine::Stop st = until ? m_.runUntil(step, [&] { return until(m_.panel.text()); }) : m_.runUntil(step);
        m_.audio.clear(); m_.wet.clear();
        if (st == Machine::Stop::Trap) { reason_ = m_.trapReason; return Run::Stopped; }
        if (st == Machine::Stop::HardStall) { reason_ = "error loop"; return Run::Stopped; }
        if (st == Machine::Stop::Predicate) return Run::Shown;
    }
    return until && until(m_.panel.text()) ? Run::Shown : Run::Time;
}

uint64_t MachinePanel::serviceBytes() const { return m_.flashServiceBytes; }

// ------------------------------------------------------------------ cache file

namespace {
constexpr char kMagic[8] = {'P', 'H', 'Z', 'U', 'F', 'L', 'S', '1'};
constexpr size_t kMd5Len = 32;
}

std::string cachePath(const std::string& dir, const std::string& osMd5) {
    return dir + "/factory-user-flash-" + osMd5 + ".dat";
}

// Layout: magic (8), the OS image's MD5 (32 hex characters), size (u32, little-endian), the user flash, its MD5 (32).
bool readCache(const std::string& path, const std::string& osMd5, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> b;
    uint8_t buf[65536]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0 && b.size() < (size_t(1) << 20)) b.insert(b.end(), buf, buf + n);
    std::fclose(f);
    const size_t head = sizeof kMagic + kMd5Len + 4;
    if (b.size() < head || std::memcmp(b.data(), kMagic, sizeof kMagic) != 0) return false;
    if (osMd5.size() != kMd5Len || std::memcmp(&b[sizeof kMagic], osMd5.data(), kMd5Len) != 0) return false;
    const uint32_t size = uint32_t(b[head - 4]) | uint32_t(b[head - 3]) << 8 | uint32_t(b[head - 2]) << 16 | uint32_t(b[head - 1]) << 24;
    if (size != Machine::kUserFlashSize || b.size() != head + size + kMd5Len) return false;
    if (md5Hex(&b[head], size) != std::string(reinterpret_cast<const char*>(&b[head + size]), kMd5Len)) return false;
    out.assign(b.begin() + long(head), b.begin() + long(head + size));
    return true;
}

bool writeCache(const std::string& path, const std::string& osMd5, const std::vector<uint8_t>& data) {
    if (osMd5.size() != kMd5Len || data.size() != Machine::kUserFlashSize) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::vector<uint8_t> b(kMagic, kMagic + sizeof kMagic);
    b.insert(b.end(), osMd5.begin(), osMd5.end());
    const uint32_t size = uint32_t(data.size());
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(size >> (8 * i)));
    b.insert(b.end(), data.begin(), data.end());
    const std::string sum = md5Hex(data.data(), data.size());
    b.insert(b.end(), sum.begin(), sum.end());
    const std::string tmp = path + ".tmp" +
        std::to_string(std::hash<std::thread::id>()(std::this_thread::get_id()) ^
                       uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(b.data(), 1, b.size(), f) == b.size();
    if (std::fclose(f) != 0 || !ok || std::rename(tmp.c_str(), path.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
    return true;
}

}  // namespace userflash
