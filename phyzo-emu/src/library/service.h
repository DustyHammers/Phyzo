// The preset library's jobs (v0.8.3 step 2; no plugin framework here). One worker thread runs one job at a time, never
// the audio thread; jobs are queued, can be cancelled, and are stopped when the service is destroyed (the plugin
// closes). The synth's own memory is never written: a library preset reaches the synth only when the UI sends its
// stored message (into Compare, as a single-preset dump over MIDI does), and files are written only on request
// (SAVE, EXPORT, IMPORT) and for the factory bank files.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "library/hidden_machine.h"
#include "library/library.h"

namespace library {

class Service {
public:
    struct Config {
        std::string dataDir;                         // the plugin's data folder: Library/ and cache/library/ inside
        std::string osMd5;                           // the OS image's MD5 (an index made with another OS is not used)
        std::function<std::vector<uint8_t>()> machineState;   // the running machine's state for a clone (Engine snapshot)
    };
    Service(Config cfg, std::unique_ptr<Machines> machines);
    ~Service();                                      // cancels the running job and the queue, then joins

    std::string libraryDir() const { return cfg_.dataDir + "/Library"; }

    // ---- any thread (the UI): each queues a job and returns at once
    // The LIBRARY area opened: make the factory bank files that are missing, rescan, read the files not in the cache.
    void open();
    // SAVE: a clone dumps the current preset into Library/Singles/<name>.syx (never overwriting a file). The caller has
    // checked the Compare LED; the clone checks it again.
    void save();
    // EXPORT B1 / B2: a clone dumps the bank; the message arrives through takeExport.
    void exportBank(int bank);
    // IMPORT: copies the files into Library/ (single-preset files into Library/Singles/), never overwriting, then reads them.
    void import(const std::vector<std::string>& paths);
    // Stops the running job and drops the queue.
    void cancelAll();

    // ---- the UI's view (any thread)
    struct SourceView {
        std::string title;
        size_t count = 0;                            // presets listed (SINGLES: files)
        size_t done = 0, total = 0;                  // while busy: progress (shown as done/total)
        bool busy = false;
    };
    struct EntryView { std::string name; bool ready = false, accepted = true; };
    uint64_t serial() const { return serial_.load(); }   // changes whenever anything below changes
    std::vector<SourceView> sources() const;
    std::vector<EntryView> entries(int source) const;
    // The stored single-preset message of a source's entry (false: not read yet, or not accepted by the OS).
    bool message(int source, int entry, std::vector<uint8_t>& out) const;
    bool takeNotice(std::string& text);              // messages for the UI, in order
    bool takeExport(int& bank, std::vector<uint8_t>& bytes);
    bool busy() const;                               // a job is running or queued
    std::thread::id workerId() const { return worker_.get_id(); }

private:
    struct Job { enum class Type { Refresh, Index, Save, Export, Import } type; std::string path, md5; int bank = 0; std::vector<std::string> paths; };
    void enqueue(Job job, bool front);
    void run();
    void refresh();
    void makeFactory(const std::vector<int>& banks);
    void index(const std::string& path, const std::string& md5);
    void doSave();
    void doExport(int bank);
    void doImport(const std::vector<std::string>& paths);
    void notice(const std::string& text);
    void changed() { ++serial_; }
    const Index* indexOf(const std::string& md5) const;   // under mtx_
    std::string indexDir() const { return cfg_.dataDir + "/cache/library"; }

    Config cfg_;
    std::unique_ptr<Machines> machines_;
    std::atomic<bool> cancel_{false}, quit_{false};
    std::thread worker_;
    mutable std::mutex mtx_;                          // everything below
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool running_ = false;
    Scan scan_;
    std::map<std::string, Index> indexes_;          // by file MD5
    struct Progress { size_t done = 0, total = 0; };
    std::map<std::string, Progress> progress_;      // by file MD5, or "factory0" / "factory1" while making those files
    std::set<std::string> reportedBad_;
    std::deque<std::string> notices_;
    std::deque<std::pair<int, std::vector<uint8_t>>> exports_;
    std::atomic<uint64_t> serial_{0};
};

}  // namespace library
