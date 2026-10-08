#include "library/service.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include "library/syx.h"
#include "os_image.h"

namespace fs = std::filesystem;

namespace library {

namespace {
std::mutex g_oneJob;                                // one hidden-machine job at a time in the process (all instances)
std::string upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
std::string factoryKey(int bank) { return "factory" + std::to_string(bank); }
}  // namespace

Service::Service(Config cfg, std::unique_ptr<Machines> machines) : cfg_(std::move(cfg)), machines_(std::move(machines)) {
    for (int k = 0; k < 2; ++k) {                    // the factory sources are always listed
        Source s; s.type = Source::Type::Factory; s.title = kFactoryTitles[k]; s.factoryBank = k;
        scan_.sources.push_back(s);
    }
    worker_ = std::thread(&Service::run, this);
}

Service::~Service() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        quit_ = true;
        queue_.clear();
        cancel_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

// ------------------------------------------------------------------ queue

void Service::enqueue(Job job, bool front) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (quit_) return;
        if (job.type == Job::Type::Refresh || job.type == Job::Type::Index)   // never the same work twice in the queue
            for (const Job& q : queue_)
                if (q.type == job.type && q.md5 == job.md5) return;
        if (front) queue_.push_front(std::move(job)); else queue_.push_back(std::move(job));
    }
    changed();
    cv_.notify_all();
}

void Service::open() { enqueue({Job::Type::Refresh, {}, {}, 0, {}}, false); }
void Service::save() { enqueue({Job::Type::Save, {}, {}, 0, {}}, true); }
void Service::exportBank(int bank) { enqueue({Job::Type::Export, {}, {}, bank, {}}, true); }
void Service::import(const std::vector<std::string>& paths) { enqueue({Job::Type::Import, {}, {}, 0, paths}, true); }

void Service::cancelAll() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        queue_.clear();
        if (running_) cancel_ = true;
        progress_.clear();
    }
    changed();
}

bool Service::busy() const { std::lock_guard<std::mutex> lk(mtx_); return running_ || !queue_.empty(); }

void Service::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            running_ = true;
            cancel_ = false;
        }
        std::unique_lock<std::mutex> one(g_oneJob, std::defer_lock);   // another instance's job first: wait (cancellable)
        while (!one.try_lock() && !quit_ && !cancel_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (one.owns_lock()) switch (job.type) {
        case Job::Type::Refresh: refresh(); break;
        case Job::Type::Index: index(job.path, job.md5); break;
        case Job::Type::Save: doSave(); break;
        case Job::Type::Export: doExport(job.bank); break;
        case Job::Type::Import: doImport(job.paths); break;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            running_ = false;
            if (cancel_) progress_.clear();
            cancel_ = false;
        }
        changed();
    }
}

void Service::notice(const std::string& text) {
    { std::lock_guard<std::mutex> lk(mtx_); notices_.push_back(text); }
    changed();
}

bool Service::takeNotice(std::string& text) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (notices_.empty()) return false;
    text = std::move(notices_.front());
    notices_.pop_front();
    return true;
}

bool Service::takeExport(int& bank, std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (exports_.empty()) return false;
    bank = exports_.front().first;
    bytes = std::move(exports_.front().second);
    exports_.pop_front();
    return true;
}

// ------------------------------------------------------------------ jobs (worker thread)

