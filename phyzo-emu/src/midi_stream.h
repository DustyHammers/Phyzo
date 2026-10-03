// MIDI byte streams as whole messages, for merging several sources into the emulated MIDI port and for passing the
// OS's MIDI output on. No allocation after construction; no locks.
//
// MidiParser: bytes in, complete messages out, each with its own status byte (running status expanded), SysEx whole
// (F0 ... F7). Real-time bytes (F8-FF) are messages of their own, also when they arrive inside another message.
// Incomplete messages (a status byte or SysEx cut short by another status byte, data bytes with no status) are
// dropped, so a merged stream only ever carries complete messages.
//
// MessageRing: single producer, single consumer, lock-free; holds whole messages (a length, then the bytes).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

class MidiParser {
public:
    explicit MidiParser(size_t maxMessage = size_t(1) << 20);
    // Feeds one byte. True when a message is complete: data()/size() until the next feed().
    bool feed(uint8_t b);
    const uint8_t* data() const { return rtOut_ ? &rt_ : buf_.data(); }
    size_t size() const { return rtOut_ ? 1 : len_; }
    void reset();
    uint64_t dropped() const { return dropped_; }    // incomplete or oversized messages discarded

    static int dataBytes(uint8_t status);            // data bytes after a status byte (-1: SysEx, -2: not a status)

private:
    std::vector<uint8_t> buf_;
    size_t len_ = 0;
    int need_ = 0;                                   // data bytes still missing for the message in buf_
    uint8_t running_ = 0;                            // channel status for running status (0: none)
    bool sysex_ = false, oversize_ = false, complete_ = false, rtOut_ = false;
    uint8_t rt_ = 0;
    uint64_t dropped_ = 0;
};

class MessageRing {
public:
    explicit MessageRing(size_t capacityBytes);      // rounded up to a power of two
    // Producer. False (and nothing written) when the whole message does not fit.
    bool push(const uint8_t* data, size_t size);
    // Consumer. Size of the next message, 0 when empty.
    size_t peekSize() const;
    // Consumer: removes the next message, handing its bytes to f(byte) in order.
    template <typename F> void pop(F&& f) {
        const uint64_t t = tail_.load(std::memory_order_relaxed);
        const size_t n = peekSize();
        if (!n) return;
        for (size_t i = 0; i < n; ++i) f(buf_[(t + 4 + i) & mask_]);
        tail_.store(t + 4 + n, std::memory_order_release);
    }
    size_t capacity() const { return buf_.size(); }
    size_t usedBytes() const { return size_t(head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)); }

private:
    std::vector<uint8_t> buf_;
    uint64_t mask_;
    std::atomic<uint64_t> head_{0}, tail_{0};
};
