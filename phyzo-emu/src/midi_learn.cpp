#include "midi_learn.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>

namespace midilearn {

namespace {
int32_t pack(int control, int ch, bool note, int num) { return int32_t(control) << 16 | ch << 8 | (note ? 0x80 : 0) | (num & 0x7F); }
}  // namespace

Learn::Learn(Panel& panel) : panel_(panel) {
    for (auto& e : cc_) e.store(kNone, std::memory_order_relaxed);
    for (auto& e : note_) e.store(kNone, std::memory_order_relaxed);
    for (auto& e : msb_) e.store(-1, std::memory_order_relaxed);
    for (auto& e : fine_) e.store(false, std::memory_order_relaxed);
    for (auto& e : moved_) e.store(0, std::memory_order_relaxed);
    for (auto& e : down_) e.store(false, std::memory_order_relaxed);
    for (auto& e : watch_) e.store(false, std::memory_order_relaxed);
}

// ------------------------------------------------------------------ message side

void Learn::arm(int control) {
    armed_.store(learnable(control) ? control : kNone);
    std::lock_guard<std::mutex> lk(listMtx_);
    updateActive();
}

void Learn::updateActive() {
    active_.store(!list_.empty() || armed_.load() != kNone || provisional_.load() >= 0, std::memory_order_release);
}

bool Learn::bind(const Binding& b, std::vector<int>* displaced, bool watch) {
    std::lock_guard<std::mutex> lk(listMtx_);
    return bindLocked(b, displaced, watch);
}

bool Learn::bindLocked(const Binding& in, std::vector<int>* displaced, bool watch) {
    Binding b = in;
    if (!learnable(b.control) || b.channel < 0 || b.channel > 16 || b.number < 0 || b.number > 127) return false;
    if (b.note && !isButton(b.control)) return false;
    if (!b.note && b.number > kMaxCc) return false;
    if (!isKnob(b.control)) b.fine = false;
    // The control's own binding: kept as it is (with its 14-bit flag) if the source does not change.
    for (auto it = list_.begin(); it != list_.end(); ++it) {
        if (it->control != b.control) continue;
        const bool same = it->channel == b.channel && it->note == b.note && it->number == b.number;
        if (same) b.fine = b.fine || (isKnob(b.control) && fine_[size_t(b.control)].load());
        else releaseLocked(b.control);
        list_.erase(it);
        break;
    }
    // Whoever holds the same source moves out: the same channel, or either side on Any.
    for (auto it = list_.begin(); it != list_.end();) {
        if (it->note == b.note && it->number == b.number && (b.channel == 0 || it->channel == 0 || it->channel == b.channel)) {
            releaseLocked(it->control);
            if (isKnob(it->control)) { fine_[size_t(it->control)] = false; msb_[size_t(it->control)] = -1; }
            if (isButton(it->control)) watch_[size_t(buttonRaw(it->control))] = false;
            if (displaced) displaced->push_back(it->control);
            it = list_.erase(it);
        } else {
            ++it;
        }
    }
    list_.push_back(b);
    std::sort(list_.begin(), list_.end(), [](const Binding& x, const Binding& y) { return x.control < y.control; });
    if (isKnob(b.control)) {
        if (!b.fine) msb_[size_t(b.control)] = -1;
        fine_[size_t(b.control)] = b.fine;
    }
    if (isButton(b.control)) watch_[size_t(buttonRaw(b.control))] = watch;
    rebuild();
    updateActive();
    return true;
}

bool Learn::forget(int control) {
    std::lock_guard<std::mutex> lk(listMtx_);
    for (auto it = list_.begin(); it != list_.end(); ++it) {
        if (it->control != control) continue;
        releaseLocked(control);
        if (isKnob(control)) { fine_[size_t(control)] = false; msb_[size_t(control)] = -1; }
        if (isButton(control)) watch_[size_t(buttonRaw(control))] = false;
        list_.erase(it);
        rebuild();
        updateActive();
        return true;
    }
    return false;
}

void Learn::forgetAll() { setBindings({}); }

void Learn::setBindings(const std::vector<Binding>& list) {
    std::lock_guard<std::mutex> lk(listMtx_);
    for (int raw = 0; raw < 128; ++raw) releaseLocked(button(raw));
    list_.clear();
    for (auto& f : fine_) f = false;
    for (auto& m : msb_) m = -1;
    for (auto& w : watch_) w = false;
    for (const Binding& b : list) bindLocked(b, nullptr, false);
    rebuild();
    updateActive();
}

std::vector<Binding> Learn::bindings() const {
    std::lock_guard<std::mutex> lk(listMtx_);
    std::vector<Binding> out = list_;
    for (Binding& b : out) if (isKnob(b.control)) b.fine = fine_[size_t(b.control)].load();
    return out;
}

bool Learn::bindingOf(int control, Binding& out) const {
    std::lock_guard<std::mutex> lk(listMtx_);
    for (const Binding& b : list_)
        if (b.control == control) {
            out = b;
            if (isKnob(control)) out.fine = fine_[size_t(control)].load();
            return true;
        }
    return false;
}

int Learn::controlAt(int channel, bool note, int number) const {
    std::lock_guard<std::mutex> lk(listMtx_);
    for (const Binding& b : list_)
        if (b.channel == channel && b.note == note && b.number == number) return b.control;
    return kNone;
}

bool Learn::takeEvent(Event& e) {
    const uint32_t tail = qTail_.load(std::memory_order_relaxed);
    if (tail == qHead_.load(std::memory_order_acquire)) return false;
    e = queue_[tail % kQueue];
    qTail_.store(tail + 1, std::memory_order_release);
    return true;
}

bool Learn::commit(const Event& e, std::vector<int>* displaced) {
    std::lock_guard<std::mutex> lk(listMtx_);
    const bool ok = e.kind == Event::Learned && bindLocked({e.control, e.channel, e.note, e.number, false}, displaced, true);
    int32_t expect = pack(e.control, e.channel, e.note, e.number);
    provisional_.compare_exchange_strong(expect, -1);   // from now on the table holds it
    updateActive();
    return ok;
}

void Learn::releaseLocked(int control) {
    if (!isButton(control)) return;
    const int raw = buttonRaw(control);
    if (down_[size_t(raw)].exchange(false)) panel_.button(raw, false);
}

void Learn::releaseHeld(int control) {
    std::lock_guard<std::mutex> lk(listMtx_);
    if (control != kNone) { releaseLocked(control); return; }
    for (int raw = 0; raw < 128; ++raw) releaseLocked(button(raw));
}

void Learn::releaseOrphans() {
    std::lock_guard<std::mutex> lk(listMtx_);
    const int32_t pv = provisional_.load();
    for (int raw = 0; raw < 128; ++raw) {
        if (!down_[size_t(raw)].load()) continue;
        const int c = button(raw);
        if (pv >= 0 && (pv >> 16) == c) continue;
        if (std::none_of(list_.begin(), list_.end(), [c](const Binding& b) { return b.control == c; })) releaseLocked(c);
    }
}

bool Learn::held(int control) const { return isButton(control) && down_[size_t(buttonRaw(control))].load(); }

bool Learn::takeKnobMove(int cc, int& raw) {
    if (!isKnob(cc)) return false;
    const uint16_t v = moved_[size_t(cc)].exchange(0, std::memory_order_acq_rel);
    if (!(v & 0x8000)) return false;
    raw = v & 0x3FF;
    return true;
}

void Learn::rebuild() {
    std::array<int16_t, kChannels * 128> cc, note;
    cc.fill(kNone);
    note.fill(kNone);
    for (const Binding& b : list_) (b.note ? note : cc)[size_t(b.channel * 128 + b.number)] = int16_t(b.control);
    for (size_t i = 0; i < cc.size(); ++i) {
        if (cc_[i].load(std::memory_order_relaxed) != cc[i]) cc_[i].store(cc[i], std::memory_order_relaxed);
        if (note_[i].load(std::memory_order_relaxed) != note[i]) note_[i].store(note[i], std::memory_order_relaxed);
    }
}

// ------------------------------------------------------------------ audio thread

bool Learn::handle(const uint8_t* d, int size, double nowMs) {
    if (size < 3 || !d) return false;
    const uint8_t st = d[0];
    if (st < 0x80 || st >= 0xF0) return false;              // SysEx, system and real-time messages pass
    const int type = st & 0xF0, ch = (st & 0x0F) + 1, num = d[1] & 0x7F, val = d[2] & 0x7F;
    const bool cc = type == 0xB0, note = type == 0x90 || type == 0x80;
    if (!cc && !note) return false;                         // pitch bend, pressure, program change ... pass
    if (cc && num > kMaxCc) return false;                   // channel mode messages pass
    const int armed = armed_.load(std::memory_order_relaxed);
    if (armed != kNone && (cc || isButton(armed))) { learned(armed, ch, note, num); return true; }
    int c = kNone;
    const int32_t pv = provisional_.load(std::memory_order_relaxed);
    if (pv >= 0 && ((pv >> 8) & 0xFF) == ch && bool(pv & 0x80) == note && (pv & 0x7F) == num) c = pv >> 16;
    if (c == kNone) c = lookup(note ? note_ : cc_, ch, num);
    if (note) {
        if (!isButton(c)) return false;
        edge(buttonRaw(c), type == 0x90 && val > 0, nowMs);
        return true;
    }
    if (isKnob(c)) { knobValue(c, val); return true; }
    if (isButton(c)) { edge(buttonRaw(c), val >= 64, nowMs); return true; }
    if (num >= 32 && num <= 63) {                           // the LSB of a knob bound to CC num - 32 (on this channel)
        int m = kNone;
        if (pv >= 0 && ((pv >> 8) & 0xFF) == ch && !(pv & 0x80) && (pv & 0x7F) == num - 32) m = pv >> 16;
        if (m == kNone) m = lookup(cc_, ch, num - 32);
        if (isKnob(m)) { knobLsb(m, val); return true; }
    }
    return false;
}

void Learn::learned(int control, int ch, bool note, int num) {
    int expect = control;
    if (!armed_.compare_exchange_strong(expect, kNone)) return;
    provisional_.store(pack(control, ch, note, num));
    Event e;
    e.kind = Event::Learned; e.control = control; e.channel = ch; e.note = note; e.number = num;
    push(e);
}

void Learn::knobValue(int cc, int value) {
    const size_t i = size_t(cc);
    msb_[i].store(int16_t(value), std::memory_order_relaxed);
    const int raw = fine_[i].load(std::memory_order_relaxed) ? raw14(value << 7) : raw7(value);   // MSB: LSB = 0
    panel_.control(cc, raw);
    moved(cc, raw);
}

void Learn::knobLsb(int cc, int lsb) {
    const size_t i = size_t(cc);
    fine_[i].store(true, std::memory_order_relaxed);
    const int msb = msb_[i].load(std::memory_order_relaxed);
    if (msb < 0) return;                                    // no MSB yet: nothing to refine
    const int raw = raw14((msb << 7) | lsb);
    panel_.control(cc, raw);
    moved(cc, raw);
}

void Learn::edge(int raw, bool down, double nowMs) {
    const size_t i = size_t(raw);
    if (down) {
        if (down_[i].exchange(true)) {                      // pressed again with no release between: ignored
            if (watch_[i].exchange(false)) { Event e; e.kind = Event::OneEdge; e.control = button(raw); push(e); }
            return;
        }
        pressedAt_[i] = nowMs;
        panel_.button(raw, true);
    } else {
        if (!down_[i].exchange(false)) return;              // released again: ignored
        if (watch_[i].load(std::memory_order_relaxed) && nowMs - pressedAt_[i] <= kOneEdgeMs) watch_[i] = false;   // both edges: fine
        panel_.button(raw, false);
    }
}

void Learn::tick(double nowMs) {
    for (size_t i = 0; i < down_.size(); ++i) {
        if (!watch_[i].load(std::memory_order_relaxed) || !down_[i].load(std::memory_order_relaxed)) continue;
        if (nowMs - pressedAt_[i] <= kOneEdgeMs) continue;
        if (watch_[i].exchange(false)) { Event e; e.kind = Event::OneEdge; e.control = button(int(i)); push(e); }
    }
}

void Learn::moved(int cc, int raw) {
    moved_[size_t(cc)].store(uint16_t(0x8000 | raw), std::memory_order_release);
    moveSerial_.fetch_add(1, std::memory_order_relaxed);
}

void Learn::push(const Event& e) {
    const uint32_t head = qHead_.load(std::memory_order_relaxed);
    if (head - qTail_.load(std::memory_order_acquire) >= kQueue) return;   // full: dropped
    queue_[head % kQueue] = e;
    qHead_.store(head + 1, std::memory_order_release);
}

// ------------------------------------------------------------------ text form

std::string Learn::toText(const std::vector<Binding>& list) {
    std::string out;
    char buf[32];
    for (const Binding& b : list) {
        if (!learnable(b.control)) continue;
        if (isKnob(b.control)) std::snprintf(buf, sizeof buf, "k%d", b.control);
        else std::snprintf(buf, sizeof buf, "b%02X", buttonRaw(b.control));
        if (!out.empty()) out += ' ';
        out += buf;
        std::snprintf(buf, sizeof buf, ":%d:%c%d%s", b.channel, b.note ? 'n' : 'c', b.number, b.fine ? ":f" : "");
        out += buf;
    }
    return out;
}

std::vector<Binding> Learn::fromText(const std::string& text) {
    std::vector<Binding> out;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) {
        Binding b;
        unsigned id = 0;
        int ch = -1, num = -1, used = 0;
        char kind = 0, src = 0;
        if (std::sscanf(tok.c_str(), "%c", &kind) != 1) continue;
        if (kind == 'k' && std::sscanf(tok.c_str(), "k%u:%d:%c%d%n", &id, &ch, &src, &num, &used) == 4) b.control = knob(int(id));
        else if (kind == 'b' && std::sscanf(tok.c_str(), "b%x:%d:%c%d%n", &id, &ch, &src, &num, &used) == 4 && id <= 0x7F) b.control = button(int(id));
        else continue;
        if (!learnable(b.control) || ch < 0 || ch > 16 || (src != 'c' && src != 'n') || num < 0 || num > 127) continue;
        if ((src == 'c' && num > kMaxCc) || (src == 'n' && !isButton(b.control))) continue;
        b.channel = ch; b.note = src == 'n'; b.number = num;
        b.fine = tok.compare(size_t(used), std::string::npos, ":f") == 0;
        if (size_t(used) != tok.size() && !b.fine) continue;
        out.push_back(b);
    }
    return out;
}