void Service::refresh() {
    const std::string lib = libraryDir();
    std::error_code ec;
    fs::create_directories(lib, ec);
    std::vector<int> missing;
    for (int k = 0; k < 2; ++k) if (!fs::exists(factoryFile(lib, k), ec)) missing.push_back(k);
    if (!missing.empty()) makeFactory(missing);
    if (cancel_) return;
    Scan s = scan(lib);
    // Indexes from the cache; the other files are read in jobs of their own (the UI's actions can run in between).
    std::map<std::string, Index> cached;
    std::vector<std::pair<std::string, std::string>> todo;   // path, md5
    for (const Source& src : s.sources)
        for (const int i : src.files) {
            const File& f = s.files[size_t(i)];
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (indexes_.count(f.md5)) continue;
            }
            Index idx;
            if (readIndex(indexPath(indexDir(), f.md5), cfg_.osMd5, idx)) cached[f.md5] = std::move(idx);
            else todo.emplace_back(f.path, f.md5);
        }
    std::vector<std::string> bad;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        scan_ = std::move(s);
        for (auto& kv : cached) indexes_[kv.first] = std::move(kv.second);
        for (const int i : scan_.bad)
            if (reportedBad_.insert(scan_.files[size_t(i)].md5).second) bad.push_back(scan_.files[size_t(i)].rel);
        for (const auto& [path, md5] : todo) {
            Progress& p = progress_[md5];
            for (const File& f : scan_.files) if (f.md5 == md5) p.total = f.presets;
        }
    }
    for (const std::string& rel : bad) notice(fs::path(rel).filename().string() + "  NO PRESET OR BANK IN THIS FILE");
    for (const auto& [path, md5] : todo) enqueue({Job::Type::Index, path, md5, 0, {}}, false);
    changed();
}

void Service::makeFactory(const std::vector<int>& banks) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const int k : banks) progress_[factoryKey(k)] = {0, size_t(kBankPresets)};
    }
    notice("MAKING FACTORY BANKS (ONCE)  ABOUT 2 MINUTES");
    auto clear = [&] { std::lock_guard<std::mutex> lk(mtx_); for (const int k : banks) progress_.erase(factoryKey(k)); };
    std::string err;
    std::unique_ptr<Ops> ops = machines_->fresh(cancel_, err);
    // The Dump list starts at the current preset: from P 64, bAn1 and bAn2 are the next two steps.
    if (!ops || !ops->programChange(63)) {
        if (!cancel_) notice("FACTORY BANKS NOT MADE  " + upper(ops ? ops->error() : err));
        clear();
        return;
    }
    for (const int k : banks) {
        std::vector<uint8_t> bank;
        const bool ok = ops->dumpBank(k, bank, [&](size_t done, size_t total) {
            { std::lock_guard<std::mutex> lk(mtx_); progress_[factoryKey(k)] = {total ? done * kBankPresets / total : 0, size_t(kBankPresets)}; }
            changed();
        });
        if (!ok || !writeReplace(factoryFile(libraryDir(), k), bank)) {
            if (!cancel_) notice("FACTORY BANK " + std::to_string(k + 1) + " NOT MADE  " + upper(ok ? std::string("CANNOT WRITE THE FILE") : ops->error()));
            break;
        }
    }
    clear();
}

void Service::index(const std::string& path, const std::string& md5) {
    auto done = [&](size_t n, size_t total) {
        { std::lock_guard<std::mutex> lk(mtx_); progress_[md5] = {n, total}; }
        changed();
    };
    auto drop = [&] { { std::lock_guard<std::mutex> lk(mtx_); progress_.erase(md5); } changed(); };
    std::vector<uint8_t> bytes;
    if (!readFile(path, bytes) || md5Hex(bytes.data(), bytes.size()) != md5) { drop(); return; }   // gone or changed: the next scan
    const Split sp = split(bytes);
    const std::string title = fs::path(path).stem().string();
    Index idx;
    idx.other = sp.other;
    const size_t total = sp.presetCount();
    size_t n = 0;
    done(0, total);
    std::string err;
    for (const int o : sp.order) {
        if (cancel_) { drop(); return; }
        // Each item gets a machine fresh from its boot: a refused preset must not find the last one in Compare.
        std::unique_ptr<Ops> ops = machines_->fresh(cancel_, err);
        if (!ops) {
            if (!cancel_) notice(title + "  NOT READ  " + upper(err));
            drop();
            return;
        }
        if (o >= 0) {                                // a single preset: does the OS take it, and what does it call it?
            Entry e;
            e.message = sp.presets[size_t(o)];
            std::string name;
            const bool ok = ops->receive(e.message, nullptr) && ops->readName(0, name);
            if (cancel_) { drop(); return; }
            e.accepted = ok && ops->compareLit() && !name.empty();
            e.name = e.accepted ? name : title;
            idx.entries.push_back(std::move(e));
            done(++n, total);
            continue;
        }
        const Split::Bank& b = sp.banks[size_t(-1 - o)];   // a bank: received, then each preset dumped from the panel
        bool ok = ops->receive(b.message, nullptr);
        for (int i = 0; ok && i < kBankPresets; ++i) {
            const int preset = b.number * kBankPresets + i;
            Entry e;
            ok = ops->programChange(preset) && ops->readName(uint16_t(0x40 + preset), e.name) && ops->dumpPreset(e.message);
            if (ok) { idx.entries.push_back(std::move(e)); done(++n, total); }
        }
        if (!ok) {
            if (!cancel_) notice(title + "  NOT READ  " + upper(ops->error()));
            drop();
            return;
        }
    }
    writeIndex(indexPath(indexDir(), md5), cfg_.osMd5, idx);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        indexes_[md5] = std::move(idx);
        progress_.erase(md5);
    }
    changed();
}

