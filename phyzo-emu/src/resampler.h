// Stereo sample-rate converter from the machine's 44.1 kHz to the host rate: windowed-sinc (Kaiser) polyphase
// filter, 128 taps at 44.1 kHz input (wider when converting down), 512 phases with linear interpolation between
// them, passband to 20 kHz (scaled down with the output rate when converting down), about 90 dB rejection.
//
// Pull model: ask inputNeeded(n), push that many input frames, then produce(n). Output frame k is the input signal
// at input position k * inRate / outRate: the converter adds no delay of its own, it only needs to read
// lookahead() input frames beyond that position. Equal rates pass samples through unchanged.
#pragma once
#include <cstdint>
#include <vector>

class Resampler {
public:
    void setup(int inRate, int outRate);
    void reset();                                    // clears history; position back to 0
    bool bypass() const { return inRate_ == outRate_; }
    int lookahead() const { return bypass() ? 0 : half_; }   // input frames needed beyond the current position

    int64_t inputNeeded(int nOut) const;             // input frames to push before produce(nOut)
    int64_t available() const;                       // outputs that the input pushed so far allows
    void push(const float* left, const float* right, int n);
    void produce(float* left, float* right, int nOut);

    int64_t outputsProduced() const { return outCount_; }

private:
    int inRate_ = 44100, outRate_ = 44100;
    int taps_ = 0, half_ = 0;                        // kernel length in input frames, and half of it
    static constexpr int kPhases = 512;
    std::vector<float> table_;                       // (kPhases + 1) rows of taps_ coefficients
    // position of the next output: ip_ + frac_ / outRate_ input frames (exact rational stepping)
    int64_t ip_ = 0; int64_t frac_ = 0;
    int64_t outCount_ = 0;
    // input history, absolute frame index base_ at element 0 (frames before 0 are silence)
    std::vector<float> histL_, histR_;
    int64_t base_ = 0;
    int64_t pushed_ = 0;                             // absolute index of the next frame to be pushed
    float at(const std::vector<float>& h, int64_t i) const { return (i < base_ || i >= pushed_) ? 0.0f : h[size_t(i - base_)]; }
};