// ------------------------------------------------------------------ names and texts

namespace {
const char* knobName(int cc) {
    static const char* const kNames[kKnobs] = {
        "Mod Speed", "Pan", "Level", nullptr, "Filter Amount", nullptr, nullptr, "Wave Amount", "Resonance", nullptr,
        "Wave Select", "Volume", "Sustain", nullptr, "Arp Value", "Mix", "Decay", "Attack", "Pitch Amount",
        "Glide Time", "Pitch Tune", "Release", "Cutoff"};
    return isKnob(cc) ? kNames[cc] : nullptr;
}
int perfIndex(int cc) {
    for (int i = 0; i < 5; ++i) if (kPerfCc[i] == cc) return i;
    return -1;
}
struct ButtonName { int raw; const char* name; const char* upper; };
const ButtonName kButtons[] = {
    {0x19, "Bank", "BANK"}, {0x0E, "\xe2\x88\x92/No", "-/NO"}, {0x0D, "+/Yes", "+/YES"}, {0x00, "Sound 1", "SOUND 1"},
    {0x07, "Sound 2", "SOUND 2"}, {0x09, "Sound 3", "SOUND 3"}, {0x02, "Sound 4", "SOUND 4"}, {0x20, "Compare", "COMPARE"},
    {0x0B, "Save", "SAVE"}, {0x08, "MIDI Edit", "MIDI EDIT"}, {0x0C, "Dump", "DUMP"}, {0x03, "Arp Range", "ARP RANGE"},
    {0x0F, "Osc 1", "OSC 1"}, {0x0A, "Osc 2", "OSC 2"}, {0x17, "Pitch Mod", "PITCH MOD"}, {0x1F, "Glide/Mono", "GLIDE/MONO"},
    {0x10, "Env Select", "ENV SELECT"}, {0x18, "Env Mode", "ENV MODE"}, {0x1A, "Env Velocity", "ENV VELOCITY"},
    {0x04, "FX Insert", "FX INSERT"}, {0x01, "FX Reverb", "FX REVERB"}, {0x1E, "Tempo", "TEMPO"}, {0x05, "Arp Kbd", "ARP KBD"},
    {0x1D, "Variation Up", "VARIATION UP"}, {0x16, "Variation Down", "VARIATION DOWN"}, {0x13, "FX Bus Select", "FX BUS SELECT"},
    {0x15, "Wave Mod", "WAVE MOD"}, {0x12, "Filter Env", "FILTER ENV"}, {0x11, "Filter Mod", "FILTER MOD"},
    {0x14, "Filter Kbd", "FILTER KBD"}, {0x1C, "Mod Select", "MOD SELECT"}, {0x1B, "Mod Shape", "MOD SHAPE"},
    {0x06, "Arp On/Off", "ARP ON/OFF"}, {0x21, "Arp Mode", "ARP MODE"}};
const ButtonName* buttonName(int raw) {
    for (const ButtonName& b : kButtons) if (b.raw == raw) return &b;
    return nullptr;
}
std::string upper(std::string s) { for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c))); return s; }
std::string trimmed(const std::string& s) {
    const size_t a = s.find_first_not_of(' '), b = s.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string perfLabel(int i, const PerfLabels& labels) {
    const std::string l = trimmed(labels[size_t(i)]);
    return l.empty() ? "Perf " + std::to_string(i + 1) : l;
}
std::string fit(std::string s) { if (s.size() > kMessageCols) s.resize(kMessageCols); return s; }
// "CC 21 CH 1", "CC 21 ANY CH", "NOTE 60 CH 2"; channel: with the channel part
std::string sourceUpper(const Binding& b, bool channel) {
    std::string s = (b.note ? "NOTE " : "CC ") + std::to_string(b.number);
    if (channel) s += b.channel == 0 ? " ANY CH" : " CH " + std::to_string(b.channel);
    return s;
}
}  // namespace

