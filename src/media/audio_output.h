// WASAPI shared-mode output in the device's own mix format, with an adaptive jitter buffer and a
// single high-quality resampling stage (stream rate -> device rate) that also tracks the
// sender's clock with slow, inaudible ratio corrections. Mirroring audio keeps the buffer short
// for lip sync; music buffers deeper so Wi-Fi hiccups and resends never reach the speakers.
#pragma once

#include "common.h"
#include "media/resampler.h"

class AudioOutput {
public:
    enum class Profile { Mirror = 0, Music = 1 };

    ~AudioOutput() { Stop(); }
    // Reference-counted use by audio streams: the device is open only while a stream needs it
    // (an idle open stream would keep Windows from sleeping).
    void Acquire();
    void Release();
    void Start();  // idempotent; opens the default device on its own thread
    void Stop();
    // Stream sample rate and buffering profile; a change restarts the buffer.
    void Configure(int sampleRate, Profile profile);
    void Push(const float* interleaved, size_t frames);
    void PushSilence(size_t frames);  // keeps timing across a lost packet
    void Flush();
    void SetVolumeDb(float db);  // AirPlay volume: -144 = mute, -30..0 dB
    float VolumeDb() const { return volumeDb_.load(); }

private:
    void Run();
    void Render(float* out, size_t frames, uint32_t deviceChannels, int deviceRate);
    void ResetBuffer();
    void Write(const float* interleaved, size_t frames);

    static constexpr size_t kRingFrames = 4 * 48000;
    static constexpr size_t kTaps = SincResampler::kTaps;
    std::vector<float> ring_;  // interleaved stereo; the first kTaps frames are mirrored past the end
    std::atomic<size_t> write_{0}, read_{0};
    std::atomic<bool> flushReq_{false};
    std::atomic<int> inRate_{44100};
    std::atomic<int> profile_{0};
    std::atomic<bool> reconfig_{true};
    std::atomic<float> volumeDb_{0.0f};
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::mutex refMu_;
    int refs_ = 0;

    // Render-thread state.
    SincResampler rs_;
    int rsIn_ = 0, rsOut_ = 0;
    size_t base_ = 0;  // absolute index of the first frame of the interpolation window
    double frac_ = 0;
    double drift_ = 1.0;  // sender/DAC clock ratio correction
    double levelEma_ = -1;
    bool prebuffering_ = true;
    double targetSec_ = 0.06;
    float gain_ = 1.0f;
    uint64_t underruns_ = 0;
    double underrunAt_ = 0;  // when the buffer last ran dry (0 = not starved)
};
