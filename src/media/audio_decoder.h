// AirPlay audio decoding: ct=1 LPCM, ct=2 ALAC (Apple Lossless), ct=4 AAC-LC, ct=8 AAC-ELD.
#pragma once

#include "common.h"

struct AVCodecContext;
struct AVPacket;
struct AVFrame;

// Stream format from the SETUP request: compression type plus the AirPlay audioFormat bit.
struct AudioFormat {
    int ct = 0;  // 1 PCM, 2 ALAC, 4 AAC-LC, 8 AAC-ELD
    int sampleRate = 44100;
    int bits = 16;
    int channels = 2;
    int spf = 352;  // samples per frame

    // Builds the format from SETUP fields (ct, audioFormat bitmask, spf, sr; 0 = absent).
    static AudioFormat FromSetup(int ct, uint64_t audioFormat, int spf, int sr);
    bool Lossless() const { return ct == 1 || ct == 2; }
    std::string Describe() const;  // e.g. "ALAC 16-bit/44.1 kHz stereo (lossless)"
};

class AudioDecoder {
public:
    ~AudioDecoder() { Close(); }
    bool Open(const AudioFormat& fmt);
    void Close();
    // Decodes one frame; appends interleaved stereo float samples (exact for 16/24-bit input).
    // Returns frames appended, 0 when the packet held no audio, -1 on a decode error.
    int Decode(const uint8_t* data, size_t n, std::vector<float>& out);
    static const char* Name(int ct);

private:
    AudioFormat fmt_;
    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::vector<uint8_t> buf_;
    int errors_ = 0;
};