std::string controlName(int control, const PerfLabels& labels) {
    if (isKnob(control)) {
        const int p = perfIndex(control);
        if (p >= 0) {
            const std::string l = trimmed(labels[size_t(p)]);
            return l.empty() ? "Perf " + std::to_string(p + 1) : l + " (Perf " + std::to_string(p + 1) + ")";
        }
        return knobName(control);
    }
    if (isButton(control)) {
        if (const ButtonName* b = buttonName(buttonRaw(control))) return b->name;
        char buf[16];
        std::snprintf(buf, sizeof buf, "Button 0x%02X", buttonRaw(control));
        return buf;
    }
    return std::string();
}

std::string shortName(int control, const PerfLabels& labels) {
    if (isKnob(control)) {
        const int p = perfIndex(control);
        return upper(p >= 0 ? perfLabel(p, labels) : std::string(knobName(control)));
    }
    if (isButton(control)) {
        if (const ButtonName* b = buttonName(buttonRaw(control))) return b->upper;
        return upper(controlName(control, labels));
    }
    return std::string();
}

std::string sourceText(const Binding& b) {
    std::string s = (b.note ? "note " : "CC ") + std::to_string(b.number);
    if (b.fine && !b.note) s += " (14-bit)";
    s += b.channel == 0 ? ", any channel" : ", ch " + std::to_string(b.channel);
    return s;
}

