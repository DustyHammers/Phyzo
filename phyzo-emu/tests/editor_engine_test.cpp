// Editor engine test, part 2 (no ROM data): the editor's MIDI transport in the engine. A 68k program of our own
// echoes every MIDI byte it receives, so the OS's MIDI output shows exactly what reached its input, in order.
//  1. Editor closed (and never opened): the audio and the MIDI output are identical to an engine whose editor was
//     never touched, with program changes, bank selects and SysEx going in; nothing extra reaches the OS.
//  2. A program change, a bank select or a SysEx dump from the host or the virtual port is followed directly by the
//     editor's standing read.
//  3. Reply filter: the OS's replies to the editor's requests are taken out of the MIDI output and handed to the
//     editor; replies to other senders' requests, and replies when nothing is pending, pass unchanged.
//  5. Reply routing with requests the OS answers with its error message (0x7F) or not at all: partial-read replies by
//     the address they echo, the error message to the oldest request, each request expiring on its own.
//  4. The editor's messages never split a host message: a 3000-byte SysEx stays whole with editor messages around it.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>
#include "editor/editor_proto.h"
#include "engine.h"

namespace fs = std::filesystem;
namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
using Msg = std::vector<uint8_t>;

// main() at 0x4080: channel B on, then echo every received byte (as in midi_test).
const uint16_t kEchoOut[] = {0x13FC, 0x0005, 0x00F0, 0x071A, 0x0839, 0x0000, 0x00F0, 0x0719, 0x67F6, 0x1239, 0x00F0, 0x071B,
                             0x0839, 0x0002, 0x00F0, 0x0719, 0x67F6, 0x13C1, 0x00F0, 0x071B, 0x60DE};

