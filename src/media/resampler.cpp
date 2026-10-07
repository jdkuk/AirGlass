#include "media/resampler.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

double BesselI0(double x) {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 64; ++k) {
        term *= q / (double(k) * double(k));
        sum += term;
        if (term < sum * 1e-17) break;
    }
    return sum;
}

}  // namespace

void SincResampler::Init(double inRate, double outRate) {
    constexpr double kBeta = 10.0;  // ~99 dB stopband attenuation
    const double half = kTaps / 2.0;
    // Kaiser transition width for this length/attenuation, in cycles per input sample.
    const double atten = kBeta / 0.1102 + 8.7;
    const double transition = (atten - 7.95) / (14.36 * (kTaps - 1));
    // Put the stopband edge at the lower Nyquist so nothing images or aliases into the output.
    const double nyquist = 0.5 * std::min(1.0, outRate / inRate);
    cutoff_ = nyquist - transition * 0.5;
    const double i0Beta = BesselI0(kBeta);

    table_.assign(size_t(kPhases + 1) * kTaps, 0.0f);
    std::vector<double> row(kTaps);
    for (int p = 0; p <= kPhases; ++p) {
        double phase = double(p) / kPhases;
        double sum = 0;
        for (int k = 0; k < kTaps; ++k) {
            double t = double(k) - (half - 1.0) - phase;  // offset from the output position
            double x = 2.0 * cutoff_ * t;
            double sinc = std::fabs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
            double r = t / half;
            double w = std::fabs(r) >= 1.0 ? 0.0 : BesselI0(kBeta * std::sqrt(1.0 - r * r)) / i0Beta;
            row[size_t(k)] = 2.0 * cutoff_ * sinc * w;
            sum += row[size_t(k)];
        }
        // Exact unity gain at DC for every phase.
        for (int k = 0; k < kTaps; ++k) table_[size_t(p) * kTaps + size_t(k)] = float(row[size_t(k)] / sum);
    }
}

void SincResampler::Process(const float* window, double frac, float& left, float& right) const {
    double fp = std::clamp(frac, 0.0, 1.0) * kPhases;
    int p = std::min(int(fp), kPhases - 1);
    float t = float(fp - double(p));
    const float* c0 = table_.data() + size_t(p) * kTaps;
    const float* c1 = c0 + kTaps;
    float l = 0.0f, r = 0.0f;
    for (int k = 0; k < kTaps; ++k) {
        float c = c0[k] + t * (c1[k] - c0[k]);
        l += c * window[2 * k];
        r += c * window[2 * k + 1];
    }
    left = l;
    right = r;
}