std::string header(int control, const Binding* b, const PerfLabels& labels) {
    return controlName(control, labels) + " \xc2\xb7 " + (b ? sourceText(*b) : std::string("not learned"));
}

std::string listEntry(const Binding& b, const PerfLabels& labels) {
    std::string s = controlName(b.control, labels) + "  " + (b.note ? "note " : "CC ") + std::to_string(b.number);
    if (b.fine && !b.note) s += " (14-bit)";
    s += b.channel == 0 ? " any channel" : " ch " + std::to_string(b.channel);
    return s;
}

std::string osUse(int cc, const PerfLabels& labels) {
    switch (cc) {
    case 1: return "OS: mod wheel";
    case 12: return "OS: " + perfLabel(0, labels) + " knob, SYS3";
    case 71: return "OS: " + perfLabel(1, labels) + " knob";
    case 74: return "OS: " + perfLabel(2, labels) + " knob";
    case 79: return "OS: " + perfLabel(3, labels) + " knob";
    case 78: return "OS: " + perfLabel(4, labels) + " knob, SYS4";
    default: return std::string();
    }
}

std::string msgLearn(int control, const PerfLabels& labels) { return fit("LEARN " + shortName(control, labels) + "  MOVE A CONTROL..."); }

std::string msgBound(const Binding& b, const PerfLabels& labels) {
    return fit(shortName(b.control, labels) + " = " + sourceUpper(b, true));
}

std::string msgMoved(const Binding& b, const std::vector<int>& from, const PerfLabels& labels) {
    std::string names;
    for (int c : from) names += (names.empty() ? "" : ", ") + shortName(c, labels);
    return fit(sourceUpper(b, false) + " MOVED  " + names + " -> " + shortName(b.control, labels));
}

std::string msgForgotten(const Binding& b, const PerfLabels& labels) {
    return fit(shortName(b.control, labels) + ": " + sourceUpper(b, false) + " FORGOTTEN");
}

std::string msgOneEdge(const Binding& b, const PerfLabels& labels) {
    const std::string tail = "  ONE EDGE ONLY: SET BUTTON MOMENTARY";
    std::string s = shortName(b.control, labels) + " = " + sourceUpper(b, true) + tail;
    if (s.size() > kMessageCols) s = shortName(b.control, labels) + " = " + sourceUpper(b, false) + tail;
    return fit(s);
}

}  // namespace midilearn