void Service::doSave() {
    const std::vector<uint8_t> state = cfg_.machineState ? cfg_.machineState() : std::vector<uint8_t>();
    std::string err;
    std::unique_ptr<Ops> ops = state.empty() ? nullptr : machines_->clone(state, cancel_, err);
    if (!ops) { if (!cancel_) notice("NOT SAVED  THE SYNTH IS NOT RUNNING"); return; }
    // The clone shows what the panel showed (e.g. `donE` right after a panel Dump): first the preset display.
    if (!ops->waitForPreset()) { if (!cancel_) notice("NOT SAVED  " + upper(ops->error())); return; }
    if (ops->compareLit()) { notice("UNSAVED EDITS  SAVE ON THE PANEL FIRST"); return; }
    std::string name;
    std::vector<uint8_t> message;
    if (!ops->readName(0, name) || !ops->dumpPreset(message)) {
        if (!cancel_) notice("NOT SAVED  " + upper(ops->error()));
        return;
    }
    const std::string dir = singlesDir(libraryDir());
    std::string path;
    for (int tries = 0; tries < 10 && path.empty(); ++tries) {   // another writer may take the name first: never overwrite
        const std::string p = uniquePath(dir, safeFileName(name));
        if (writeNew(p, message)) path = p;
    }
    if (path.empty()) { notice("NOT SAVED  CANNOT WRITE THE FILE"); return; }
    const std::string md5 = md5Hex(message.data(), message.size());
    Index idx;
    idx.entries.push_back({name, message, true});
    writeIndex(indexPath(indexDir(), md5), cfg_.osMd5, idx);
    { std::lock_guard<std::mutex> lk(mtx_); indexes_[md5] = std::move(idx); }
    notice("SAVED  Singles/" + fs::path(path).filename().string());
    refresh();
}

void Service::doExport(int bank) {
    const std::vector<uint8_t> state = cfg_.machineState ? cfg_.machineState() : std::vector<uint8_t>();
    std::string err;
    std::unique_ptr<Ops> ops = state.empty() ? nullptr : machines_->clone(state, cancel_, err);
    if (!ops) { if (!cancel_) notice("NOT EXPORTED  THE SYNTH IS NOT RUNNING"); return; }
    // The preset display first (a clone shows what the panel showed), then P 64: the Dump list starts at the current
    // preset, so bAn1 and bAn2 are the next two steps (from P 01 it would be 64).
    std::vector<uint8_t> message;
    if (!ops->waitForPreset() || !ops->programChange(63) || !ops->dumpBank(bank, message, nullptr)) {
        if (!cancel_) notice("NOT EXPORTED  " + upper(ops->error()));
        return;
    }
    { std::lock_guard<std::mutex> lk(mtx_); exports_.emplace_back(bank, std::move(message)); }
    changed();
}

