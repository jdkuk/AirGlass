#include "media/audio_output.h"

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

namespace {
// Jitter-buffer targets (seconds of audio queued ahead of the output position).
// Mirroring starts at 90 ms: from 60 ms a real iPhone underran 4x in the first seconds (it settled
// at 120 ms); lower keeps lip sync tighter since video is shown as soon as it is decoded.
constexpr double kMirrorTarget = 0.09, kMirrorMax = 0.20;
constexpr double kMusicTarget = 0.23, kMusicMax = 0.50;  // RECORD reports ~250 ms latency

const GUID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const GUID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const GUID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const GUID kIidRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::wstring DefaultDeviceId(IMMDeviceEnumerator* en) {
    ComPtr<IMMDevice> dev;
    if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.GetAddressOf()))) return {};
    LPWSTR id = nullptr;
    std::wstring s;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
        s = id;
        CoTaskMemFree(id);
    }
    return s;
}

bool IsFloatFormat(const WAVEFORMATEX* f) {
    if (f->wBitsPerSample != 32) return false;
    if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    return f->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
           IsEqualGUID(reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f)->SubFormat, kSubtypeFloat);
}
}  // namespace

void AudioOutput::Acquire() {
    std::lock_guard<std::mutex> lk(refMu_);
    if (refs_++ == 0) Start();
}

void AudioOutput::Release() {
    std::lock_guard<std::mutex> lk(refMu_);
    if (refs_ > 0 && --refs_ == 0) {
        Stop();
        LOGI("audio: output closed (idle)");
    }
}

void AudioOutput::Start() {
    if (running_) return;
    ring_.assign((kRingFrames + kTaps) * 2, 0.0f);
    write_ = 0;
    read_ = 0;
    reconfig_ = true;
    stop_ = false;
    running_ = true;
    thread_ = std::thread(&AudioOutput::Run, this);
}

void AudioOutput::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void AudioOutput::Configure(int sampleRate, Profile profile) {
    if (sampleRate <= 0) sampleRate = 44100;
    bool changed = inRate_.exchange(sampleRate) != sampleRate;
    changed |= profile_.exchange(int(profile)) != int(profile);
    if (changed) reconfig_ = true;
}

void AudioOutput::Write(const float* in, size_t frames) {
    if (!running_ || ring_.empty()) return;
    size_t w = write_.load(std::memory_order_relaxed);
    size_t r = read_.load(std::memory_order_acquire);
    size_t space = kRingFrames - std::min(kRingFrames, w - r);
    frames = std::min(frames, space);
    for (size_t i = 0; i < frames; ++i) {
        size_t idx = (w + i) % kRingFrames;
        float l = in ? in[2 * i] : 0.0f, rr = in ? in[2 * i + 1] : 0.0f;
        ring_[idx * 2] = l;
        ring_[idx * 2 + 1] = rr;
        if (idx < kTaps) {  // mirror so every interpolation window is contiguous
            ring_[(idx + kRingFrames) * 2] = l;
            ring_[(idx + kRingFrames) * 2 + 1] = rr;
        }
    }
    write_.store(w + frames, std::memory_order_release);
}

void AudioOutput::Push(const float* in, size_t frames) { Write(in, frames); }

void AudioOutput::PushSilence(size_t frames) { Write(nullptr, frames); }

void AudioOutput::Flush() { flushReq_ = true; }

void AudioOutput::SetVolumeDb(float db) { volumeDb_ = db; }

void AudioOutput::ResetBuffer() {
    base_ = write_.load(std::memory_order_acquire);
    read_.store(base_, std::memory_order_release);
    frac_ = 0;
    prebuffering_ = true;
    levelEma_ = -1;
    underrunAt_ = 0;
}

