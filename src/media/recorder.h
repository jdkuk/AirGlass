// AirGlass Pro: records the mirrored screen to MP4.
//
// The sender's H.264 stream is copied into the file (FFmpeg's MP4 muxer, no re-encode), so a
// recording costs almost no CPU and keeps the original quality. A recording has to begin on a
// keyframe, and senders send them rarely, so the recorder always keeps the packets since the last
// keyframe: Start() writes those first, timed before zero, and an MP4 edit list starts playback at
// the click.
// Video only: mirroring audio is not recorded.
#pragma once

#include "common.h"

#include <deque>

struct AVFormatContext;
struct AVStream;

class Recorder {
public:
    ~Recorder();

    // Every Annex-B access unit from the mirror thread (with SPS/PPS prepended on config changes).
    void Feed(const uint8_t* data, size_t n);
    // Size of the decoded picture, needed for the MP4 header (renderer thread).
    void SetFrameSize(int w, int h);

    // UI thread. Start returns false if there is nothing to record yet or the file can't be created.
    bool Start(const std::wstring& path, std::string* error);
    // Finishes the file; returns its path, or empty if nothing was written.
    std::wstring Stop();
    bool Active();
    double Seconds();  // length so far

    // Called (from the mirror thread) when a recording ends by itself, e.g. the device rotated.
    std::function<void(const std::wstring& path, const std::string& reason)> onAutoStop;

private:
    struct Packet {
        std::vector<uint8_t> data;
        int64_t us = 0;  // arrival time
        bool key = false;
    };
    bool OpenLocked(std::string* error);
    void WriteLocked(const Packet& p);
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
    int64_t t0_ = -1, lastPts_ = -1, lastUs_ = 0;
};