void Service::doImport(const std::vector<std::string>& paths) {
    int copied = 0;
    std::string last;
    for (const std::string& p : paths) {
        std::vector<uint8_t> bytes;
        const std::string file = fs::path(p).filename().string();
        if (!readFile(p, bytes)) { notice(file + "  CANNOT BE READ"); continue; }
        const Split sp = split(bytes);
        if (sp.empty()) { notice(file + "  NO PRESET OR BANK IN THIS FILE"); continue; }
        const std::string dir = sp.single() ? singlesDir(libraryDir()) : libraryDir();
        const std::string stem = safeFileName(fs::path(p).stem().string());
        bool ok = false;
        for (int tries = 0; tries < 10 && !ok; ++tries) ok = writeNew(uniquePath(dir, stem), bytes);
        if (!ok) { notice(file + "  CANNOT BE COPIED"); continue; }
        ++copied;
        last = stem;
    }
    if (copied == 1) notice("IMPORTED  " + last);
    else if (copied > 1) notice("IMPORTED " + std::to_string(copied) + " FILES");
    if (copied) refresh();
}

// ------------------------------------------------------------------ the UI's view

const Index* Service::indexOf(const std::string& md5) const {
    const auto it = indexes_.find(md5);
    return it == indexes_.end() ? nullptr : &it->second;
}

std::vector<Service::SourceView> Service::sources() const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<SourceView> out;
    for (const Source& src : scan_.sources) {
        SourceView v;
        v.title = src.title;
        if (src.type == Source::Type::Factory) {
            const auto mk = progress_.find(factoryKey(src.factoryBank));
            if (mk != progress_.end()) { v.busy = true; v.done = mk->second.done; v.total = mk->second.total; out.push_back(v); continue; }
        }
        if (src.type == Source::Type::Singles) {
            size_t ready = 0;
            for (const int i : src.files) if (indexOf(scan_.files[size_t(i)].md5)) ++ready;
            v.count = src.files.size();
            if (ready < v.count) { v.busy = true; v.done = ready; v.total = v.count; }
            out.push_back(v);
            continue;
        }
        for (const int i : src.files) {
            const File& f = scan_.files[size_t(i)];
            if (const Index* idx = indexOf(f.md5)) v.count += idx->entries.size();
            else {
                const auto p = progress_.find(f.md5);
                v.busy = true;
                v.done += p != progress_.end() ? p->second.done : 0;
                v.total += p != progress_.end() && p->second.total ? p->second.total : f.presets;
            }
        }
        out.push_back(v);
    }
    return out;
}

std::vector<Service::EntryView> Service::entries(int source) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<EntryView> out;
    if (source < 0 || size_t(source) >= scan_.sources.size()) return out;
    const Source& src = scan_.sources[size_t(source)];
    for (const int i : src.files) {
        const File& f = scan_.files[size_t(i)];
        const Index* idx = indexOf(f.md5);
        if (src.type == Source::Type::Singles) {
            if (idx && !idx->entries.empty()) out.push_back({idx->entries[0].name, true, idx->entries[0].accepted});
            else out.push_back({f.title, false, true});
        } else if (idx) {
            for (const Entry& e : idx->entries) out.push_back({e.name, true, e.accepted});
        }
    }
    return out;
}

bool Service::message(int source, int entry, std::vector<uint8_t>& out) const {
    std::lock_guard<std::mutex> lk(mtx_);
    if (source < 0 || size_t(source) >= scan_.sources.size() || entry < 0) return false;
    const Source& src = scan_.sources[size_t(source)];
    int k = entry;
    for (const int i : src.files) {
        const Index* idx = indexOf(scan_.files[size_t(i)].md5);
        if (src.type == Source::Type::Singles) {
            if (k-- != 0) continue;
            if (!idx || idx->entries.empty() || !idx->entries[0].accepted) return false;
            out = idx->entries[0].message;
            return true;
        }
        if (!idx) continue;
        if (size_t(k) < idx->entries.size()) {
            if (!idx->entries[size_t(k)].accepted) return false;
            out = idx->entries[size_t(k)].message;
            return true;
        }
        k -= int(idx->entries.size());
    }
    return false;
}

}  // namespace library
