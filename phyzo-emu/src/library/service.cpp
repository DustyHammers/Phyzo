#include "library/service.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
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
        Source s; s.type = Source::Type::Factory; s.group = Source::Group::Factory; s.title = kFactoryTitles[k]; s.factoryBank = k;
        s.label = fs::path(factoryFile(libraryDir(), k)).stem().string();
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

void Service::open() { Job j; j.type = Job::Type::Refresh; enqueue(std::move(j), false); }
void Service::prepareSave() { Job j; j.type = Job::Type::PrepareSave; enqueue(std::move(j), true); }
void Service::saveAs(const SaveDraft& draft, const std::string& name) {
    Job j; j.type = Job::Type::SaveAs;
    j.draft = draft; j.name = name;
    enqueue(std::move(j), true);
}
void Service::exportBank(int bank) { Job j; j.type = Job::Type::Export; j.bank = bank; enqueue(std::move(j), true); }
void Service::import(const std::vector<std::string>& paths) { Job j; j.type = Job::Type::Import; j.paths = paths; enqueue(std::move(j), true); }

void Service::cancelAll() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const Job& j : queue_) if (j.type == Job::Type::LoadBank) bankJob_ = BankJob();
        queue_.clear();
        if (running_) cancel_ = true;
        progress_.clear();
    }
    changed();
}

bool Service::takeSaveDraft(SaveDraft& out) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (drafts_.empty()) return false;
    out = std::move(drafts_.front());
    drafts_.pop_front();
    return true;
}

bool Service::oneBank(const std::vector<uint8_t>& file, std::vector<uint8_t>* message) {
    const Split sp = split(file);
    if (sp.banks.size() != 1 || !sp.presets.empty()) return false;
    if (message) *message = sp.banks[0].message;
    return true;
}

bool Service::loadBank(const std::string& path, int bank) {
    if (bank < 0 || bank > 1) return false;
    std::vector<uint8_t> bytes;
    const std::string title = fs::u8path(path).stem().u8string();
    if (!readFile(path, bytes) || !oneBank(bytes)) { notice(title + " has no bank"); return false; }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (bankJob_.active || quit_) return false;
        bankJob_ = BankJob();
        bankJob_.active = true;
        bankJob_.bank = bank;
        bankJob_.file = title;
    }
    Job j; j.type = Job::Type::LoadBank;
    j.path = path; j.bank = bank;
    enqueue(std::move(j), true);
    return true;
}

Service::BankJob Service::bankJob() const { std::lock_guard<std::mutex> lk(mtx_); return bankJob_; }

void Service::setPhase(BankJob::Phase p) {
    { std::lock_guard<std::mutex> lk(mtx_); bankJob_.phase = p; }
    changed();
}

void Service::cancelBankBackup() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!bankJob_.active || (bankJob_.phase != BankJob::Phase::Queued && bankJob_.phase != BankJob::Phase::Backup)) return;
        const auto queued = std::find_if(queue_.begin(), queue_.end(), [](const Job& j) { return j.type == Job::Type::LoadBank; });
        if (queued != queue_.end()) { queue_.erase(queued); bankJob_ = BankJob(); }
        else cancel_ = true;                         // the running job stops at its next step; nothing is sent
    }
    changed();
}

bool Service::takeLoaded(int& bank, std::string& file) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (loaded_.empty()) return false;
    bank = loaded_.front().first;
    file = std::move(loaded_.front().second);
    loaded_.pop_front();
    return true;
}

// ------------------------------------------------------------------ Rename and Delete (UI thread)

bool Service::isFactoryFile(const std::string& path) const {
    const fs::path p = fs::u8path(path).lexically_normal();
    for (int k = 0; k < 2; ++k)
        if (p == fs::u8path(factoryFile(libraryDir(), k)).lexically_normal()) return true;
    return false;
}

