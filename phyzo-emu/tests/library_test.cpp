// Preset library test (no ROM data; v0.8.3 step 2): .syx splitting and classification, the index cache, the Library
// scan, safe file names, and the job service against a scripted stand-in for the OS's hidden machines (its own preset
// and bank messages, made up here: no OS data).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>
#include "library/library.h"
#include "library/service.h"
#include "library/syx.h"
#include "os_image.h"

namespace fs = std::filesystem;
using namespace library;
using Bytes = std::vector<uint8_t>;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// A made-up single-preset message: the OS's header, then the name as 16 7-bit bytes.
Bytes presetMsg(const std::string& name, uint8_t device = 0) {
    Bytes m = {0xF0, 0x0F, 0x11, 0x01, device, kPresetCommand, kPresetType, 0x7F, 0x7F};
    std::string n = name; n.resize(16, ' ');
    for (char c : n) m.push_back(uint8_t(c) & 0x7F);
    m.push_back(0xF7);
    return m;
}
std::string nameOf(const Bytes& m) { return m.size() >= 26 ? std::string(m.begin() + 9, m.begin() + 25) : std::string(); }
// A made-up bank message: header, bank, then 32 names.
Bytes bankMsg(int bank, const std::vector<std::string>& names) {
    Bytes m = {0xF0, 0x0F, 0x11, 0x01, 0x00, kBankCommand, uint8_t(bank), 0x00};
    for (int i = 0; i < kBankPresets; ++i) {
        std::string n = names[size_t(i)]; n.resize(16, ' ');
        for (char c : n) m.push_back(uint8_t(c) & 0x7F);
    }
    m.push_back(0xF7);
    return m;
}
std::string trimmed(std::string s) { while (!s.empty() && s.back() == ' ') s.pop_back(); return s; }
void writeFile(const fs::path& p, const Bytes& b) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), long(b.size()));
}
Bytes cat(std::initializer_list<Bytes> parts) { Bytes out; for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end()); return out; }

// ------------------------------------------------------------------ splitting

void testSplit() {
    const Bytes p1 = presetMsg("Alpha Pad"), p2 = presetMsg("Epsilon Bell", 0x10);
    std::vector<std::string> names(32);
    for (int i = 0; i < 32; ++i) names[size_t(i)] = "B2 " + std::to_string(i + 1);
    const Bytes b2 = bankMsg(1, names), b1 = bankMsg(0, names);
    const Bytes sound = {0xF0, 0x0F, 0x11, 0x01, 0x00, 0x02, 0x10, 0x00, 0x01, 0x22, 0xF7};   // another object type
    const Bytes foreign = {0xF0, 0x43, 0x10, 0x01, 0x02, 0xF7};
    int bank = -1;
    CHECK(classify(p1) == Kind::Preset && classify(p2) == Kind::Preset);
    CHECK(classify(b1, &bank) == Kind::Bank && bank == 0);
    CHECK(classify(b2, &bank) == Kind::Bank && bank == 1);           // byte 6 = 1: Bank 2
    CHECK(classify(sound) == Kind::Other && classify(foreign) == Kind::Other);
    // Channel messages with running status around the SysEx, a real-time byte inside one.
    Bytes withRt = p2; withRt.insert(withRt.begin() + 12, 0xF8);
    const Bytes file = cat({{0x90, 0x3C, 0x40, 0x3E, 0x40}, p1, {0xB0, 0x07, 0x64, 0x08, 0x10}, sound, b2, withRt, foreign, {0xC0, 0x05}});
    const Split s = split(file);
    CHECK(s.presets.size() == 2 && s.presets[0] == p1 && s.presets[1] == p2);   // the real-time byte is not kept
    CHECK(s.banks.size() == 1 && s.banks[0].number == 1 && s.banks[0].message == b2);
    CHECK(s.other == 2 && s.truncated == 0);
    CHECK((s.order == std::vector<int>{0, -1, 1}));
    CHECK(!s.single() && s.presetCount() == 2 + 32);
    // Truncated: a status byte inside the SysEx, a new F0 before F7, the end of the file.
    Bytes cut1(p1.begin(), p1.begin() + 12); cut1.push_back(0x90); cut1.push_back(0x3C); cut1.push_back(0x40);
    Bytes cut2(p1.begin(), p1.begin() + 12);
    const Split t = split(cat({cut1, cut2, p2, Bytes(p1.begin(), p1.end() - 1)}));
    CHECK(t.truncated == 3 && t.presets.size() == 1 && t.presets[0] == p2);
    CHECK(t.single());
    CHECK(split(cat({sound, foreign})).empty());
}

// ------------------------------------------------------------------ names, files, cache, scan

