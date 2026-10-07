// H.264 decoding through FFmpeg with D3D11VA hardware acceleration (software fallback).
#pragma once

#include "media/frame_sink.h"

struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct AVBufferRef;

class VideoDecoder {
public:
    ~VideoDecoder() { Close(); }
    bool Open(FrameSink* sink);
    void Close();
    // Decodes one Annex-B access unit and forwards every produced frame to the sink.
    int Decode(const uint8_t* data, size_t n);
    bool Hardware() const { return lastWasHw_; }

    std::function<void(int w, int h)> onSize;

private:
    void Output(AVFrame* f);
    FrameSink* sink_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVBufferRef* hwDev_ = nullptr;
    std::vector<uint8_t> buf_;
    int lastW_ = 0, lastH_ = 0;
    bool lastWasHw_ = false;
    bool loggedFormat_ = false;
    int errors_ = 0;
};
