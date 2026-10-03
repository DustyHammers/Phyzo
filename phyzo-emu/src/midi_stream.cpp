#include "midi_stream.h"

MidiParser::MidiParser(size_t maxMessage) : buf_(maxMessage < 3 ? 3 : maxMessage) {}

void MidiParser::reset() {
    len_ = 0; need_ = 0; running_ = 0;
    sysex_ = oversize_ = complete_ = rtOut_ = false;
}

int MidiParser::dataBytes(uint8_t s) {
    if (s < 0x80) return -2;
    if (s < 0xF0) return (s & 0xF0) == 0xC0 || (s & 0xF0) == 0xD0 ? 1 : 2;
    switch (s) {
    case 0xF0: return -1;
    case 0xF1: case 0xF3: return 1;                  // MTC quarter frame, song select
    case 0xF2: return 2;                             // song position
    case 0xF6: return 0;                             // tune request
    default: return s >= 0xF8 ? 0 : -2;              // real-time: no data; F4 F5 F7 are not message starts
    }
}

bool MidiParser::feed(uint8_t b) {
    rtOut_ = false;
    if (complete_) {                                 // the previous message was handed out
        complete_ = false;
        len_ = 0;
        if (running_) { buf_[0] = running_; len_ = 1; need_ = dataBytes(running_); }
    }
    if (b >= 0xF8) { rt_ = b; rtOut_ = true; return true; }   // real-time: on its own, never inside another message

    if (b & 0x80) {
        if (b == 0xF7) {
            if (!sysex_) return false;               // a stray end of SysEx
            sysex_ = false;
            if (oversize_) { oversize_ = false; ++dropped_; len_ = 0; return false; }
            buf_[len_++] = b;
            complete_ = true;
            return true;
        }
        // Any other status byte ends what came before; an incomplete message is dropped.
        if (sysex_ || (len_ > 0 && need_ > 0 && len_ > 1)) ++dropped_;
        sysex_ = oversize_ = false;
        len_ = 0; need_ = 0;
        const int d = dataBytes(b);
        if (d == -2) { running_ = 0; return false; } // F4 F5: undefined, ignored (they cancel running status)
        if (d == -1) { running_ = 0; sysex_ = true; buf_[0] = b; len_ = 1; return false; }
        running_ = b < 0xF0 ? b : 0;                 // system common cancels running status
        buf_[0] = b; len_ = 1; need_ = d;
        if (need_ == 0) { complete_ = true; return true; }
        return false;
    }

    // data byte
    if (sysex_) {
        if (len_ < buf_.size() - 1) buf_[len_++] = b; else oversize_ = true;
        return false;
    }
    if (len_ == 0 || need_ <= 0) return false;       // no status to belong to
    buf_[len_++] = b;
    if (--need_ == 0) { complete_ = true; return true; }
    return false;
}

// ------------------------------------------------------------------ MessageRing

MessageRing::MessageRing(size_t capacityBytes) {
    size_t c = 64;
    while (c < capacityBytes) c <<= 1;
    buf_.assign(c, 0);
    mask_ = c - 1;
}

bool MessageRing::push(const uint8_t* d, size_t n) {
    if (n == 0 || n > 0xFFFFFFFFu) return false;
    const uint64_t h = head_.load(std::memory_order_relaxed);
    const uint64_t t = tail_.load(std::memory_order_acquire);
    if ((h - t) + 4 + n > buf_.size()) return false;
    for (int i = 0; i < 4; ++i) buf_[(h + uint64_t(i)) & mask_] = uint8_t(uint32_t(n) >> (8 * i));
    for (size_t i = 0; i < n; ++i) buf_[(h + 4 + i) & mask_] = d[i];
    head_.store(h + 4 + n, std::memory_order_release);
    return true;
}

size_t MessageRing::peekSize() const {
    const uint64_t t = tail_.load(std::memory_order_relaxed);
    const uint64_t h = head_.load(std::memory_order_acquire);
    if (h == t) return 0;
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i) n |= uint32_t(buf_[(t + uint64_t(i)) & mask_]) << (8 * i);
    return n;
}