struct Rig {
    Engine e;
    std::vector<Msg> out;                            // the OS's MIDI output as the host receives it
    std::vector<float> audio;
    explicit Rig(const fs::path& dir) {
        e.setRoms((dir / "os").string(), (dir / "wave").string(), "os-md5", "wave-md5");
        for (int i = 0; i < 1000 && e.state() != Engine::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(e.state() == Engine::State::Running);
        e.setMidiToHost(true);
        e.prepare(48000, 256);
    }
    void block(const std::vector<Msg>& host = {}) {
        std::vector<Engine::MidiEvent> ev;
        for (size_t i = 0; i < host.size(); ++i) ev.push_back({int(10 + i), host[i].data(), int(host[i].size())});
        std::vector<float> l(256), r(256);
        e.process(l.data(), r.data(), 256, ev.data(), int(ev.size()));
        audio.insert(audio.end(), l.begin(), l.end());
        for (const auto& m : e.hostMidiEvents()) out.emplace_back(e.hostMidiBytes() + m.start, e.hostMidiBytes() + m.start + m.size);
    }
    void run(int blocks) { for (int i = 0; i < blocks; ++i) block(); }
    std::vector<Msg> replies() {
        std::vector<Msg> v;
        while (e.editorReplies().peekSize()) { Msg m; e.editorReplies().pop([&](uint8_t b) { m.push_back(b); }); v.push_back(m); }
        return v;
    }
};

const edproto::Address kRead{edproto::kSound, 0, 2, 0, 0x30, 0x20};

// A reply as the OS sends it (device 00, command 0x44), echoing the request's address; `marker` tells replies apart.
Msg reply(uint8_t marker, const edproto::Address& a = kRead) {
    std::vector<uint8_t> data(a.length, 0);
    data[0] = marker;
    Msg m = edproto::partialWrite(a, data);
    m[4] = 0x00; m[5] = edproto::kPartialReply;
    return m;
}
Msg error(uint8_t code) { return {0xF0, 0x0F, 0x11, 0x01, 0x00, edproto::kError, code, 0xF7}; }   // the OS's error message
std::vector<Msg> answers(const std::vector<Msg>& v) {   // the OS's replies among the output (requests echo too)
    std::vector<Msg> r;
    for (const Msg& m : v) if (edproto::isReply(m.data(), m.size())) r.push_back(m);
    return r;
}
}  // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() / "phyzo_editor_engine_test";
    fs::remove_all(dir); fs::create_directories(dir);
    {
        std::vector<uint8_t> os(0x1000, 0);
        for (size_t i = 0; i < sizeof kEchoOut / 2; ++i) { os[0x80 + 2 * i] = uint8_t(kEchoOut[i] >> 8); os[0x81 + 2 * i] = uint8_t(kEchoOut[i]); }
        std::ofstream(dir / "os", std::ios::binary).write(reinterpret_cast<const char*>(os.data()), long(os.size()));
        std::vector<char> wave(0x400000, 0);
        std::ofstream(dir / "wave", std::ios::binary).write(wave.data(), long(wave.size()));
    }
    const Msg pc = {0xC0, 0x05}, bank = {0xB0, 0x00, 0x01}, note = {0x90, 0x40, 0x40}, dump = {0xF0, 0x0F, 0x11, 0x01, 0x00, 0x02, 0x10, 0xF7};
    const Msg read = edproto::partialRead(kRead);

    // 1. Closed = untouched.
    {
        Rig a(dir), b(dir);
        b.e.setEditorOpen(true);
        b.e.setEditorStandingRead({read});
        b.e.setEditorOpen(false);
        CHECK(!b.e.editorSend(read.data(), read.size()));
        b.e.setEditorStandingRead({read});           // set while closed: still never sent
        const std::vector<std::vector<Msg>> feed = {{pc}, {}, {bank, note}, {dump}, {}, {reply(1)}, {}};
        for (int i = 0; i < 400; ++i) { a.block(feed[size_t(i) % feed.size()]); b.block(feed[size_t(i) % feed.size()]); }
        CHECK(a.out == b.out && !a.out.empty());
        CHECK(a.audio == b.audio);
        CHECK(b.e.editorRequestsForTest() == 0 && b.replies().empty());
        std::printf("editor_engine_test: editor closed: %zu output messages and the audio identical to an untouched engine\n", a.out.size());
    }

    // 2. The standing read goes in directly behind a preset trigger.
    {
        Rig r(dir);
        r.e.setEditorOpen(true);
        r.e.setEditorStandingRead({read});
        r.block({note, pc, note});
        r.run(60);
        r.block({bank});
        r.run(60);
        r.e.virtualMidiIn(dump.data(), dump.size());
        r.run(60);
        const std::vector<Msg> want = {note, pc, read, note, bank, read, dump, read};
        CHECK(r.out == want);
        CHECK(r.e.presetTriggers() == 3);
        std::printf("editor_engine_test: program change, bank select and a SysEx dump each followed by the standing read\n");
    }

    // 3. Reply filter.
    {
        Rig r(dir);
        r.e.setEditorOpen(true);
        CHECK(r.e.editorSend(read.data(), read.size()));
        r.run(40);                                   // the request reaches the OS (and echoes: a request, not a reply)
        r.block({reply(1)});                          // the "OS's reply": to the editor
        r.run(40);
        r.block({reply(2)});                          // nothing pending: passes
        r.run(40);
        CHECK((r.out == std::vector<Msg>{read, reply(2)}));
        CHECK((r.replies() == std::vector<Msg>{reply(1)}));
        // Another sender's request first, then the editor's: the first reply is the other sender's.
        r.out.clear();
        r.block({read});                             // from the host
        r.e.editorSend(read.data(), read.size());
        r.run(40);
        r.block({reply(3), reply(4)});
        r.run(40);
        CHECK((r.out == std::vector<Msg>{read, read, reply(3)}));
        CHECK((r.replies() == std::vector<Msg>{reply(4)}));
        // Other OS output passes while a request is pending.
        r.out.clear();
        r.e.editorSend(read.data(), read.size());
        r.run(40);
        r.block({note, reply(5)});
        r.run(40);
        CHECK((r.out == std::vector<Msg>{read, note}) && (r.replies() == std::vector<Msg>{reply(5)}));
        std::printf("editor_engine_test: replies to the editor's requests removed from MIDI out, all other output passes\n");
    }

    // 4. The editor never splits a host message.
    {
        Rig r(dir);
        r.e.setEditorOpen(true);
        Msg big = {0xF0, 0x01};
        for (int i = 0; i < 3000; ++i) big.push_back(uint8_t(i & 0x7F));
        big.push_back(0xF7);
        const Msg shortMsg = {0xF0, 0x0F, 0x11, 0x01, 0x7F, 0x13, 0x00, 0xF7};
        std::vector<Msg> want;
        for (int i = 0; i < 40; ++i) {
            std::vector<Msg> host;
            if (i == 3) host.push_back(big);
            if (i % 5 == 0) host.push_back(note);
            if (i % 2 == 0) r.e.editorSend(shortMsg.data(), shortMsg.size());
            r.block(host);
        }
        r.run(800);
        size_t bigs = 0, shorts = 0, notes = 0;
        bool whole = true;
        for (const Msg& m : r.out) {
            if (m == big) ++bigs;
            else if (m == shortMsg) ++shorts;
            else if (m == note) ++notes;
            else whole = false;
        }
        CHECK(whole && bigs == 1 && shorts == 20 && notes == 8);
        std::printf("editor_engine_test: a 3000-byte host SysEx stays whole among %zu editor messages\n", shorts);
    }

    // 5. The OS answers some requests with its error message and others not at all.
    {
        Rig r(dir);
        r.e.setEditorOpen(true);
        const edproto::Address aE{edproto::kSound, 0, 2, 0, 0x40, 0x10}, aH{edproto::kSound, 0, 2, 1, 0x40, 0x10};
        const Msg readE = edproto::partialRead(aE), readH = edproto::partialRead(aH), bad = edproto::partialRead(kRead);
        // A host request answered with the error message, then an editor request and a host request.
        r.block({bad});
        r.run(40);
        r.block({error(1)});
        r.run(40);
        r.e.editorSend(readE.data(), readE.size());
        r.run(40);
        r.block({readH});
        r.run(40);
        r.block({reply(2, aE), reply(3, aH)});
        r.run(40);
        CHECK((answers(r.out) == std::vector<Msg>{error(1), reply(3, aH)}));
        CHECK((r.replies() == std::vector<Msg>{reply(2, aE)}));
        // The same with the replies the other way round: each still reaches its own sender.
        r.out.clear();
        r.e.editorSend(readE.data(), readE.size());
        r.run(40);
        r.block({readH});
        r.run(40);
        r.block({reply(4, aH), reply(5, aE)});
        r.run(40);
        CHECK((answers(r.out) == std::vector<Msg>{reply(4, aH)}));
        CHECK((r.replies() == std::vector<Msg>{reply(5, aE)}));
        // An editor request answered with the error message goes to the editor; a host request after it gets its reply.
        r.out.clear();
        r.e.editorSend(bad.data(), bad.size());
        r.run(40);
        r.block({readH});
        r.run(40);
        r.block({error(6), reply(7, aH)});
        r.run(40);
        CHECK((answers(r.out) == std::vector<Msg>{reply(7, aH)}));
        CHECK((r.replies() == std::vector<Msg>{error(6)}));
        // An editor request the OS never answers: later replies still find their senders, and it expires on its own
        // (an error message after that passes).
        r.out.clear();
        r.e.editorSend(bad.data(), bad.size());
        r.run(40);
        r.block({readH});
        r.e.editorSend(readE.data(), readE.size());
        r.run(40);
        r.block({reply(8, aE), reply(9, aH)});
        r.run(40);
        CHECK((answers(r.out) == std::vector<Msg>{reply(9, aH)}));
        CHECK((r.replies() == std::vector<Msg>{reply(8, aE)}));
        r.out.clear();
        r.run(int(Engine::kEditorReplyTimeoutSamples * 48000 / 44100 / 256) + 20);
        r.block({error(10)});
        r.run(40);
        CHECK((answers(r.out) == std::vector<Msg>{error(10)}) && r.replies().empty());
        std::printf("editor_engine_test: error messages and unanswered requests: every reply reaches its own sender\n");
    }

    fs::remove_all(dir);
    std::printf("editor_engine_test: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
