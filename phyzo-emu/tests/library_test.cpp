// Preset library test (no ROM data; v0.8.3 step 2): .syx splitting and classification, the index cache, the Library
// scan, safe file names, and the job service against a scripted stand-in for the OS's hidden machines (its own preset
// and bank messages, made up here: no OS data).
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
    int fresh = 0, clones = 0, receives = 0;
    Model running = factoryModel();                 // the running machine, for clones
    std::atomic<bool> blockReceive{false};          // receive() waits here until the job is cancelled
    std::atomic<bool> sawCancel{false};
    std::atomic<int> gateAt{-1};                    // dumpPreset number `gateAt` waits until `gateOpen`
    std::atomic<bool> gateOpen{true}, atGate{false};
    int dumps = 0;
};

class FakeOps : public Ops {
public:
    FakeOps(Shared& sh, Model m, const std::atomic<bool>& cancel) : sh_(sh), m_(std::move(m)), cancel_(cancel) {}
    bool receive(const std::vector<uint8_t>& msg, const Progress& progress) override {
        note();
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
    bool programChange(int p) override { note(); m_.current = p; m_.edit = m_.stored[size_t(p)]; m_.compare = false; return true; }
    bool readName(uint16_t n, std::string& name) override {
        note();
        if (n == 0) name = m_.edit;
        else if (n >= 0x40 && n < 0x80) name = m_.stored[size_t(n - 0x40)];
        else return fail("bad number");
        return true;
    }
    bool compareLit() override { note(); return m_.compare; }
    bool dumpPreset(std::vector<uint8_t>& msg) override {
        note();
        int k;
        { std::lock_guard<std::mutex> lk(sh_.mtx); k = sh_.dumps++; }
        if (k == sh_.gateAt) { sh_.atGate = true; while (!sh_.gateOpen && !cancel_) std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
        msg = presetMsg(m_.edit);
        return true;
    }
    bool dumpBank(int bank, std::vector<uint8_t>& msg, const Progress& progress) override {
        note();
        if (progress) { progress(51210, 102420); progress(102420, 102420); }
        msg = bankMsg(bank, std::vector<std::string>(m_.stored.begin() + bank * 32, m_.stored.begin() + bank * 32 + 32));
        return true;
    }
    std::string error() const override { return err_; }
private:
    void note() { std::lock_guard<std::mutex> lk(sh_.mtx); sh_.threads.push_back(std::this_thread::get_id()); }
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
        { std::lock_guard<std::mutex> lk(sh_.mtx); ++sh_.fresh; sh_.threads.push_back(std::this_thread::get_id()); }
        return std::make_unique<FakeOps>(sh_, factoryModel(), cancel);
    }
    std::unique_ptr<Ops> clone(const std::vector<uint8_t>& state, const std::atomic<bool>& cancel, std::string& err) override {
        std::lock_guard<std::mutex> lk(sh_.mtx);
        ++sh_.clones; sh_.threads.push_back(std::this_thread::get_id());
        if (state != Bytes{1, 2, 3}) { err = "bad state"; return nullptr; }
        return std::make_unique<FakeOps>(sh_, sh_.running, cancel);
    }
private:
    Shared& sh_;
};

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
Service::Config config(const fs::path& data) { return {data.string(), kOs, [] { return Bytes{1, 2, 3}; }}; }

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
        CHECK(has(n, "MAKING FACTORY BANKS (ONCE)  ABOUT A MINUTE"));
        // Names come from the OS: a bank's stored presets (0x40 + n), a single's edit buffer.
        const auto fb2 = s.entries(1);
        CHECK(fb2.size() == 32 && fb2[0].name == "Factory 33" && fb2[31].name == "Factory 64");
        const auto pads = s.entries(sourceOf(s, "My Pads"));
        CHECK(pads.size() == 32 && pads[4].name == "Mine 5");
        const auto singles = s.entries(sourceOf(s, "SINGLES"));
        CHECK(singles.size() == 2 && singles[0].name == "Lead" && singles[0].accepted);
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
    // SAVE: refused while the Compare LED is lit; otherwise the clone's dump of the current preset, never overwriting.
    {
        Service s(config(data), std::make_unique<FakeMachines>(sh));
        s.open();
        CHECK(waitIdle(s));
        notices(s);
        sh.running.compare = true;
        sh.running.edit = "Edited";
        size_t files = 0; for (const auto& e : fs::directory_iterator(lib / "Singles")) { (void)e; ++files; }
        s.save();
        CHECK(waitIdle(s));
        CHECK(has(notices(s), "UNSAVED EDITS  SAVE ON THE PANEL FIRST"));
        size_t after = 0; for (const auto& e : fs::directory_iterator(lib / "Singles")) { (void)e; ++after; }
        CHECK(after == files);
        sh.running.compare = false;
        sh.running.edit = "Beta/Arp";
        s.save();
        CHECK(waitIdle(s));
        CHECK(has(notices(s), "SAVED  Singles/Beta_Arp.syx"));
        Bytes b;
        CHECK(readFile((lib / "Singles" / "Beta_Arp.syx").string(), b) && b == presetMsg("Beta/Arp"));
        s.save();
        CHECK(waitIdle(s));
        CHECK(has(notices(s), "SAVED  Singles/Beta_Arp 2.syx"));
        CHECK(readFile((lib / "Singles" / "Beta_Arp.syx").string(), b) && b == presetMsg("Beta/Arp"));   // untouched
        const auto singles = s.entries(sourceOf(s, "SINGLES"));
        CHECK(singles.size() == 4 && singles[0].name == "Beta/Arp" && singles[0].ready);   // known at once, no re-read
        // EXPORT: a clone's bank dump, handed to the UI.
        sh.running.stored[32] = "Saved On Panel";
        s.exportBank(1);
        CHECK(waitIdle(s));
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
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_library_test";
    fs::remove_all(dir); fs::create_directories(dir);
    testSplit();
    testNames(dir);
    testIndexCache(dir);
    testScan(dir);
    testService(dir);
    fs::remove_all(dir);
    std::printf("library_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
