#include "media/audio_decoder.h"

#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

namespace {

// AirPlay audioFormat bits (bit index -> codec/rate/bits/channels), e.g. 0x40000 (bit 18) is
// ALAC 44100/16/2 for audio-only streams and 0x1000000 (bit 24) AAC-ELD 44100/2 for mirroring.
struct FormatBit {
    int bit, ct, rate, bits, ch;
};
constexpr FormatBit kFormatBits[] = {
    {2, 1, 8000, 16, 1},   {3, 1, 8000, 16, 2},   {4, 1, 16000, 16, 1},  {5, 1, 16000, 16, 2},
    {6, 1, 24000, 16, 1},  {7, 1, 24000, 16, 2},  {8, 1, 32000, 16, 1},  {9, 1, 32000, 16, 2},
    {10, 1, 44100, 16, 1}, {11, 1, 44100, 16, 2}, {12, 1, 44100, 24, 1}, {13, 1, 44100, 24, 2},
    {14, 1, 48000, 16, 1}, {15, 1, 48000, 16, 2}, {16, 1, 48000, 24, 1}, {17, 1, 48000, 24, 2},
    {18, 2, 44100, 16, 2}, {19, 2, 44100, 24, 2}, {20, 2, 48000, 16, 2}, {21, 2, 48000, 24, 2},
    {22, 4, 44100, 16, 2}, {23, 4, 48000, 16, 2}, {24, 8, 44100, 16, 2}, {25, 8, 48000, 16, 2},
    {26, 8, 16000, 16, 1}, {27, 8, 24000, 16, 1}, {31, 8, 44100, 16, 1}, {32, 8, 48000, 16, 1},
};

}  // namespace

AudioFormat AudioFormat::FromSetup(int ct, uint64_t audioFormat, int spf, int sr) {
    AudioFormat f;
    f.ct = ct;
    f.spf = ct == 8 ? 480 : ct == 4 ? 1024 : 352;
    for (const auto& b : kFormatBits) {
        if (audioFormat == (uint64_t(1) << b.bit) && b.ct == ct) {
            f.sampleRate = b.rate;
            f.bits = b.bits;
            f.channels = b.ch;
        }
    }
    if (spf > 0) f.spf = spf;
    if (sr >= 8000 && sr <= 192000) f.sampleRate = sr;
    return f;
}

std::string AudioFormat::Describe() const {
    char b[96];
    if (ct == 2 || ct == 1)
        snprintf(b, sizeof(b), "%s %d-bit/%g kHz %s (lossless)", AudioDecoder::Name(ct), bits, sampleRate / 1000.0,
                 channels == 1 ? "mono" : "stereo");
    else
        snprintf(b, sizeof(b), "%s %g kHz %s", AudioDecoder::Name(ct), sampleRate / 1000.0,
                 channels == 1 ? "mono" : "stereo");
    return b;
}

const char* AudioDecoder::Name(int ct) {
    switch (ct) {
    case 1: return "LPCM";
    case 2: return "ALAC";
    case 4: return "AAC-LC";
    case 8: return "AAC-ELD";
    default: return "unknown";
    }
}