void testNames(const fs::path& dir) {
    CHECK(safeFileName("Beta Arp") == "Beta Arp");
    CHECK(safeFileName("  Bass/Lead: 2?  ") == "Bass_Lead_ 2_");
    CHECK(safeFileName("a\\b*c\"d<e>f|g") == "a_b_c_d_e_f_g");
    CHECK(safeFileName(std::string("x\x01y")) == "x_y");
    CHECK(safeFileName("..hidden.") == "hidden");
    CHECK(safeFileName("   ") == "Preset");
    const std::string d = (dir / "names").string();
    const Bytes a = {1, 2, 3}, b = {4, 5};
    const std::string p1 = uniquePath(d, "Pad");
    CHECK(p1 == d + "/Pad.syx");
    CHECK(writeNew(p1, a));
    CHECK(!writeNew(p1, b));                                  // never overwritten
    Bytes got;
    CHECK(readFile(p1, got) && got == a);
    const std::string p2 = uniquePath(d, "Pad");
    CHECK(p2 == d + "/Pad 2.syx");
    CHECK(writeNew(p2, b));
    CHECK(uniquePath(d, "Pad") == d + "/Pad 3.syx");
}

void testIndexCache(const fs::path& dir) {
    const std::string os = "0123456789abcdef0123456789abcdef";
    Index idx;
    idx.other = 2;
    idx.entries.push_back({"Alpha Pad", presetMsg("Alpha Pad"), true});
    idx.entries.push_back({"Odd", presetMsg("Odd"), false});
    const std::string md5 = "fedcba9876543210fedcba9876543210";
    const std::string path = indexPath((dir / "cache").string(), md5);
    CHECK(path == (dir / "cache").string() + "/" + md5 + ".idx");   // the key is the file's MD5
    CHECK(writeIndex(path, os, idx));
    Index back;
    CHECK(readIndex(path, os, back));
    CHECK(back.other == 2 && back.entries.size() == 2 && back.entries[0].name == "Alpha Pad" && back.entries[0].message == presetMsg("Alpha Pad"));
    CHECK(back.entries[1].accepted == false);
    CHECK(!readIndex(path, "ffffffffffffffffffffffffffffffff", back));   // made with another OS
    Bytes raw; CHECK(readFile(path, raw));
    raw[20] ^= 1; writeFile(path, raw);
    CHECK(!readIndex(path, os, back));                         // damaged
}

void testScan(const fs::path& dir) {
    const fs::path lib = dir / "scanlib";
    std::vector<std::string> names(32, "x");
    writeFile(lib / "Factory" / "Factory Bank 2.syx", bankMsg(1, names));
    writeFile(lib / "Zeta Set.syx", cat({presetMsg("A"), presetMsg("B")}));
    writeFile(lib / "my pads.syx", bankMsg(0, names));
    writeFile(lib / "Sub" / "Deep" / "Lone.syx", presetMsg("Lone"));
    writeFile(lib / "Singles" / "Arp.syx", presetMsg("Arp"));
    writeFile(lib / "Bad.syx", Bytes{0xF0, 0x43, 0x00, 0xF7});
    writeFile(lib / "notes.txt", presetMsg("not a syx"));
    writeFile(lib / "UPPER.SYX", presetMsg("Upper"));
    const Scan s = scan(lib.string());
    std::vector<std::string> titles;
    for (const Source& src : s.sources) titles.push_back(src.title);
    CHECK((titles == std::vector<std::string>{"FACTORY BANK 1", "FACTORY BANK 2", "my pads", "Zeta Set", "SINGLES"}));
    CHECK(s.sources[0].files.empty() && s.sources[1].files.size() == 1);   // Factory Bank 1 missing, still listed first
    const Source& singles = s.sources.back();
    CHECK(singles.type == Source::Type::Singles && singles.files.size() == 3);
    std::vector<std::string> rels;
    for (const int i : singles.files) rels.push_back(s.files[size_t(i)].rel);
    CHECK((rels == std::vector<std::string>{"Singles/Arp.syx", "Sub/Deep/Lone.syx", "UPPER.SYX"}));
    CHECK(s.bad.size() == 1 && s.files[size_t(s.bad[0])].title == "Bad");
    Bytes b; readFile((lib / "my pads.syx").string(), b);
    CHECK(s.files[size_t(s.sources[2].files[0])].md5 == md5Hex(b.data(), b.size()));
    CHECK(s.files[size_t(s.sources[2].files[0])].presets == 32);
}

// ------------------------------------------------------------------ the service, with a scripted OS

// The OS as the fakes see it: 64 stored presets, the edit buffer, the Compare LED.
struct Model {
    std::vector<std::string> stored = std::vector<std::string>(64);
    std::string edit;
    bool compare = false;
    int current = 0;
    bool done = false;                              // the display shows `donE` (after a Dump)
};
Model factoryModel() {
    Model m;
    for (int i = 0; i < 64; ++i) m.stored[size_t(i)] = "Factory " + std::to_string(i + 1);
    m.edit = m.stored[0];
    return m;
}

struct Shared {
    std::mutex mtx;
    std::vector<std::thread::id> threads;           // every call's thread
    std::vector<std::string> calls;                 // every call, in order (name and argument)
    int fresh = 0, clones = 0, receives = 0;
    Model running = factoryModel();                 // the running machine, for clones
    std::atomic<bool> blockReceive{false};          // receive() waits here until the job is cancelled
    std::atomic<bool> sawCancel{false};
    std::atomic<int> gateAt{-1};                    // dumpPreset number `gateAt` waits until `gateOpen`
    std::atomic<bool> gateOpen{true}, atGate{false};
    int dumps = 0;
    // The bank load: a clone's bank dump waits while holdBank (until cancelled), or fails; what the running machine
    // was sent (sendToMachine), and whether the OS takes it into its bank (as the real OS does) or ignores it.
    std::atomic<bool> holdBank{false}, atBank{false}, failBank{false};
    std::vector<Bytes> sentToMachine;
    bool applySent = true;
    int busyPolls = 0;                               // machineInputBusy answers true this many times
};

