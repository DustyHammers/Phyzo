#include "panel_model.h"
#include <cstdio>

void PanelModel::log(PanelEvent e) { if (keepLog) transcript.push_back(std::move(e)); }
void PanelModel::history(DisplayChange d) { if (keepLog) displayHistory.push_back(d); }

void PanelModel::setFont(const uint8_t* font) {
    // Reverse map segment pattern -> character, preferring the most readable glyph.
    // '?' first so the "unknown" pattern decodes as '?'; digits before letters (0 vs O).
    static const char* pref =
        "? 0123456789AbCdEFGHIJKLMnoPqrStUVWXyZabcdefghijklmnopqrstuvwxyzBDNOQRTY-_=[]()<>/\\|'\"^~*+,.:;!@#$%&`{}";
    segToChar_.clear();
    for (const char* p = pref; *p; ++p) {
        uint8_t code = font[uint8_t(*p) & 0x7f];
        if (!segToChar_.count(code)) segToChar_[code] = *p;
    }
}

std::string PanelModel::decodeSeg(uint8_t s) const {
    auto it = segToChar_.find(s);
    return std::string(1, it == segToChar_.end() ? '#' : it->second);
}

int PanelModel::expectedData(uint8_t st) const {
    if (st == 0x9d) return 2;                      // LED flash: code, rate
    if (st >= 0x90 && st <= 0x9f) return 1;        // LED state / display digit / dots
    if (st == 0xf0) return 1;                      // F0 33
    if (st == 0x80 || st == 0x81) return 1;
    if ((st & 0xf0) == 0xb0) return 2;
    return 0;                                      // F1-F4 and anything unknown
}

void PanelModel::onOsByte(uint8_t b, uint64_t cycle) {
    if (b & 0x80) {
        if (!msg_.empty()) {
            if (keepLog) log({msgStart_, true, msg_, "incomplete message"});
            ++unknownMessages;
            msg_.clear();
        }
        status_ = b;
        msg_.push_back(b);
        msgStart_ = cycle;
        if (expectedData(b) == 0) complete();
        return;
    }
    if (msg_.empty()) {
        if (status_ >= 0x90 && status_ <= 0x9f) {     // running status
            msg_.push_back(status_);
            msgStart_ = cycle;
        } else {
            if (keepLog) log({cycle, true, {b}, "orphan data byte"});
            ++unknownMessages;
            return;
        }
    }
    msg_.push_back(b);
    if (int(msg_.size()) - 1 >= expectedData(status_)) complete();
}

