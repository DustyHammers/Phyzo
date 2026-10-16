// The preset library's jobs (v0.8.3 step 2, v0.9.0; no plugin framework here). One worker thread runs one job at a
// time, never the audio thread (and one at a time in the whole process: instances take turns); jobs are queued, can be
// cancelled, and are stopped when the service is destroyed (the plugin closes). A library preset reaches the synth
// only when the UI sends its stored message (into Compare, as a single-preset dump over MIDI does); the synth's memory
// is written only by a bank load the user confirmed (Bank 1 or 2, backed up first). Files are written only on request
// (Save, Save as file, Import, Rename, Delete, the bank load's backup) and for the factory bank files.
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
        // The bank load (v0.9.0): a whole message into the running machine's MIDI input (Engine::libraryInput; false:
        // not taken), and whether it is still on its way (Engine::libraryInputBusy).
        std::function<bool(const std::vector<uint8_t>&)> sendToMachine;
        std::function<bool()> machineInputBusy;
        // Delete: the file to the Trash (the plugin: JUCE's File::moveToTrash). None: Delete is refused.
        std::function<bool(const std::string& path)> moveToTrash;
        // The bank load: how long the OS may take to read the message (a bank takes about 33 s at cable speed), then
        // how long the stored names may take to match the file's.
        double bankDeliveryMs = 120000, bankConfirmMs = 30000;
    };
    Service(Config cfg, std::unique_ptr<Machines> machines);
    ~Service();                                      // cancels the running job and the queue, then joins

    std::string libraryDir() const { return cfg_.dataDir + "/Library"; }

    // ---- any thread (the UI): each queues a job and returns at once
    // The LIBRARY tab opened: make the factory bank files that are missing, rescan, read the files not in the cache.
    void open();
    // Save with a name (v0.9.0), step 1: a clone checks the Compare LED, then reads the current preset's number and
    // name and dumps it. The draft arrives through takeSaveDraft (compare: unsaved edits, nothing dumped).
    struct SaveDraft {
        bool compare = false;                        // the Compare LED was lit: "save on the panel first"
        int preset = -1;                             // 0-63 (the display's P nn)
        std::string osName;                          // the OS's name, trimmed
        std::vector<uint8_t> message;                // the single-preset message (empty: not dumped; see error)
        std::string error;
    };
    void prepareSave();
    bool takeSaveDraft(SaveDraft& out);
    // Step 2: writes the draft's message to Library/Singles/<name>.syx: the name as typed, made safe for a file name
    // ('_' for characters file systems reserve); empty: the OS's name; never overwriting (" 2", " 3" ...). The bytes
    // are the dump's, unchanged. Notice "SAVED  Singles/<file>".
    void saveAs(const SaveDraft& draft, const std::string& name);
    // Save as file (EXPORT B1 / B2): a clone dumps the bank; the message arrives through takeExport.
    void exportBank(int bank);
    // IMPORT: copies the files into Library/ (single-preset files into Library/Singles/), never overwriting, then reads them.
    void import(const std::vector<std::string>& paths);
    // Stops the running job and drops the queue.
    void cancelAll();

    // ---- Rename and Delete (v0.9.0; UI thread, at once). A library file only, never the factory files. Rename keeps
    // the folder and changes the file name (made safe as Save's; refused if that name exists); the index is keyed by
    // the file's MD5, so nothing is read again. Delete moves the file to the Trash. Each returns the footer's message
    // ("" for nothing to say); the list follows after a rescan (queued).
    std::string rename(const std::string& path, const std::string& newName);
    std::string remove(const std::string& path);
    bool isFactoryFile(const std::string& path) const;

    // ---- the bank load (v0.9.0): a bank file into Bank 1 or 2 of the running machine. A clone first dumps the bank to
    // Library/Backups/Bank N YYYY-MM-DD HHMM.syx (never overwriting); if that fails nothing is sent. Then the file's
    // bank message, with byte 6 set to the target bank and nothing else changed, goes into the running machine's MIDI
    // input (Config::sendToMachine); the load is done when the bank's 32 stored names (read in a clone) equal the
    // file's names from its index. One bank job at a time: false (nothing queued) while one is queued or running, or
    // the file does not hold exactly one bank (notice "<file> has no bank").
    bool loadBank(const std::string& path, int bank);
    struct BankJob {
        bool active = false;
        int bank = 0;
        enum class Phase { Queued, Backup, Load, Confirm } phase = Phase::Queued;
        std::string file;                            // the file's title
    };
    BankJob bankJob() const;
    void cancelBankBackup();                         // Cancel: only while it is queued or backing up
    bool takeLoaded(int& bank, std::string& file);   // a confirmed load: the Memory column's new title
    // Whether a file holds exactly one bank (and nothing else that is a preset): what Load can take.
    static bool oneBank(const std::vector<uint8_t>& file, std::vector<uint8_t>* message = nullptr);

    // ---- the UI's view (any thread)
    struct SourceView {
        std::string title;
        Source::Group group = Source::Group::Banks;
        std::string label;                           // the window's name (Source::label)
        std::string path;                            // the file (SINGLES: the Singles folder; a factory file missing: its path)
        size_t count = 0;                            // presets listed (SINGLES: files)
        size_t done = 0, total = 0;                  // while busy: progress (shown as done/total)
        bool busy = false;
        bool bank = false;                           // exactly one bank: Load can take it
    };
    // A preset: its name (the OS's; a single by its file name), whether it is read and taken by the OS, its file.
    struct EntryView { std::string name; bool ready = false, accepted = true; std::string path; };
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
    struct Job {
        enum class Type { Refresh, Index, PrepareSave, SaveAs, Export, Import, LoadBank } type;
        std::string path, md5; int bank = 0; std::vector<std::string> paths;
        SaveDraft draft; std::string name;
    };
    void enqueue(Job job, bool front);
    void run();
    void refresh();
    void makeFactory(const std::vector<int>& banks);
    void index(const std::string& path, const std::string& md5);
    void doPrepareSave();
    void doSaveAs(const SaveDraft& draft, const std::string& name);
    void doExport(int bank);
    void doLoadBank(const std::string& path, int bank);
    void setPhase(BankJob::Phase p);
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
    std::deque<SaveDraft> drafts_;
    BankJob bankJob_;
    std::deque<std::pair<int, std::string>> loaded_;
    std::atomic<uint64_t> serial_{0};
};

}  // namespace library