class FakeOps : public Ops {
public:
    FakeOps(Shared& sh, Model m, const std::atomic<bool>& cancel) : sh_(sh), m_(std::move(m)), cancel_(cancel) {}
    bool receive(const std::vector<uint8_t>& msg, const Progress& progress) override {
        note("receive");
        { std::lock_guard<std::mutex> lk(sh_.mtx); ++sh_.receives; }
        while (sh_.blockReceive) {
            if (cancel_) { sh_.sawCancel = true; return fail("cancelled"); }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (progress) progress(msg.size(), msg.size());
        int bank = 0;
        switch (classify(msg, &bank)) {
        case Kind::Preset:
            if (trimmed(nameOf(msg)) == "REFUSE") return true;   // the OS ignores it: no Compare, the old edit buffer
            m_.edit = trimmed(nameOf(msg)); m_.compare = true; return true;
        case Kind::Bank:
            for (int i = 0; i < kBankPresets; ++i)
                m_.stored[size_t(bank * kBankPresets + i)] = trimmed(std::string(msg.begin() + 8 + 16 * i, msg.begin() + 24 + 16 * i));
            return true;
        default: return true;
        }
    }
    bool programChange(int p) override { note("programChange " + std::to_string(p)); m_.current = p; m_.edit = m_.stored[size_t(p)]; m_.compare = false; return true; }
    bool readName(uint16_t n, std::string& name) override {
        note("readName " + std::to_string(n));
        if (n == 0) name = m_.edit;
        else if (n >= 0x40 && n < 0x80) name = m_.stored[size_t(n - 0x40)];
        else return fail("bad number");
        return true;
    }
    bool compareLit() override { note("compareLit"); return m_.compare; }
    bool waitForPreset() override { note("waitForPreset"); m_.done = false; return true; }
    int displayedPreset() override { return m_.current; }
    // As the real hidden machine: the Dumps wait for the preset display first, and leave `donE` behind.
    bool dumpPreset(std::vector<uint8_t>& msg) override {
        note("dumpPreset");
        if (m_.done) m_.done = false;
        int k;
        { std::lock_guard<std::mutex> lk(sh_.mtx); k = sh_.dumps++; }
        if (k == sh_.gateAt) { sh_.atGate = true; while (!sh_.gateOpen && !cancel_) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        msg = presetMsg(m_.edit);
        m_.done = true;
        return true;
    }
    bool dumpBank(int bank, std::vector<uint8_t>& msg, const Progress& progress) override {
        note("dumpBank " + std::to_string(bank));
        if (m_.done) m_.done = false;
        if (sh_.holdBank) { sh_.atBank = true; while (sh_.holdBank && !cancel_) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        if (cancel_) return fail("cancelled");
        if (sh_.failBank) return fail("no bank came");
        if (progress) { progress(51210, 102420); progress(102420, 102420); }
        msg = bankMsg(bank, std::vector<std::string>(m_.stored.begin() + bank * 32, m_.stored.begin() + bank * 32 + 32));
        m_.done = true;
        return true;
    }
    std::string error() const override { return err_; }
private:
    void note(const std::string& call) {
        std::lock_guard<std::mutex> lk(sh_.mtx);
        sh_.threads.push_back(std::this_thread::get_id());
        sh_.calls.push_back(call);
    }
    bool fail(const std::string& e) { err_ = e; return false; }
    Shared& sh_;
    Model m_;
    const std::atomic<bool>& cancel_;
    std::string err_;
};

class FakeMachines : public Machines {
public:
    explicit FakeMachines(Shared& sh) : sh_(sh) {}
    std::unique_ptr<Ops> fresh(const std::atomic<bool>& cancel, std::string&) override {
        { std::lock_guard<std::mutex> lk(sh_.mtx); ++sh_.fresh; sh_.threads.push_back(std::this_thread::get_id()); sh_.calls.push_back("fresh"); }
        return std::make_unique<FakeOps>(sh_, factoryModel(), cancel);
    }
    std::unique_ptr<Ops> clone(const std::vector<uint8_t>& state, const std::atomic<bool>& cancel, std::string& err) override {
        std::lock_guard<std::mutex> lk(sh_.mtx);
        ++sh_.clones; sh_.threads.push_back(std::this_thread::get_id()); sh_.calls.push_back("clone");
        if (state != Bytes{1, 2, 3}) { err = "bad state"; return nullptr; }
        return std::make_unique<FakeOps>(sh_, sh_.running, cancel);
    }
private:
    Shared& sh_;
};

// The calls made since the last machine of that kind ("clone" or "fresh") was made.
std::vector<std::string> callsSince(Shared& sh, const std::string& kind) {
    std::lock_guard<std::mutex> lk(sh.mtx);
    for (size_t i = sh.calls.size(); i-- > 0;)
        if (sh.calls[i] == kind) return std::vector<std::string>(sh.calls.begin() + long(i) + 1, sh.calls.end());
    return {};
}

bool waitIdle(Service& s, int ms = 10000) {
    for (int i = 0; i < ms && s.busy(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return !s.busy();
}
std::vector<std::string> notices(Service& s) { std::vector<std::string> v; std::string t; while (s.takeNotice(t)) v.push_back(t); return v; }
bool has(const std::vector<std::string>& v, const std::string& x) { return std::find(v.begin(), v.end(), x) != v.end(); }
int sourceOf(Service& s, const std::string& title) {
    const auto v = s.sources();
    for (size_t i = 0; i < v.size(); ++i) if (v[i].title == title) return int(i);
    return -1;
}
const std::string kOs = "0123456789abcdef0123456789abcdef";
Service::Config config(const fs::path& data) {
    Service::Config c;
    c.dataDir = data.string();
    c.osMd5 = kOs;
    c.machineState = [] { return Bytes{1, 2, 3}; };
    return c;
}
// With the bank load's hooks: the running machine's MIDI input (the OS takes a bank message into the bank its byte 6
// names, as the real OS does) and the Trash (a folder of the test's own).
Service::Config config(const fs::path& data, Shared& sh, const fs::path& trash) {
    Service::Config c = config(data);
    c.sendToMachine = [&sh](const Bytes& m) {
        std::lock_guard<std::mutex> lk(sh.mtx);
        sh.calls.push_back("send");
        sh.sentToMachine.push_back(m);
        int bank = -1;
        if (sh.applySent && classify(m, &bank) == Kind::Bank)
            for (int i = 0; i < kBankPresets; ++i)
                sh.running.stored[size_t(bank * kBankPresets + i)] = trimmed(std::string(m.begin() + 8 + 16 * i, m.begin() + 24 + 16 * i));
        return true;
    };
    c.machineInputBusy = [&sh] { std::lock_guard<std::mutex> lk(sh.mtx); return sh.busyPolls > 0 && sh.busyPolls-- > 0; };
    c.moveToTrash = [trash](const std::string& path) {
        std::error_code ec;
        fs::create_directories(trash, ec);
        fs::rename(path, trash / fs::u8path(path).filename(), ec);
        return !ec;
    };
    c.bankDeliveryMs = 2000;
    c.bankConfirmMs = 300;
    return c;
}

void testService(const fs::path& dir) {
    const fs::path data = dir / "data";
    const fs::path lib = data / "Library";
    std::vector<std::string> mine(32);
    for (int i = 0; i < 32; ++i) mine[size_t(i)] = "Mine " + std::to_string(i + 1);
    writeFile(lib / "My Pads.syx", bankMsg(1, mine));               // a Bank 2 file
    writeFile(lib / "Singles" / "Lead.syx", presetMsg("Lead"));
    writeFile(lib / "Sub" / "Refused.syx", presetMsg("REFUSE"));
    writeFile(lib / "Two.syx", cat({presetMsg("One A"), {0x90, 0x40, 0x40}, presetMsg("One B")}));
    writeFile(lib / "Bad.syx", Bytes{0xF0, 0x0F, 0x11, 0x01, 0x00, 0x02, 0x10, 0x00, 0xF7});
    Shared sh;
    {
        Service s(config(data), std::make_unique<FakeMachines>(sh));
        const auto initial = s.sources();
        CHECK(initial.size() == 2 && initial[0].title == "FACTORY BANK 1");   // listed before anything is read
        s.open();
        CHECK(waitIdle(s));
        // The factory bank files were made (from a machine booted the v0.8.3 way), then read like any bank file.
        Bytes f1, f2;
        CHECK(readFile(factoryFile(lib.string(), 0), f1) && readFile(factoryFile(lib.string(), 1), f2));
        int bank = -1;
        CHECK(classify(f1, &bank) == Kind::Bank && bank == 0 && classify(f2, &bank) == Kind::Bank && bank == 1);
        const auto v = s.sources();
        std::vector<std::string> titles;
        for (const auto& x : v) titles.push_back(x.title + ":" + std::to_string(x.count) + (x.busy ? "*" : ""));
        CHECK((titles == std::vector<std::string>{"FACTORY BANK 1:32", "FACTORY BANK 2:32", "My Pads:32", "Two:2", "SINGLES:2"}));
        const auto n = notices(s);
        CHECK(has(n, "Bad.syx  NO PRESET OR BANK IN THIS FILE"));
        CHECK(has(n, "MAKING FACTORY BANKS (ONCE)  ABOUT 2 MINUTES"));
        {   // the factory banks: from P 64, bAn1 then bAn2, each Dump waiting for the preset display (after `donE`)
            std::lock_guard<std::mutex> lk(sh.mtx);
            const auto first = std::find(sh.calls.begin(), sh.calls.end(), "fresh");
            CHECK(first != sh.calls.end() && sh.calls.end() - first >= 4);
            if (sh.calls.end() - first >= 4)
                CHECK((std::vector<std::string>(first + 1, first + 4) == std::vector<std::string>{"programChange 63", "dumpBank 0", "dumpBank 1"}));
        }
        // Names come from the OS: a bank's stored presets (0x40 + n), a single's edit buffer.
        const auto fb2 = s.entries(1);
        CHECK(fb2.size() == 32 && fb2[0].name == "Factory 33" && fb2[31].name == "Factory 64");
        const auto pads = s.entries(sourceOf(s, "My Pads"));
        CHECK(pads.size() == 32 && pads[4].name == "Mine 5");
        const auto singles = s.entries(sourceOf(s, "SINGLES"));
        CHECK(singles.size() == 2 && singles[0].name == "Lead" && singles[0].accepted);   // by file name
        CHECK(singles[1].name == "Refused" && !singles[1].accepted);   // listed as not accepted, under its file's name
        const auto two = s.entries(sourceOf(s, "Two"));
        CHECK(two.size() == 2 && two[1].name == "One B");
        // The message a click sends: exactly the stored bytes (a single: the file's own; a bank: the panel's dump).
        Bytes m;
        CHECK(s.message(sourceOf(s, "Two"), 1, m) && m == presetMsg("One B"));
        CHECK(s.message(sourceOf(s, "My Pads"), 4, m) && m == presetMsg("Mine 5"));
        CHECK(!s.message(sourceOf(s, "SINGLES"), 1, m));           // not accepted: nothing to send
        CHECK(!s.message(sourceOf(s, "SINGLES"), 2, m) && !s.message(99, 0, m));
        // Every hidden machine call ran on the service's worker thread, never this (UI) thread.
        std::lock_guard<std::mutex> lk(sh.mtx);
        CHECK(!sh.threads.empty());
        bool onWorker = true;
        for (const auto& t : sh.threads) onWorker &= t == s.workerId() && t != std::this_thread::get_id();
        CHECK(onWorker);
    }
    // The index cache: a second instance reads nothing again.
    const int freshBefore = sh.fresh;
    {
        Service s(config(data), std::make_unique<FakeMachines>(sh));
        s.open();
        CHECK(waitIdle(s));
        CHECK(sh.fresh == freshBefore);
        CHECK(s.entries(sourceOf(s, "My Pads")).size() == 32);
        CHECK(has(notices(s), "Bad.syx  NO PRESET OR BANK IN THIS FILE"));   // told once per instance
        s.open();
        CHECK(waitIdle(s));
        CHECK(!has(notices(s), "Bad.syx  NO PRESET OR BANK IN THIS FILE"));
        // A changed file is read again (its MD5 is the key).
        mine[4] = "Changed 5";
        writeFile(lib / "My Pads.syx", bankMsg(1, mine));
        s.open();
        CHECK(waitIdle(s));
        CHECK(sh.fresh == freshBefore + 1);
        CHECK(s.entries(sourceOf(s, "My Pads"))[4].name == "Changed 5");
        Bytes b; readFile((lib / "My Pads.syx").string(), b);
        CHECK(fs::exists(indexPath((data / "cache" / "library").string(), md5Hex(b.data(), b.size()))));
    }
    // Progress while a bank is read: in its count column (done/total).
    {
        writeFile(lib / "Live Set.syx", bankMsg(0, mine));
        Service s(config(data), std::make_unique<FakeMachines>(sh));
        { std::lock_guard<std::mutex> lk(sh.mtx); sh.dumps = 0; }
        sh.gateOpen = false; sh.atGate = false; sh.gateAt = 12;
        s.open();
        for (int i = 0; i < 5000 && !sh.atGate; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        const auto v = s.sources();
        const int live = sourceOf(s, "Live Set");
        CHECK(live >= 0 && v[size_t(live)].busy && v[size_t(live)].done == 12 && v[size_t(live)].total == 32);
        CHECK(s.entries(sourceOf(s, "My Pads")).size() == 32);    // the other sources still work meanwhile
        sh.gateOpen = true;
        CHECK(waitIdle(s));
        CHECK(!s.sources()[size_t(sourceOf(s, "Live Set"))].busy && s.sources()[size_t(sourceOf(s, "Live Set"))].count == 32);
        sh.gateAt = -1;
    }
    // Save with a name (v0.9.0): the draft (a clone: Compare LED, preset number, OS name, dump), then the file.
    {
        Service s(config(data), std::make_unique<FakeMachines>(sh));
        s.open();
        CHECK(waitIdle(s));
        notices(s);
        auto draft = [&] {
            s.prepareSave();
            CHECK(waitIdle(s));
            Service::SaveDraft d;
            CHECK(s.takeSaveDraft(d));
            Service::SaveDraft extra;
            CHECK(!s.takeSaveDraft(extra));                  // one draft per request
            return d;
        };
        auto singlesCount = [&] { size_t n = 0; for (const auto& e : fs::directory_iterator(lib / "Singles")) { (void)e; ++n; } return n; };
        sh.running.compare = true;
        sh.running.edit = "Edited";
        const size_t files = singlesCount();
        Service::SaveDraft d = draft();
        CHECK(d.compare && d.message.empty() && d.error.empty());       // unsaved edits: "save on the panel first"
        CHECK((callsSince(sh, "clone") == std::vector<std::string>{"waitForPreset", "compareLit"}));
        CHECK(singlesCount() == files);
        sh.running.compare = false;
        sh.running.edit = "  Beta/Arp ";
        sh.running.current = 4;
        sh.running.done = true;                              // right after a panel Dump: the clone shows `donE`
        d = draft();
        CHECK(!d.compare && d.preset == 4 && d.osName == "  Beta/Arp " && d.message == presetMsg("  Beta/Arp "));
        CHECK((callsSince(sh, "clone") == std::vector<std::string>{"waitForPreset", "compareLit", "readName 0", "dumpPreset"}));
        s.saveAs(d, "My: Lead?");                            // typed: reserved characters become '_'
        CHECK(waitIdle(s));
        CHECK(has(notices(s), "SAVED  Singles/My_ Lead_.syx"));
        Bytes b;
        CHECK(readFile((lib / "Singles" / "My_ Lead_.syx").string(), b) && b == d.message);   // the dump's bytes, unchanged
        s.saveAs(d, "   ");                                  // empty: the OS's name (made safe)
        CHECK(waitIdle(s));
        CHECK(has(notices(s), "SAVED  Singles/Beta_Arp.syx"));
        s.saveAs(d, "");
        CHECK(waitIdle(s));
        CHECK(has(notices(s), "SAVED  Singles/Beta_Arp 2.syx"));   // never overwriting
        CHECK(readFile((lib / "Singles" / "Beta_Arp.syx").string(), b) && b == d.message);
        CHECK(readFile((lib / "Singles" / "Beta_Arp 2.syx").string(), b) && b == d.message);
        const auto singles = s.entries(sourceOf(s, "SINGLES"));
        std::vector<std::string> names;
        for (const auto& e : singles) names.push_back(e.name + (e.ready ? "" : "*"));
        CHECK((names == std::vector<std::string>{"Beta_Arp 2", "Beta_Arp", "Lead", "My_ Lead_", "Refused"}));   // by file name, no re-read
        CHECK(singles.size() == 5 && singles[3].path == (lib / "Singles" / "My_ Lead_.syx").string());
        Service::SaveDraft none;
        s.saveAs(none, "x");                                 // nothing dumped: nothing written
        CHECK(waitIdle(s));
        CHECK(has(notices(s), "NOT SAVED  NOTHING TO SAVE") && singlesCount() == files + 3);
        sh.running.current = 0;
        // EXPORT: a clone's bank dump, handed to the UI.
        sh.running.stored[32] = "Saved On Panel";
        s.exportBank(1);
        CHECK(waitIdle(s));
        // The preset display first, then P 64 (bAn1 and bAn2 are the next steps of the Dump list), then the bank.
        CHECK((callsSince(sh, "clone") == std::vector<std::string>{"waitForPreset", "programChange 63", "dumpBank 1"}));
        int bank = -1; Bytes ex;
        CHECK(s.takeExport(bank, ex) && bank == 1 && ex.size() > 8 && ex[6] == 1);
        CHECK(trimmed(std::string(ex.begin() + 8, ex.begin() + 24)) == "Saved On Panel");
        CHECK(!s.takeExport(bank, ex));
        // IMPORT: copied into Library/ (a single into Singles/), never overwriting, then read.
        const fs::path out = dir / "outside";
        writeFile(out / "Lead.syx", presetMsg("Imported Lead"));
        writeFile(out / "Big.syx", bankMsg(0, mine));
        writeFile(out / "Junk.syx", Bytes{0x90, 0x40, 0x40});
        s.import({(out / "Lead.syx").string(), (out / "Big.syx").string(), (out / "Junk.syx").string()});
        CHECK(waitIdle(s));
        const auto n = notices(s);
        CHECK(has(n, "IMPORTED 2 FILES") && has(n, "Junk.syx  NO PRESET OR BANK IN THIS FILE"));
        CHECK(readFile((lib / "Singles" / "Lead.syx").string(), b) && b == presetMsg("Lead"));   // the old one stays
        CHECK(readFile((lib / "Singles" / "Lead 2.syx").string(), b) && b == presetMsg("Imported Lead"));
        CHECK(fs::exists(lib / "Big.syx") && !fs::exists(lib / "Junk.syx"));
        CHECK(sourceOf(s, "Big") >= 0 && s.entries(sourceOf(s, "Big")).size() == 32);
    }
    // Jobs are queued and cancelled: closing stops the running job at once and drops the queue.
    {
        writeFile(lib / "Slow.syx", bankMsg(0, std::vector<std::string>(32, "Slow")));
        writeFile(lib / "Slower.syx", bankMsg(1, std::vector<std::string>(32, "Slower")));
        sh.blockReceive = true; sh.sawCancel = false;
        const int receives = sh.receives;
        auto s = std::make_unique<Service>(config(data), std::make_unique<FakeMachines>(sh));
        s->open();
        for (int i = 0; i < 5000 && sh.receives == receives; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        CHECK(s->busy());
        const auto t0 = std::chrono::steady_clock::now();
        s.reset();                                             // the plugin closes
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(sh.sawCancel && ms < 1000);
        CHECK(sh.receives == receives + 1);                    // the queued file never started
        // cancelAll: the same without closing; the service keeps working afterwards.
        sh.sawCancel = false;
        Service t(config(data), std::make_unique<FakeMachines>(sh));
        t.open();
        for (int i = 0; i < 5000 && !sh.sawCancel && sh.receives == receives + 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        t.cancelAll();
        CHECK(waitIdle(t, 2000));
        CHECK(sh.sawCancel);
        sh.blockReceive = false;
        t.open();
        CHECK(waitIdle(t));
        CHECK(t.entries(sourceOf(t, "Slow")).size() == 32 && t.entries(sourceOf(t, "Slower")).size() == 32);
    }
}
// The plugin window's view (v0.9.0): groups and names, Rename and Delete, the bank load.
void testWindow(const fs::path& dir) {
    const fs::path data = dir / "window";
    const fs::path lib = data / "Library", trash = dir / "Trash";
    std::vector<std::string> pads(32), strings(32);
    for (int i = 0; i < 32; ++i) { pads[size_t(i)] = "Pad " + std::to_string(i + 1); strings[size_t(i)] = "Str " + std::to_string(i + 1); }
    writeFile(lib / "zeta.syx", bankMsg(0, pads));
    writeFile(lib / "Alpha.syx", bankMsg(1, strings));
    writeFile(lib / "Live" / "Set A.syx", cat({presetMsg("A1"), presetMsg("A2")}));   // two presets: not a bank
    writeFile(lib / "Singles" / "Solo.syx", presetMsg("Solo"));
    writeFile(lib / "Backups" / "Bank 1 2026-01-02 0304.syx", bankMsg(0, pads));
    Shared sh;
    Service s(config(data, sh, trash), std::make_unique<FakeMachines>(sh));
    s.open();
    CHECK(waitIdle(s));
    notices(s);
    // Groups in order: FACTORY, BANKS (by name, case ignored; a subfolder as folder/name), SINGLES, BACKUPS.
    using G = Source::Group;
    std::vector<std::string> rows;
    for (const auto& v : s.sources())
        rows.push_back(std::to_string(int(v.group)) + " " + v.label + " " + std::to_string(v.count) + (v.bank ? " bank" : ""));
    CHECK((rows == std::vector<std::string>{"0 Factory Bank 1 32 bank", "0 Factory Bank 2 32 bank", "1 Alpha 32 bank", "1 Live/Set A 2",
                                            "1 zeta 32 bank", "2 Singles 1", "3 Bank 1 2026-01-02 0304 32 bank"}));
    CHECK(s.sources()[2].path == (lib / "Alpha.syx").string() && s.sources()[5].path == (lib / "Singles").string());
    CHECK(s.sources()[0].group == G::Factory && s.isFactoryFile(s.sources()[0].path) && !s.isFactoryFile(s.sources()[2].path));
    CHECK(backupName(1, 0).rfind("Bank 2 19", 0) == 0 && backupName(1, 0).size() == std::string("Bank 2 1970-01-01 0000").size());

    // Rename: the file name only, made safe; an existing name refused; factory files never; the index follows by MD5.
    const int freshBefore = sh.fresh;
    CHECK(s.rename((lib / "zeta.syx").string(), "Alpha") == "Not renamed: Alpha.syx already exists");
    CHECK(fs::exists(lib / "zeta.syx"));
    CHECK(s.rename(s.sources()[0].path, "Mine") == "Not renamed: not a library file");
    CHECK(s.rename((lib / "zeta.syx").string(), "  ") == "" && s.rename((lib / "zeta.syx").string(), "zeta") == "");
    CHECK(s.rename((lib / "zeta.syx").string(), "Big/Pads") == "Renamed to Big_Pads.syx");
    CHECK(waitIdle(s));
    CHECK(!fs::exists(lib / "zeta.syx") && fs::exists(lib / "Big_Pads.syx"));
    CHECK(s.rename((lib / "Live" / "Set A.syx").string(), "Set B") == "Renamed to Set B.syx" && fs::exists(lib / "Live" / "Set B.syx"));
    CHECK(waitIdle(s));
    const int big = sourceOf(s, "Big_Pads");
    CHECK(big >= 0 && s.entries(big).size() == 32 && s.entries(big)[3].name == "Pad 4");
    CHECK(sh.fresh == freshBefore);                                      // nothing read again
    // Delete: to the Trash (the hook), never a factory file.
    CHECK(s.remove(s.sources()[1].path) == "Not deleted: not a library file" && fs::exists(s.sources()[1].path));
    CHECK(s.remove((lib / "Live" / "Set B.syx").string()) == "Deleted Set B (in the Trash)");
    CHECK(waitIdle(s));
    CHECK(!fs::exists(lib / "Live" / "Set B.syx") && fs::exists(trash / "Set B.syx"));
    CHECK(sourceOf(s, "Set B") < 0);
    {
        Service t(config(data), std::make_unique<FakeMachines>(sh));   // no Trash hook: refused, the file stays
        CHECK(t.remove((lib / "Alpha.syx").string()).rfind("Not deleted", 0) == 0 && fs::exists(lib / "Alpha.syx"));
    }

    // The bank load. Alpha is a Bank 2 file; load it into Bank 1: backup first, then the message with byte 6 = 0 and
    // nothing else changed, then confirmed by the 32 stored names.
    auto backups = [&] { size_t n = 0; std::error_code ec; for (const auto& e : fs::directory_iterator(lib / "Backups", ec)) { (void)e; ++n; } return n; };
    Bytes alpha; CHECK(readFile((lib / "Alpha.syx").string(), alpha));
    CHECK(!s.loadBank((lib / "Singles" / "Solo.syx").string(), 0));       // not one bank
    CHECK(has(notices(s), "Solo has no bank"));
    {
        std::lock_guard<std::mutex> lk(sh.mtx);
        sh.calls.clear();
        sh.busyPolls = 5;                                                // on its way for a while
    }
    const size_t before = backups();
    sh.running.stored[0] = "Old 1";
    CHECK(s.loadBank((lib / "Alpha.syx").string(), 0));
    CHECK(!s.loadBank((lib / "Big_Pads.syx").string(), 1));             // one bank job at a time
    CHECK(s.bankJob().active && s.bankJob().bank == 0 && s.bankJob().file == "Alpha");
    CHECK(waitIdle(s));
    CHECK(!s.bankJob().active);
    auto n = notices(s);
    CHECK(has(n, "LOADED Alpha INTO BANK 1"));
    int lb = -1; std::string lf;
    CHECK(s.takeLoaded(lb, lf) && lb == 0 && lf == "Alpha" && !s.takeLoaded(lb, lf));
    {
        std::lock_guard<std::mutex> lk(sh.mtx);
        CHECK(sh.sentToMachine.size() == 1);
        if (sh.sentToMachine.size() == 1) {
            const Bytes& m = sh.sentToMachine[0];
            CHECK(m.size() == alpha.size() && m[6] == 0 && alpha[6] == 1);
            Bytes same = m; same[6] = alpha[6];
            CHECK(same == alpha);                                        // byte 6 only
        }
        // The backup (a clone's dump of Bank 1) before anything was sent.
        const auto dump = std::find(sh.calls.begin(), sh.calls.end(), "dumpBank 0");
        const auto send = std::find(sh.calls.begin(), sh.calls.end(), "send");
        CHECK(dump != sh.calls.end() && send != sh.calls.end() && dump < send);
        CHECK(sh.running.stored[0] == "Str 1" && sh.running.stored[31] == "Str 32" && sh.running.stored[32] == "Factory 33");
    }
    CHECK(backups() == before + 1);
    bool found = false;
    for (const auto& v : s.sources()) found |= v.group == G::Backups && v.label.rfind("Bank 1 ", 0) == 0 && v.label != "Bank 1 2026-01-02 0304";
    CHECK(found);                                                         // the new backup is listed
    // The backup holds the bank as it was (its first name "Old 1").
    for (const auto& e : fs::directory_iterator(lib / "Backups")) {
        Bytes b; readFile(e.path().string(), b);
        if (e.path().filename().string() != "Bank 1 2026-01-02 0304.syx") CHECK(trimmed(std::string(b.begin() + 8, b.begin() + 24)) == "Old 1");
    }
    // A failed backup: nothing is sent.
    sh.failBank = true;
    { std::lock_guard<std::mutex> lk(sh.mtx); sh.sentToMachine.clear(); }
    CHECK(s.loadBank((lib / "Big_Pads.syx").string(), 1));
    CHECK(waitIdle(s));
    n = notices(s);
    CHECK(has(n, "NOT LOADED  BANK 2 COULD NOT BE BACKED UP  NO BANK CAME"));
    CHECK(sh.sentToMachine.empty() && backups() == before + 1);
    sh.failBank = false;
    // Cancel during the backup: nothing is sent, no backup written.
    sh.holdBank = true; sh.atBank = false;
    CHECK(s.loadBank((lib / "Big_Pads.syx").string(), 1));
    for (int i = 0; i < 5000 && !sh.atBank; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(s.bankJob().active && s.bankJob().phase == Service::BankJob::Phase::Backup);
    s.cancelBankBackup();
    CHECK(waitIdle(s));
    sh.holdBank = false;
    CHECK(has(notices(s), "NOT LOADED  CANCELLED"));
    CHECK(sh.sentToMachine.empty() && backups() == before + 1 && !s.bankJob().active);
    // The OS does not take it: after the confirmation time, "didn't confirm" (the backup stays).
    sh.applySent = false;
    CHECK(s.loadBank((lib / "Big_Pads.syx").string(), 1));
    for (int i = 0; i < 5000 && s.bankJob().phase != Service::BankJob::Phase::Confirm; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    s.cancelBankBackup();                                               // too late: the load was sent
    CHECK(waitIdle(s));
    n = notices(s);
    CHECK(has(n, "Bank 2 didn't confirm the load"));
    CHECK(sh.sentToMachine.size() == 1 && sh.sentToMachine[0][6] == 1 && backups() == before + 2);
    CHECK(!s.takeLoaded(lb, lf));
    sh.applySent = true;
}
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_library_test";
    fs::remove_all(dir); fs::create_directories(dir);
    testSplit();
    testNames(dir);
    testIndexCache(dir);
    testScan(dir);
    testService(dir);
    testWindow(dir);
    fs::remove_all(dir);
    std::printf("library_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
