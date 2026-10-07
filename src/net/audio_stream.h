// AirPlay audio stream: RTP over UDP (data + control ports), AES-128-CBC per packet,
// de-duplication/reordering by sequence number, retransmission requests, decode, and hand-off
// to the output.
#pragma once

#include <cstdio>

#include "common.h"
#include "crypto/crypto.h"
#include "media/audio_decoder.h"
#include "media/audio_output.h"
#include "net/netutil.h"

class AudioStream {
public:
    ~AudioStream() { Stop(); }
    // music: an audio-only (non-mirroring) session - deeper buffering and more patient resends.
    bool Start(const sockaddr_storage& peer, uint16_t peerControlPort, const AudioFormat& fmt, bool music,
               const uint8_t aesKey[16], const uint8_t aesIv[16], AudioOutput* out);
    void Stop();
    // FLUSH (pause/seek). nextSeq: first sequence number after the flush (RTP-Info), or -1.
    void Flush(int nextSeq = -1);
    uint16_t DataPort() const { return dataPort_; }
    uint16_t ControlPort() const { return ctrlPort_; }
    const AudioFormat& Format() const { return fmt_; }
    // Called on the audio thread when packets start/stop flowing (play/pause on the sender);
    // `at` is when it happened (NowSeconds), e.g. the end of the last packet for a stall.
    std::function<void(bool playing, double at)> onPlaying;

private:
    struct Slot {
        bool filled = false;
        uint16_t seq = 0;
        std::vector<uint8_t> payload;
    };
    void Run();
    void OnRtp(const uint8_t* p, size_t n);
    void Drain(bool timeUp);
    void DecodeAndPlay(std::vector<uint8_t>& payload);
    void RequestResend(uint16_t seq, uint16_t count);
    void SetPlaying(bool playing, double at);

    SOCKET data_ = INVALID_SOCKET, ctrl_ = INVALID_SOCKET;
    uint16_t dataPort_ = 0, ctrlPort_ = 0;
    sockaddr_storage ctrlPeer_{};
    bool haveCtrlPeer_ = false;
    AudioFormat fmt_;
    bool music_ = false;
    double gapWait_ = 0.08;
    crypto::AesCbc cbc_;
    uint8_t iv_[16]{};
    AudioDecoder dec_;
    AudioOutput* out_ = nullptr;
    bool acquired_ = false;
    std::vector<Slot> slots_;
    bool haveNext_ = false;
    uint16_t nextSeq_ = 0, highSeq_ = 0;
    double gapSince_ = 0, lastResend_ = 0;
    int resendTries_ = 0;
    uint16_t ctrlSeq_ = 0;
    std::vector<float> pcm_;
    std::atomic<bool> flushReq_{false};
    std::atomic<int> flushSeq_{-1};
    int dropBefore_ = -1;  // after a flush: ignore packets older than this sequence number
    bool playing_ = false;
    double lastPacket_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    uint64_t packets_ = 0, lost_ = 0, badFrames_ = 0, notAudio_ = 0;
    FILE* dump_ = nullptr;  // AIRGLASS_DEBUG_AUDIODUMP: decoded music as raw s16le (lossless check)
};
