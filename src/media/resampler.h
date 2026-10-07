// High-quality polyphase windowed-sinc interpolator for stereo float audio. One instance converts
// the stream rate to the device rate and also absorbs sender/DAC clock drift, so the ratio may
// vary continuously at run time.
//   128 taps, Kaiser beta 10 (~100 dB stopband), 1024 phases with linear phase interpolation;
//   passband flat (<0.001 dB) to ~19.8 kHz for 44.1 kHz input.
#pragma once

#include <vector>

class SincResampler {
public:
    static constexpr int kTaps = 128;  // window length in input frames (even)
    static constexpr int kPhases = 1024;

    // Designs the filter for a nominal inRate -> outRate conversion.
    void Init(double inRate, double outRate);
    bool Ready() const { return !table_.empty(); }
    // window: kTaps interleaved stereo input frames. frac (0..1) is the output position between
    // window frames kTaps/2-1 and kTaps/2.
    void Process(const float* window, double frac, float& left, float& right) const;
    double Cutoff() const { return cutoff_; }  // -6 dB point, cycles per input sample

private:
    std::vector<float> table_;  // (kPhases + 1) rows of kTaps coefficients
    double cutoff_ = 0;
};