bool AudioDecoder::Open(const AudioFormat& fmt) {
    Close();
    fmt_ = fmt;
    errors_ = 0;
    if (fmt.ct == 1) return true;  // raw PCM needs no codec

    // ALAC "magic cookie" (ALACSpecificConfig in an 'alac' atom) for this stream's parameters.
    uint8_t alac[36] = {0x00, 0x00, 0x00, 0x24, 'a', 'l', 'a', 'c'};
    WrBE32(alac + 12, uint32_t(fmt.spf));
    alac[16] = 0;  // compatible version
    alac[17] = uint8_t(fmt.bits);
    alac[18] = 40;  // rice history mult
    alac[19] = 10;  // rice initial history
    alac[20] = 14;  // rice limit
    alac[21] = uint8_t(fmt.channels);
    WrBE16(alac + 22, 255);  // max run
    WrBE32(alac + 24, 0);    // max frame bytes (unknown)
    WrBE32(alac + 28, 0);    // average bit rate (unknown)
    WrBE32(alac + 32, uint32_t(fmt.sampleRate));
    static const uint8_t kAacLc44[2] = {0x12, 0x10};
    static const uint8_t kAacLc48[2] = {0x11, 0x90};
    static const uint8_t kAacEld[4] = {0xF8, 0xE8, 0x50, 0x00};

    AVCodecID id;
    const uint8_t* extra;
    int extraLen;
    switch (fmt.ct) {
    case 2: id = AV_CODEC_ID_ALAC; extra = alac; extraLen = sizeof(alac); break;
    case 4:
        id = AV_CODEC_ID_AAC;
        extra = fmt.sampleRate == 48000 ? kAacLc48 : kAacLc44;
        extraLen = 2;
        break;
    case 8: id = AV_CODEC_ID_AAC; extra = kAacEld; extraLen = sizeof(kAacEld); break;
    default: LOGE("audio: unsupported compression type %d", fmt.ct); return false;
    }
    const AVCodec* codec = avcodec_find_decoder(id);
    if (!codec) {
        LOGE("audio: FFmpeg lacks decoder for %s", Name(fmt.ct));
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->sample_rate = fmt.sampleRate;
    av_channel_layout_default(&ctx_->ch_layout, fmt.channels);
    ctx_->extradata = static_cast<uint8_t*>(av_mallocz(size_t(extraLen) + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(ctx_->extradata, extra, size_t(extraLen));
    ctx_->extradata_size = extraLen;
    int rc = avcodec_open2(ctx_, codec, nullptr);
    if (rc < 0) {
        LOGE("audio: avcodec_open2(%s) failed %d", Name(fmt.ct), rc);
        Close();
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    return true;
}

void AudioDecoder::Close() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
}

int AudioDecoder::Decode(const uint8_t* data, size_t n, std::vector<float>& out) {
    if (!n) return 0;
    if (fmt_.ct == 1) {
        size_t frames = n / 4;
        for (size_t i = 0; i < frames * 2; ++i) {
            int16_t s = int16_t(data[2 * i] | (data[2 * i + 1] << 8));
            out.push_back(float(s) / 32768.0f);
        }
        return int(frames);
    }
    if (!ctx_) return -1;
    // AAC-LC occasionally arrives with an ADTS header; strip it.
    if (fmt_.ct == 4 && n > 7 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0) {
        size_t hdr = (data[1] & 1) ? 7 : 9;
        if (n <= hdr) return 0;
        data += hdr;
        n -= hdr;
    }
    buf_.resize(n + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(buf_.data(), data, n);
    std::memset(buf_.data() + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt_->data = buf_.data();
    pkt_->size = int(n);
    int rc = avcodec_send_packet(ctx_, pkt_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        if (errors_++ < 10) LOGW("audio: %s decode error %d (first byte 0x%02x, %d bytes)", Name(fmt_.ct), rc, data[0], int(n));
        return -1;
    }
    int total = 0;
    while (avcodec_receive_frame(ctx_, frame_) >= 0) {
        int ns = frame_->nb_samples;
        int ch = frame_->ch_layout.nb_channels;
        size_t base = out.size();
        out.resize(base + size_t(ns) * 2);
        float* o = out.data() + base;
        auto get = [&](int c, int i) -> float {
            int cc = std::min(c, ch - 1);
            switch (frame_->format) {
            case AV_SAMPLE_FMT_FLTP: return reinterpret_cast<const float*>(frame_->data[cc])[i];
            case AV_SAMPLE_FMT_FLT: return reinterpret_cast<const float*>(frame_->data[0])[i * ch + cc];
            case AV_SAMPLE_FMT_S16P: return reinterpret_cast<const int16_t*>(frame_->data[cc])[i] / 32768.0f;
            case AV_SAMPLE_FMT_S16: return reinterpret_cast<const int16_t*>(frame_->data[0])[i * ch + cc] / 32768.0f;
            case AV_SAMPLE_FMT_S32P: return reinterpret_cast<const int32_t*>(frame_->data[cc])[i] / 2147483648.0f;
            case AV_SAMPLE_FMT_S32:
                return reinterpret_cast<const int32_t*>(frame_->data[0])[i * ch + cc] / 2147483648.0f;
            default: return 0.0f;
            }
        };
        for (int i = 0; i < ns; ++i) {
            o[2 * i] = get(0, i);
            o[2 * i + 1] = get(1, i);
        }
        total += ns;
        av_frame_unref(frame_);
    }
    return total;
}