void AudioOutput::Render(float* out, size_t frames, uint32_t ch, int devRate) {
    const bool music = profile_.load() == int(Profile::Music);
    if (reconfig_.exchange(false) || rsOut_ != devRate) {
        int in = inRate_.load();
        if (in != rsIn_ || devRate != rsOut_) {
            rs_.Init(in, devRate);
            rsIn_ = in;
            rsOut_ = devRate;
            LOGI("audio: %d Hz stream -> %d Hz device, windowed-sinc %d taps (flat to %.1f kHz)", in, devRate,
                 int(kTaps), (rs_.Cutoff() - 0.025) * in / 1000.0);
        }
        targetSec_ = music ? kMusicTarget : kMirrorTarget;
        drift_ = 1.0;
        underruns_ = 0;
        ResetBuffer();
    }
    if (flushReq_.exchange(false)) ResetBuffer();

    float db = volumeDb_.load();
    float targetGain = db <= -100.0f ? 0.0f : std::pow(10.0f, db / 20.0f);
    const double inRate = double(rsIn_);
    size_t avail = write_.load(std::memory_order_acquire) - base_;
    // Audio queued ahead of the output position, in input frames.
    double level = double(avail) - double(kTaps / 2) - frac_;

    if (prebuffering_) {
        if (avail >= kTaps && level >= targetSec_ * inRate) {
            prebuffering_ = false;
            // Ran dry and refilled quickly: that was network jitter, so buffer a little more.
            // (A long gap is a pause or the end of the stream, not a reason to add latency.)
            if (underrunAt_ > 0 && NowSeconds() - underrunAt_ < targetSec_ + 1.0) {
                ++underruns_;
                targetSec_ = std::min(music ? kMusicMax : kMirrorMax, targetSec_ + (music ? 0.05 : 0.015));
                if (underruns_ <= 20 || underruns_ % 50 == 0)
                    LOGI("audio: underrun #%llu, jitter target now %.0f ms", (unsigned long long)underruns_,
                         targetSec_ * 1000.0);
            }
            underrunAt_ = 0;
        } else {
            std::memset(out, 0, frames * ch * sizeof(float));
            return;
        }
    }
    // Hard catch-up if far too much is queued (e.g. after a stall on the sender).
    double excess = level - targetSec_ * inRate;
    if (excess > std::max(0.25, targetSec_) * inRate) {
        size_t skip = size_t(excess);
        base_ += skip;
        avail -= skip;
        level -= double(skip);
        levelEma_ = -1;
    }
    // Clock-drift tracking: steer the buffer toward its target with a slow, slew-limited ratio
    // change (music: at most 0.05 %/s, i.e. far below audible pitch change).
    double dt = double(frames) / double(devRate);
    double levelSec = level / inRate;
    if (levelEma_ < 0) levelEma_ = levelSec;
    levelEma_ += (levelSec - levelEma_) * std::min(1.0, dt / (music ? 2.0 : 0.5));
    double err = levelEma_ - targetSec_;
    double want = 1.0 + std::clamp(err / (music ? 20.0 : 2.0), music ? -0.002 : -0.003, music ? 0.002 : 0.003);
    double slew = (music ? 0.0005 : 0.003) * dt;
    drift_ += std::clamp(want - drift_, -slew, slew);
    const double step = inRate / double(devRate) * drift_;

    size_t i = 0;
    float g = gain_;
    float gStep = (targetGain - g) / float(std::max<size_t>(frames, 1));
    for (; i < frames; ++i) {
        if (avail < kTaps + 2) break;
        const float* win = ring_.data() + (base_ % kRingFrames) * 2;
        float l, r;
        rs_.Process(win, frac_, l, r);
        g += gStep;
        if (ch >= 2) {
            out[i * ch] = l * g;
            out[i * ch + 1] = r * g;
            for (uint32_t c = 2; c < ch; ++c) out[i * ch + c] = 0.0f;
        } else {
            out[i] = (l + r) * 0.5f * g;
        }
        frac_ += step;
        size_t adv = size_t(frac_);
        frac_ -= double(adv);
        base_ += adv;
        avail -= adv;
    }
    gain_ = targetGain;
    read_.store(base_, std::memory_order_release);
    if (i < frames) {
        std::memset(out + i * ch, 0, (frames - i) * ch * sizeof(float));
        prebuffering_ = true;
        if (underrunAt_ == 0) underrunAt_ = NowSeconds();
    }
}

void AudioOutput::Run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    Event evt;
    while (!stop_) {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        auto activate = [&]() {
            client.Reset();
            return SUCCEEDED(dev->Activate(kIidAudioClient, CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(client.GetAddressOf())));
        };
        if (FAILED(CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator,
                                    reinterpret_cast<void**>(en.GetAddressOf()))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, dev.GetAddressOf())) || !activate()) {
            LOGW("audio: no output device; retrying");
            for (int i = 0; i < 10 && !stop_; ++i) Sleep(100);
            continue;
        }
        std::wstring devId = DefaultDeviceId(en.Get());

        // Preferred: the engine's own mix format, so there is no second (Windows) resampler and
        // our high-quality one does the only rate conversion.
        int rate = 0;
        uint32_t ch = 2;
        bool native = false;
        HRESULT hr = E_FAIL;
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            if (IsFloatFormat(mix) && mix->nChannels >= 1) {
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 200000, 0, mix,
                                        nullptr);
                if (SUCCEEDED(hr)) {
                    native = true;
                    rate = int(mix->nSamplesPerSec);
                    ch = mix->nChannels;
                }
            }
            CoTaskMemFree(mix);
        }
        if (!native) {
            // Fallback: stream-rate float stereo, converted by Windows.
            rate = inRate_.load();
            WAVEFORMATEXTENSIBLE wf{};
            wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wf.Format.nChannels = 2;
            wf.Format.nSamplesPerSec = DWORD(rate);
            wf.Format.wBitsPerSample = 32;
            wf.Format.nBlockAlign = 8;
            wf.Format.nAvgBytesPerSec = DWORD(rate) * 8;
            wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
            wf.Samples.wValidBitsPerSample = 32;
            wf.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            wf.SubFormat = kSubtypeFloat;
            ch = 2;
            if (activate())
                hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                        200000, 0, &wf.Format, nullptr);
        }
        UINT32 bufFrames = 0;
        if (FAILED(hr) || FAILED(client->SetEventHandle(evt.Handle())) || FAILED(client->GetBufferSize(&bufFrames)) ||
            FAILED(client->GetService(kIidRenderClient, reinterpret_cast<void**>(render.GetAddressOf())))) {
            LOGE("audio: WASAPI init failed 0x%08lx", hr);
            for (int i = 0; i < 20 && !stop_; ++i) Sleep(100);
            continue;
        }
        LOGI("audio: output opened: %d Hz, %u ch, 32-bit float, %s (%u frame buffer)", rate, ch,
             native ? "device mix format" : "converted by Windows", bufFrames);
        reconfig_ = true;
        client->Start();
        double lastCheck = NowSeconds();
        while (!stop_) {
            WaitForSingleObject(evt.Handle(), 100);
            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) {
                LOGW("audio: device lost; reopening");
                break;
            }
            UINT32 n = bufFrames - pad;
            if (n) {
                BYTE* data = nullptr;
                if (FAILED(render->GetBuffer(n, &data))) break;
                Render(reinterpret_cast<float*>(data), n, ch, rate);
                render->ReleaseBuffer(n, 0);
            }
            double now = NowSeconds();
            if (now - lastCheck > 2.0) {
                lastCheck = now;
                if (DefaultDeviceId(en.Get()) != devId) {
                    LOGI("audio: default device changed; switching");
                    break;
                }
            }
        }
        client->Stop();
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    CoUninitialize();
}
