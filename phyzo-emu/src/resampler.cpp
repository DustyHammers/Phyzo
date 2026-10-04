#include "resampler.h"
#include <algorithm>
#include <cmath>

namespace {
double besselI0(double x) {             // power series; converges quickly for the beta used here
    double sum = 1, term = 1, q = x * x / 4;
    for (int k = 1; k < 64; ++k) { term *= q / (double(k) * k); sum += term; if (term < sum * 1e-17) break; }
    return sum;
}
}

void Resampler::setup(int inRate, int outRate) {
    inRate_ = std::max(1, inRate);
    outRate_ = std::max(1, outRate);
    table_.clear();
    if (bypass()) { taps_ = half_ = 0; reset(); return; }
    const double scale = std::min(1.0, double(outRate_) / inRate_);          // < 1 when converting down
    taps_ = int(std::ceil(128.0 / scale / 2.0)) * 2;
    half_ = taps_ / 2;
    const double passEdge = 20000.0 * scale, stopEdge = 0.5 * std::min(inRate_, outRate_);
    const double fc = 0.5 * (passEdge + stopEdge) / inRate_;                  // cutoff, cycles per input frame
    const double beta = 8.96, i0b = besselI0(beta);                          // Kaiser window for about 90 dB
    table_.assign(size_t(kPhases + 1) * size_t(taps_), 0.0f);
    std::vector<double> row(static_cast<size_t>(taps_));
    for (int ph = 0; ph <= kPhases; ++ph) {
        const double t = double(ph) / kPhases;
        double sum = 0;
        for (int j = 0; j < taps_; ++j) {
            const double d = t + half_ - 1 - j;                              // distance from the output position
            const double x = d / half_;
            const double w = std::fabs(x) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - x * x)) / i0b;
            const double a = 2.0 * fc * d * M_PI;
            const double s = std::fabs(a) < 1e-12 ? 1.0 : std::sin(a) / a;
            row[size_t(j)] = 2.0 * fc * s * w;
            sum += row[size_t(j)];
        }
        for (int j = 0; j < taps_; ++j) table_[size_t(ph) * size_t(taps_) + size_t(j)] = float(row[size_t(j)] / sum);   // unity gain at DC
    }
    reset();
}

void Resampler::reset() {
    ip_ = frac_ = outCount_ = 0;
    pushed_ = 0;
    base_ = -int64_t(taps_);                                                  // silence before the first frame
    histL_.assign(size_t(taps_), 0.0f);
    histR_.assign(size_t(taps_), 0.0f);
    histL_.reserve(size_t(taps_) + 65536);                                     // no reallocation in normal use
    histR_.reserve(size_t(taps_) + 65536);
}

int64_t Resampler::inputNeeded(int nOut) const {
    if (nOut <= 0) return 0;
    int64_t last;
    if (bypass()) last = ip_ + nOut - 1;
    else last = ip_ + (frac_ + int64_t(nOut - 1) * inRate_) / outRate_ + half_;
    return std::max<int64_t>(0, last + 1 - pushed_);
}

int64_t Resampler::available() const {
    if (bypass()) return std::max<int64_t>(0, pushed_ - ip_);
    const int64_t m = pushed_ - 1 - half_ - ip_;           // furthest step from ip_ still covered by pushed input
    if (m < 0) return 0;
    int64_t k = ((m + 1) * outRate_ - 1 - frac_) / inRate_ + 1;
    while (k > 0 && inputNeeded(int(k)) > 0) --k;          // (exact check; the estimate is at most one too high)
    return k;
}

void Resampler::push(const float* left, const float* right, int n) {
    histL_.insert(histL_.end(), left, left + n);
    histR_.insert(histR_.end(), right, right + n);
    pushed_ += n;
}

void Resampler::produce(float* left, float* right, int nOut) {
    if (bypass()) {
        for (int k = 0; k < nOut; ++k, ++ip_) { left[k] = at(histL_, ip_); right[k] = at(histR_, ip_); }
    } else {
        for (int k = 0; k < nOut; ++k) {
            const int64_t first = ip_ - half_ + 1;
            if (first < base_ || ip_ + half_ >= pushed_) { left[k] = right[k] = 0.0f; }   // not enough input pushed
            else {
                const double pos = double(frac_) / outRate_ * kPhases;
                const int ph = std::min(kPhases - 1, int(pos));
                const float a = float(pos - ph), b = 1.0f - a;
                const float* c0 = &table_[size_t(ph) * size_t(taps_)];
                const float* c1 = c0 + taps_;
                const float* xl = &histL_[size_t(first - base_)];
                const float* xr = &histR_[size_t(first - base_)];
                float sl = 0, sr = 0;
                for (int j = 0; j < taps_; ++j) {
                    const float c = c0[j] * b + c1[j] * a;
                    sl += xl[j] * c; sr += xr[j] * c;
                }
                left[k] = sl; right[k] = sr;
            }
            frac_ += inRate_;
            ip_ += frac_ / outRate_;
            frac_ %= outRate_;
        }
    }
    outCount_ += nOut;
    // drop history no longer reachable (keep it contiguous; trim in large steps)
    const int64_t keepFrom = ip_ - half_ - 1;
    if (keepFrom - base_ > 8192) {
        const size_t drop = size_t(keepFrom - base_);
        histL_.erase(histL_.begin(), histL_.begin() + long(drop));
        histR_.erase(histR_.begin(), histR_.begin() + long(drop));
        base_ += int64_t(drop);
    }
}