namespace {
// A file inside the Library folder (never the folder itself or anything outside it).
bool insideLibrary(const std::string& libraryDir, const fs::path& p) {
    const std::string rel = p.lexically_normal().lexically_relative(fs::u8path(libraryDir).lexically_normal()).generic_u8string();
    return !rel.empty() && rel != "." && rel.rfind("..", 0) != 0;
}
std::string trimmedName(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t"), e = s.find_last_not_of(" \t");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
}  // namespace

std::string Service::rename(const std::string& path, const std::string& newName) {
    const fs::path from = fs::u8path(path);
    std::error_code ec;
    if (isFactoryFile(path) || !insideLibrary(libraryDir(), from) || !fs::is_regular_file(from, ec)) return "Not renamed: not a library file";
    const std::string typed = trimmedName(newName);
    if (typed.empty() || typed == from.stem().u8string()) return {};     // nothing to change
    const std::string stem = safeFileName(typed);
    const fs::path to = from.parent_path() / fs::u8path(stem + from.extension().u8string());
    // The same file under another case (a case-insensitive disk): allowed; any other existing file: refused.
    if (fs::exists(to, ec) && !fs::equivalent(from, to, ec)) return "Not renamed: " + to.filename().u8string() + " already exists";
    fs::rename(from, to, ec);
    if (ec) return "Not renamed: " + ec.message();
    open();                                          // the list follows; the index is the file's MD5: nothing is read again
    return "Renamed to " + to.filename().u8string();
}

std::string Service::remove(const std::string& path) {
    const fs::path p = fs::u8path(path);
    std::error_code ec;
    if (isFactoryFile(path) || !insideLibrary(libraryDir(), p) || !fs::is_regular_file(p, ec)) return "Not deleted: not a library file";
    if (!cfg_.moveToTrash || !cfg_.moveToTrash(path)) return "Not deleted: " + p.filename().u8string() + " could not be moved to the Trash";
    open();
    return "Deleted " + p.stem().u8string() + " (in the Trash)";
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
        case Job::Type::PrepareSave: doPrepareSave(); break;
        case Job::Type::SaveAs: doSaveAs(job.draft, job.name); break;
        case Job::Type::Export: doExport(job.bank); break;
        case Job::Type::Import: doImport(job.paths); break;
        case Job::Type::LoadBank: doLoadBank(job.path, job.bank); break;
        }
        if (job.type == Job::Type::LoadBank) { std::lock_guard<std::mutex> lk(mtx_); bankJob_ = BankJob(); }
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
    for (const auto& [path, md5] : todo) {
        Job j; j.type = Job::Type::Index; j.path = path; j.md5 = md5;
        enqueue(std::move(j), false);
    }
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

// Save with a name, step 1: the clone's view of the current preset (as the panel shows it), or why not.
void Service::doPrepareSave() {
    SaveDraft d;
    auto done = [&] { { std::lock_guard<std::mutex> lk(mtx_); drafts_.push_back(std::move(d)); } changed(); };
    const std::vector<uint8_t> state = cfg_.machineState ? cfg_.machineState() : std::vector<uint8_t>();
    std::string err;
    std::unique_ptr<Ops> ops = state.empty() ? nullptr : machines_->clone(state, cancel_, err);
    if (!ops) { d.error = "NOT SAVED  THE SYNTH IS NOT RUNNING"; done(); return; }
    // Unsaved edits first (the LED does not depend on what the display shows); then the preset display: the clone shows
    // what the panel showed (`donE` right after a panel Dump, or an effect page: waitForPresetDisplay).
    if (ops->compareLit()) { d.compare = true; done(); return; }
    if (!ops->waitForPreset()) { d.error = "NOT SAVED  " + upper(ops->error()); done(); return; }
    d.preset = ops->displayedPreset();
    if (!ops->readName(0, d.osName) || !ops->dumpPreset(d.message)) {
        d.message.clear();
        d.error = "NOT SAVED  " + upper(ops->error());
    }
    done();
}

// Step 2: the draft's bytes as they are, in a new file (never overwriting); its index is known at once.
void Service::doSaveAs(const SaveDraft& draft, const std::string& name) {
    if (draft.message.empty()) { notice("NOT SAVED  NOTHING TO SAVE"); return; }
    const std::string typed = trimmedName(name);
    const std::string stem = safeFileName(typed.empty() ? draft.osName : typed);
    const std::string dir = singlesDir(libraryDir());
    std::string path;
    for (int tries = 0; tries < 10 && path.empty(); ++tries) {   // another writer may take the name first: never overwrite
        const std::string p = uniquePath(dir, stem);
        if (writeNew(p, draft.message)) path = p;
    }
    if (path.empty()) { notice("NOT SAVED  CANNOT WRITE THE FILE"); return; }
    const std::string md5 = md5Hex(draft.message.data(), draft.message.size());
    Index idx;
    idx.entries.push_back({draft.osName, draft.message, true});
    writeIndex(indexPath(indexDir(), md5), cfg_.osMd5, idx);
    { std::lock_guard<std::mutex> lk(mtx_); indexes_[md5] = std::move(idx); }
    notice("SAVED  Singles/" + fs::path(path).filename().string());
    refresh();
}

// The bank load: backup (cancellable), then the file's message into the running machine, then the names confirmed.
void Service::doLoadBank(const std::string& path, int bank) {
    const std::string title = fs::u8path(path).stem().u8string();
    const std::string bankText = "Bank " + std::to_string(bank + 1);
    std::vector<uint8_t> bytes, message;
    if (!readFile(path, bytes) || !oneBank(bytes, &message)) { notice(title + " has no bank"); return; }
    // The file's names: from its index (read now if it is not yet).
    const std::string md5 = md5Hex(bytes.data(), bytes.size());
    std::vector<std::string> names;
    auto known = [&] {
        std::lock_guard<std::mutex> lk(mtx_);
        const Index* idx = indexOf(md5);
        if (!idx || idx->entries.size() != size_t(kBankPresets)) return false;
        names.clear();
        for (const Entry& e : idx->entries) names.push_back(e.name);
        return true;
    };
    setPhase(BankJob::Phase::Backup);
    if (!known()) index(path, md5);
    if (cancel_) { notice("NOT LOADED  CANCELLED"); return; }
    if (!known()) { notice("NOT LOADED  " + title + " COULD NOT BE READ"); return; }
    // The backup: a clone dumps the bank as it is now. No backup, no load.
    {
        const std::vector<uint8_t> state = cfg_.machineState ? cfg_.machineState() : std::vector<uint8_t>();
        std::string err;
        std::unique_ptr<Ops> ops = state.empty() ? nullptr : machines_->clone(state, cancel_, err);
        std::vector<uint8_t> backup;
        const bool dumped = ops && ops->waitForPreset() && ops->programChange(63) && ops->dumpBank(bank, backup, nullptr);
        if (cancel_) { notice("NOT LOADED  CANCELLED"); return; }
        if (!dumped) {
            notice("NOT LOADED  " + upper(bankText) + " COULD NOT BE BACKED UP  " + upper(ops ? ops->error() : std::string("the synth is not running")));
            return;
        }
        const std::string dir = backupsDir(libraryDir());
        const std::string base = backupName(bank, std::time(nullptr));
        bool written = false;
        for (int tries = 0; tries < 10 && !written; ++tries) written = writeNew(uniquePath(dir, base), backup);
        if (!written) { notice("NOT LOADED  " + upper(bankText) + " COULD NOT BE BACKED UP  CANNOT WRITE THE FILE"); return; }
    }
    if (cancel_) { notice("NOT LOADED  CANCELLED"); return; }
    // The load: byte 6 is the target bank, nothing else changes. From here on it is not cancelled.
    setPhase(BankJob::Phase::Load);
    message[6] = uint8_t(bank);
    if (!cfg_.sendToMachine || !cfg_.sendToMachine(message)) { notice("NOT LOADED  THE SYNTH IS NOT RUNNING"); refresh(); return; }
    using Clock = std::chrono::steady_clock;
    auto ms = [](Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); };
    const auto sent = Clock::now();
    while (cfg_.machineInputBusy && cfg_.machineInputBusy() && ms(sent) < cfg_.bankDeliveryMs && !quit_)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    // Confirmed when the bank's 32 stored names (read in a clone of the running machine) are the file's.
    setPhase(BankJob::Phase::Confirm);
    const auto start = Clock::now();
    bool ok = false;
    std::atomic<bool> never{false};                  // the confirmation is not cancelled (the load was sent)
    while (!ok && !quit_) {
        const std::vector<uint8_t> state = cfg_.machineState ? cfg_.machineState() : std::vector<uint8_t>();
        std::string err;
        std::unique_ptr<Ops> ops = state.empty() ? nullptr : machines_->clone(state, never, err);
        ok = ops != nullptr;
        for (int i = 0; ok && i < kBankPresets; ++i) {
            std::string name;
            ok = ops->readName(uint16_t(0x40 + bank * kBankPresets + i), name) && name == names[size_t(i)];
        }
        if (ok || ms(start) >= cfg_.bankConfirmMs) break;
        for (int i = 0; i < 25 && !quit_; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (!ok) {
        if (!quit_) notice(bankText + " didn't confirm the load");
        refresh();
        return;
    }
    { std::lock_guard<std::mutex> lk(mtx_); loaded_.emplace_back(bank, title); }
    notice("LOADED " + title + " INTO " + upper(bankText));
    refresh();                                       // the new backup in the list
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
        v.group = src.group;
        v.label = src.label;
        if (src.type == Source::Type::Singles) v.path = singlesDir(cfg_.dataDir + "/Library");
        else if (!src.files.empty()) v.path = scan_.files[size_t(src.files[0])].path;
        else if (src.type == Source::Type::Factory) v.path = factoryFile(cfg_.dataDir + "/Library", src.factoryBank);
        v.bank = src.type != Source::Type::Singles && src.files.size() == 1 && scan_.files[size_t(src.files[0])].oneBank;
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
        if (src.type == Source::Type::Singles) {         // by file name (the name it was saved under)
            if (idx && !idx->entries.empty()) out.push_back({f.title, true, idx->entries[0].accepted, f.path});
            else out.push_back({f.title, false, true, f.path});
        } else if (idx) {
            for (const Entry& e : idx->entries) out.push_back({e.name, true, e.accepted, f.path});
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