void PanelModel::complete() {
    const uint8_t st = msg_[0];
    char buf[160];
    std::string d;
    if (st == 0xf0) {
        ++helloCount;
        d = (msg_.size() > 1 && msg_[1] == 0x33) ? "hello" : "F0 with unexpected data";
        if (replyHello && msg_.size() > 1 && msg_[1] == 0x33 && sendToOs) {
            uint64_t when = msgStart_ + uint64_t(helloDelayMs * cyclesPerMs);
            if (keepLog) log({msgStart_, true, msg_, d});
            msg_.clear();
            const uint8_t reply[2] = {0xf0, 0x33};
            sendToOs(reply, 2, when);
            if (keepLog) log({when, false, {0xf0, 0x33}, "hello reply (panel model)"});
            return;
        }
    } else if (st == 0xf2) {
        ++f2Count;
        d = "F2 handshake";
        if (replyF2 >= 0 && sendToOs) {
            if (keepLog) log({msgStart_, true, msg_, d});
            msg_.clear();
            uint64_t when = msgStart_ + uint64_t(helloDelayMs * cyclesPerMs);
            const uint8_t reply[2] = {0xf2, uint8_t(replyF2 & 0x7f)};
            sendToOs(reply, 2, when);
            if (keepLog) log({when, false, {0xf2, uint8_t(replyF2 & 0x7f)}, "F2 reply (panel model)"});
            return;
        }
    } else if (st == 0xf3) {
        ++f3Count; d = "F3";
    } else if (st == 0xf4) {
        ++f4Count;
        d = "F4 request: report all analog controls";
        if ((answerF4 || answerF4Value >= 0) && sendToOs) {
            if (keepLog) log({msgStart_, true, msg_, d});
            msg_.clear();
            std::vector<uint8_t> out;
            for (uint8_t i = 0; i < kControls; ++i) {      // one message per control, in cc order
                const int v = (answerF4Value >= 0 ? answerF4Value : controls[i]) & 0x3ff;
                out.push_back(uint8_t(0xb0 | ((v >> 7) & 7)));
                out.push_back(i);
                out.push_back(uint8_t(v & 0x7f));
            }
            uint64_t when = msgStart_ + uint64_t(helloDelayMs * cyclesPerMs);
            sendToOs(out.data(), out.size(), when);
            if (answerF4Value >= 0) std::snprintf(buf, sizeof buf, "26 control reports, value %d (panel model)", answerF4Value & 0x3ff);
            else std::snprintf(buf, sizeof buf, "26 control reports, panel positions (panel model)");
            if (keepLog) log({when, false, out, buf});
            return;
        }
    } else if (st >= 0x93 && st <= 0x96 && msg_.size() > 1) {
        int pos = 0x96 - st;
        raw_[pos] = msg_[1];
        std::string t;
        for (int i = 0; i < 4; ++i) t += decodeSeg(raw_[i]);
        if (keepLog) {
            std::snprintf(buf, sizeof buf, "digit %d = %02X '%s' -> \"%s\"", pos, msg_[1],
                          decodeSeg(msg_[1]).c_str(), t.c_str());
            d = buf;
        }
        if (t != text_ || displayHistory.empty() || displayHistory.back().raw != raw_) {
            text_ = t;
            history({msgStart_, t, raw_, dots_});
        }
    } else if (st == 0x97 && msg_.size() > 1) {
        dots_ = msg_[1];
        if (keepLog) {
            std::snprintf(buf, sizeof buf, "dots = %02X%s%s", dots_, (dots_ & 8) ? " colon" : "",
                          (dots_ & 4) ? " point" : "");
            d = buf;
        }
        history({msgStart_, text_, raw_, dots_});
    } else if (st == 0x9d && msg_.size() > 2) {
        ledState[msg_[1]] = 2;
        if (onLed) onLed(st, msg_[1], msg_[2], msgStart_);
        if (keepLog) { std::snprintf(buf, sizeof buf, "LED %02X flashing, rate %02X", msg_[1], msg_[2]); d = buf; }
    } else if (st >= 0x90 && st <= 0x9f && msg_.size() > 1) {
        ledState[msg_[1]] = st - 0x90;
        if (onLed) onLed(st, msg_[1], 0, msgStart_);
        if (keepLog) { std::snprintf(buf, sizeof buf, "LED %02X state %d", msg_[1], st - 0x90); d = buf; }
    } else {
        ++unknownMessages;
        if (keepLog) d = "unknown";
    }
    if (keepLog) log({msgStart_, true, msg_, d});
    msg_.clear();
}

void PanelModel::onResetPin(bool asserted, uint64_t cycle) {
    if (asserted == resetAsserted_) return;
    resetAsserted_ = asserted;
    if (!asserted) ++resetPulses;
    if (keepLog) log({cycle, true, {}, asserted ? "OP0 set (panel reset asserted)" : "OP0 cleared (panel reset released)"});
}

void PanelModel::moveControl(int cc, int raw, uint64_t cycle) {
    if (cc < 0 || cc >= kControls) return;
    const int v = raw < 0 ? 0 : raw > 1023 ? 1023 : raw;
    controls[size_t(cc)] = uint16_t(v);
    const uint8_t m[3] = {uint8_t(0xb0 | ((v >> 7) & 7)), uint8_t(cc), uint8_t(v & 0x7f)};
    inject(m, 3, cycle, "control moved");
}

void PanelModel::inject(const std::vector<uint8_t>& bytes, uint64_t cycle, const std::string& note) {
    if (keepLog) log({cycle, false, bytes, note});
    if (sendToOs) sendToOs(bytes.data(), bytes.size(), cycle);
}

void PanelModel::inject(const uint8_t* bytes, size_t n, uint64_t cycle, const char* note) {
    if (keepLog) log({cycle, false, std::vector<uint8_t>(bytes, bytes + n), note});
    if (sendToOs) sendToOs(bytes, n, cycle);
}

// ---------------------------------------------------------------- state (plugin projects)
#include "state_io.h"

void PanelModel::save(StateWriter& w) const {
    w.put(raw_); w.put(dots_); w.str(text_); w.put(status_); w.vec(msg_); w.put(msgStart_); w.put(resetAsserted_);
    w.map(ledState);
    w.put(controls);                                   // added after v0.3.0; older states end here
}
void PanelModel::load(StateReader& r) {
    r.get(raw_); r.get(dots_); r.str(text_); r.get(status_); r.vec(msg_, 64); r.get(msgStart_); r.get(resetAsserted_);
    r.map(ledState);
    if (!r.atEnd()) r.get(controls);
    else controls = kFreshControls;
}
