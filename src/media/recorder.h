// AirGlass Pro: records the mirrored screen, with its sound, to MP4.
//
// The sender's H.264 stream is copied into the file (FFmpeg's MP4 muxer, no re-encode), so a
// recording costs almost no CPU and keeps the original quality. A recording has to begin on a
// keyframe, and senders send them rarely, so the recorder always keeps the packets since the last
// keyframe: Start() writes those first, packed into the file's first instant (one 90 kHz tick each),
// so playback starts at the click showing the picture on screen then. No edit lists, no negative
// timestamps: some players ignore those and played the whole cache (minutes on a still screen).
// A device sends no frames while its screen is still, so each picture lasts until the next one: the
// newest packet is held back until the next arrives, and Stop() gives it a duration up to the moment
// of the stop, so a recording is as long as the wall-clock time it ran.
// Sound: when the session has a mirroring audio stream, its decoded PCM is encoded to AAC-LC (FFmpeg's
// built-in "aac" encoder) as a second track. Both tracks are timed by the same clock, the arrival time on
// this PC, so they stay in sync; gaps (sound that starts late, pauses, lost packets) become silence.
#pragma once

#include "common.h"

#include <deque>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

class Recorder {
public:
    ~Recorder();

    // Every Annex-B access unit from the mirror thread (with SPS/PPS prepended on config changes).
    void Feed(const uint8_t* data, size_t n);
    // Size of the decoded picture, needed for the MP4 header (renderer thread).
    void SetFrameSize(int w, int h);
    // Mirroring audio (audio threads): a stream started or stopped, and each decoded block (interleaved
    // stereo float). A recording gets a sound track if a stream is running when it starts.
    void AudioStarted(int sampleRate);
    void AudioStopped();
    void FeedAudio(const float* pcm, size_t frames);

    // UI thread. Start returns false if there is nothing to record yet or the file can't be created.
    bool Start(const std::wstring& path, std::string* error);
    // Finishes the file; returns its path, or empty if nothing was written.
    std::wstring Stop();
    bool Active();
    bool HasSound();   // the recording in progress has a sound track
    double Seconds();  // length so far (wall-clock time since Start)

    // Called (from the mirror thread) when a recording ends by itself, e.g. the device rotated.
    std::function<void(const std::wstring& path, const std::string& reason)> onAutoStop;

private:
    struct Packet {
        std::vector<uint8_t> data;
        int64_t us = 0;  // arrival time
        bool key = false;
    };
    bool OpenLocked(std::string* error);
    bool OpenAudioLocked();
    void WriteLocked(const Packet& p, int64_t pts);  // pts in 90 kHz ticks
    void WriteHeldLocked(int64_t until);             // the held packet, lasting until `until`
    void WritePacketLocked(AVPacket* pkt);
    void PadAudioLocked(int64_t samples);
    void EncodeAudioLocked(bool flush);
    void CloseAudioLocked();
    std::wstring CloseLocked();

    std::mutex mu_;
    std::deque<Packet> gop_;     // packets since (and including) the last keyframe
    size_t gopBytes_ = 0;
    bool gopValid_ = false;      // false after the cache overflowed until the next keyframe
    std::vector<uint8_t> config_;  // SPS/PPS of the current stream (Annex-B)
    int w_ = 0, h_ = 0;
    AVFormatContext* fmt_ = nullptr;
    AVStream* st_ = nullptr;
    std::wstring path_;
    std::vector<uint8_t> fileConfig_;  // SPS/PPS the open file was started with
    int64_t t0_ = -1, lastPts_ = -1;
    AVPacket* held_ = nullptr;  // the newest video packet, written once its duration is known
    // Sound
    int audioStreams_ = 0, audioRate_ = 0;  // running mirroring audio streams and their sample rate
    AVStream* ast_ = nullptr;
    AVCodecContext* aenc_ = nullptr;
    AVFrame* aframe_ = nullptr;
    int trackRate_ = 0;
    std::vector<float> apending_;  // interleaved samples waiting for a whole encoder frame
    int64_t aSamples_ = 0;         // samples placed on the track so far: the track is at t0_ + aSamples_ / rate
    int64_t aEncoded_ = 0;         // samples handed to the encoder (pts of the next frame)
    double aDrift_ = 0;            // smoothed offset of arriving sound from the track position (samples)
    bool aWarned_ = false;
};
