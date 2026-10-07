// AirPlay screen-mirroring video stream: TCP data port, 128-byte packet headers,
// AES-128-CTR encrypted AVCC H.264 payloads.
#pragma once

#include "common.h"
#include "crypto/crypto.h"
#include "media/video_decoder.h"
#include "net/netutil.h"

class MirrorStream {
public:
    struct Callbacks {
        std::function<void(int w, int h)> onVideoSize;
        std::function<void()> onEnded;  // sender closed the stream (not called on Stop())
    };
    ~MirrorStream() { Stop(); }
    bool Start(const uint8_t aesKey[16], uint64_t streamConnectionId, FrameSink* sink, Callbacks cb);
    void Stop();
    uint16_t Port() const { return port_; }

private:
    void Run();
    void HandleCodec(const uint8_t* hdr, std::vector<uint8_t>& payload);
    void HandleVideo(std::vector<uint8_t>& payload);

    SOCKET listen_ = INVALID_SOCKET;
    SOCKET conn_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    crypto::AesCtr ctr_;
    FrameSink* sink_ = nullptr;
    VideoDecoder decoder_;
    std::vector<uint8_t> spsPps_;
    std::vector<uint8_t> joined_;
    bool pendingSpsPps_ = false;
    bool paused_ = false;
    Callbacks cb_;
    std::thread thread_;
    std::atomic<bool> stop_{false};

    // statistics
    double statStart_ = 0;
    uint64_t statFrames_ = 0, statBytes_ = 0;
    int badPackets_ = 0;
    double lastReportLog_ = 0;
};
