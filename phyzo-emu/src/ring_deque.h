// A FIFO with a preallocated ring (what the serial port needs of std::deque, without allocating as bytes flow:
// the audio thread feeds it). It grows, doubling, only when more than the reserved number of entries wait.
#pragma once
#include <cstddef>
#include <utility>
#include <vector>

template <typename T>
class RingDeque {
public:
    explicit RingDeque(size_t reserve = 64) { size_t c = 8; while (c < reserve) c <<= 1; buf_.resize(c); }
    bool empty() const { return n_ == 0; }
    size_t size() const { return n_; }
    const T& front() const { return buf_[head_]; }
    T& front() { return buf_[head_]; }
    const T& operator[](size_t i) const { return buf_[(head_ + i) & (buf_.size() - 1)]; }
    void push_back(const T& v) {
        if (n_ == buf_.size()) grow();
        buf_[(head_ + n_) & (buf_.size() - 1)] = v;
        ++n_;
    }
    template <typename... A> void emplace_back(A&&... a) { push_back(T(std::forward<A>(a)...)); }
    void pop_front() { head_ = (head_ + 1) & (buf_.size() - 1); --n_; }
    void clear() { head_ = 0; n_ = 0; }

private:
    void grow() {
        std::vector<T> b(buf_.size() * 2);
        for (size_t i = 0; i < n_; ++i) b[i] = (*this)[i];
        buf_.swap(b);
        head_ = 0;
    }
    std::vector<T> buf_;
    size_t head_ = 0, n_ = 0;
};
