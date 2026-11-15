#include "media/recorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

namespace {

constexpr size_t kMaxGopBytes = size_t(256) << 20;  // cache cap; beyond it we wait for the next keyframe
constexpr int kMirrorAudioRate = 44100;  // iOS screen mirroring's sound: the track's rate before any sound starts
// Sound timing (seconds). Arriving sound is placed on the track by its arrival time, like the video. Jitter
// within kDriftLimit is absorbed; a smoothed offset beyond it (the sender's clock running slightly fast or
// slow) is corrected by a little silence or a dropped block; a jump beyond kGap (sound that starts late,
// a pause) is filled with silence at once. With no sound at all for kSilenceAfter, the video fills the track
// with silence, so the encoder never has a long gap to catch up on.
constexpr double kDriftLimit = 0.04, kGap = 0.2, kSilenceAfter = 0.5, kSilenceLead = 0.3;

std::string AvError(int rc) {
    char buf[128] = {};
    av_strerror(rc, buf, sizeof(buf));
    return buf;
}

int64_t NowUs() { return int64_t(NowSeconds() * 1e6); }

// Walks the Annex-B start codes; calls fn(type, nal_start, nal_end_including_start_code_of_next).
template <typename Fn>
void ForEachNal(const uint8_t* d, size_t n, Fn fn) {
    size_t i = 0, start = SIZE_MAX, sc = 0;
    while (i + 3 <= n) {
        if (d[i] == 0 && d[i + 1] == 0 && (d[i + 2] == 1 || (i + 3 < n && d[i + 2] == 0 && d[i + 3] == 1))) {
            size_t len = d[i + 2] == 1 ? 3 : 4;
            if (start != SIZE_MAX) fn(d[start] & 0x1f, d + sc, d + i);
            sc = i;
            i += len;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) fn(d[start] & 0x1f, d + sc, d + n);
}

}  // namespace

Recorder::~Recorder() {
    std::lock_guard<std::mutex> lk(mu_);
    CloseLocked();
}

void Recorder::SetFrameSize(int w, int h) {
    std::lock_guard<std::mutex> lk(mu_);
    w_ = w;
    h_ = h;
}

void Recorder::Feed(const uint8_t* data, size_t n) {
    if (!n) return;
    Packet p;
    p.us = NowUs();
    std::vector<uint8_t> config;
    ForEachNal(data, n, [&](int type, const uint8_t* b, const uint8_t* e) {
        if (type == 5) p.key = true;
        if (type == 7 || type == 8) config.insert(config.end(), b, e);
    });
    std::wstring stoppedPath;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!config.empty() && config != config_) {
            config_ = config;
            // A new picture size (the device rotated): this MP4 can't change size midway, so finish it.
            if (fmt_ && config_ != fileConfig_) stoppedPath = CloseLocked();
        }
        if (p.key) {
            gop_.clear();
            gopBytes_ = 0;
            gopValid_ = true;
        }
        if (gopValid_) {
            p.data.assign(data, data + n);
            gopBytes_ += n;
            if (gopBytes_ > kMaxGopBytes) {
                gop_.clear();
                gopBytes_ = 0;
                gopValid_ = false;
            } else {
                gop_.push_back(p);
                if (fmt_) WriteLocked(gop_.back(), (p.us - t0_) * 9 / 100);
            }
        } else if (fmt_) {
            p.data.assign(data, data + n);
            WriteLocked(p, (p.us - t0_) * 9 / 100);
        }
    }
    if (!stoppedPath.empty() && onAutoStop) onAutoStop(stoppedPath, "the device rotated");
}

void Recorder::AudioStarted(int sampleRate) {
    std::lock_guard<std::mutex> lk(mu_);
    ++audioStreams_;
    audioRate_ = sampleRate;
    if (fmt_ && !aenc_) LOGI("record: sound started, but this recording has no sound track");
    else if (fmt_ && sampleRate == trackRate_) LOGI("record: sound started; it joins the recording in sync");
}

void Recorder::AudioStopped() {
    std::lock_guard<std::mutex> lk(mu_);
    if (audioStreams_ > 0) --audioStreams_;
}

void Recorder::FeedAudio(const float* pcm, size_t frames) {
    int64_t now = NowUs();
    std::lock_guard<std::mutex> lk(mu_);
    if (!aenc_ || !frames) return;
    if (audioRate_ != trackRate_) {  // a new stream at another rate: not on this track
        if (!aWarned_) LOGW("record: sound changed to %d Hz; the rest of this recording is silent", audioRate_);
        aWarned_ = true;
        return;
    }
    // Where the block belongs: it ends about now, by the same arrival clock as the video.
    int64_t at = (now - t0_) * trackRate_ / 1000000 - int64_t(frames);
    double lag = double(at - aSamples_);
    if (lag > kGap * trackRate_) {
        PadAudioLocked(int64_t(lag));
        aDrift_ = 0;
    } else {
        aDrift_ = aDrift_ * 0.98 + lag * 0.02;
        if (aDrift_ > kDriftLimit * trackRate_) {
            PadAudioLocked(int64_t(aDrift_));
            aDrift_ = 0;
        } else if (aDrift_ < -kDriftLimit * trackRate_) {
            aDrift_ += double(frames);  // ahead of the clock: leave this block out
            return;
        }
    }
    apending_.insert(apending_.end(), pcm, pcm + frames * 2);
    aSamples_ += int64_t(frames);
    EncodeAudioLocked(false);
}

bool Recorder::Start(const std::wstring& path, std::string* error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (fmt_) return true;
    path_ = path;
    return OpenLocked(error);
}

bool Recorder::OpenLocked(std::string* error) {
    if (!gopValid_ || gop_.empty() || config_.empty() || w_ <= 0 || h_ <= 0) {
        *error = "no video yet: start mirroring first";
        return false;
    }
    std::string file = WideToUtf8(path_);
    int rc = avformat_alloc_output_context2(&fmt_, nullptr, "mp4", file.c_str());
    if (rc < 0 || !fmt_) {
        *error = "mp4 muxer: " + AvError(rc);
        fmt_ = nullptr;
        return false;
    }
    st_ = avformat_new_stream(fmt_, nullptr);
    st_->time_base = AVRational{1, 90000};
    AVCodecParameters* par = st_->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id = AV_CODEC_ID_H264;
    par->width = w_;
    par->height = h_;
    par->extradata = static_cast<uint8_t*>(av_mallocz(config_.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    std::memcpy(par->extradata, config_.data(), config_.size());
    par->extradata_size = int(config_.size());
    // A sound track even before any sound: it is silent until the sound starts, then joins in sync.
    OpenAudioLocked(audioStreams_ > 0 && audioRate_ > 0 ? audioRate_ : kMirrorAudioRate);
    fmt_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;  // nothing is negative; never shift a track
    if ((rc = avio_open(&fmt_->pb, file.c_str(), AVIO_FLAG_WRITE)) < 0) {
        *error = "can't create the file: " + AvError(rc);
        CloseAudioLocked();
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    av_dict_set(&opts, "use_editlist", "0", 0);  // both tracks start at zero
    rc = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        *error = "mp4 header: " + AvError(rc);
        CloseAudioLocked();
        avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        _wremove(path_.c_str());
        return false;
    }
    fileConfig_ = config_;
    t0_ = NowUs();  // the click
    lastPts_ = INT64_MIN;
    // The encoder's first output is its priming (initial_padding samples, decoded but not ours): numbering the
    // input from there puts the first packet at zero, so the sound needs no edit list either. aSamples_ is
    // then the track's position in time, as before.
    aSamples_ = aEncoded_ = aenc_ ? aenc_->initial_padding : 0;
    aDrift_ = 0;
    aWarned_ = false;
    apending_.clear();
    int64_t tick = 0;  // the cache: the decoder runs through it in the first instant and shows its last picture
    for (const Packet& p : gop_) WriteLocked(p, tick++);
    std::string sound = ", no sound track";
    if (aenc_) sound = ", sound " + std::to_string(trackRate_) + " Hz AAC" + (audioStreams_ > 0 ? "" : " (silent so far)");
    LOGI("record: started %dx%d%s -> %s (%zu cached packets)", w_, h_, sound.c_str(), file.c_str(), gop_.size());
    return true;
}

// The sound track: FFmpeg's own AAC-LC encoder (LGPL, part of every FFmpeg build), stereo, 160 kb/s.
bool Recorder::OpenAudioLocked(int rate) {
    const AVCodec* codec = avcodec_find_encoder_by_name("aac");
    if (!codec) {
        LOGW("record: FFmpeg has no AAC encoder; recording without sound");
        return false;
    }
    aenc_ = avcodec_alloc_context3(codec);
    aenc_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aenc_->sample_rate = rate;
    av_channel_layout_default(&aenc_->ch_layout, 2);
    aenc_->bit_rate = 160000;
    aenc_->time_base = AVRational{1, rate};
    if (fmt_->oformat->flags & AVFMT_GLOBALHEADER) aenc_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int rc = avcodec_open2(aenc_, codec, nullptr);
    if (rc < 0) {
        LOGW("record: AAC encoder: %s; recording without sound", AvError(rc).c_str());
        CloseAudioLocked();
        return false;
    }
    aframe_ = av_frame_alloc();
    if (aframe_) {
        aframe_->format = AV_SAMPLE_FMT_FLTP;
        aframe_->sample_rate = rate;
        av_channel_layout_copy(&aframe_->ch_layout, &aenc_->ch_layout);
        aframe_->nb_samples = aenc_->frame_size;
    }
    if (!aframe_ || av_frame_get_buffer(aframe_, 0) < 0 || !(ast_ = avformat_new_stream(fmt_, nullptr))) {
        LOGW("record: out of memory for the sound track; recording without sound");
        CloseAudioLocked();
        return false;
    }
    ast_->time_base = aenc_->time_base;
    avcodec_parameters_from_context(ast_->codecpar, aenc_);
    trackRate_ = rate;
    return true;
}

// Frees the encoder (the file's sound stream goes with the file).
void Recorder::CloseAudioLocked() {
    if (aenc_) avcodec_free_context(&aenc_);
    if (aframe_) av_frame_free(&aframe_);
    ast_ = nullptr;
    apending_.clear();
}

void Recorder::PadAudioLocked(int64_t samples) {
    int64_t chunk = int64_t(aenc_->frame_size) * 16;
    while (samples > 0) {
        int64_t n = std::min(samples, chunk);
        apending_.resize(apending_.size() + size_t(n) * 2, 0.0f);
        aSamples_ += n;
        samples -= n;
        EncodeAudioLocked(false);
    }
}

// Encodes whole frames from apending_ (and, with flush, the rest padded with silence, then the encoder's delay).
void Recorder::EncodeAudioLocked(bool flush) {
    const size_t fs = size_t(aenc_->frame_size);
    size_t avail = apending_.size() / 2, off = 0;
    AVPacket* pkt = av_packet_alloc();
    auto drain = [&] {
        while (avcodec_receive_packet(aenc_, pkt) == 0) {
            av_packet_rescale_ts(pkt, aenc_->time_base, ast_->time_base);
            pkt->stream_index = ast_->index;
            WritePacketLocked(pkt);
        }
    };
    while (avail - off >= fs || (flush && avail > off)) {
        size_t n = std::min(fs, avail - off);
        if (av_frame_make_writable(aframe_) < 0) break;
        auto* l = reinterpret_cast<float*>(aframe_->data[0]);
        auto* r = reinterpret_cast<float*>(aframe_->data[1]);
        for (size_t i = 0; i < fs; ++i) {
            l[i] = i < n ? apending_[2 * (off + i)] : 0.0f;
            r[i] = i < n ? apending_[2 * (off + i) + 1] : 0.0f;
        }
        aframe_->pts = aEncoded_;
        aEncoded_ += int64_t(fs);
        int rc = avcodec_send_frame(aenc_, aframe_);
        if (rc < 0) LOGW("record: AAC encode failed: %s", AvError(rc).c_str());
        drain();
        off += n;
    }
    apending_.erase(apending_.begin(), apending_.begin() + long(off * 2));
    if (flush) {
        avcodec_send_frame(aenc_, nullptr);
        drain();
    }
    av_packet_free(&pkt);
}

void Recorder::WriteLocked(const Packet& p, int64_t pts) {
    if (lastPts_ != INT64_MIN && pts <= lastPts_) pts = lastPts_ + 1;
    lastPts_ = pts;
    AVPacket* pkt = av_packet_alloc();
    if (!pkt || av_new_packet(pkt, int(p.data.size())) < 0) {
        av_packet_free(&pkt);
        return;
    }
    std::memcpy(pkt->data, p.data.data(), p.data.size());
    pkt->pts = pkt->dts = pts;
    pkt->stream_index = st_->index;
    if (p.key) pkt->flags |= AV_PKT_FLAG_KEY;
    WriteHeldLocked(pts);  // the previous picture lasts until this one
    held_ = pkt;
    // No sound arriving (silence on the sender, or it starts late): keep the track up with the picture.
    if (aenc_ && fmt_ && p.us > t0_) {
        int64_t behind = (p.us - t0_) * trackRate_ / 1000000 - aSamples_;
        if (behind > int64_t(kSilenceAfter * trackRate_)) PadAudioLocked(behind - int64_t(kSilenceLead * trackRate_));
    }
}

void Recorder::WriteHeldLocked(int64_t until) {
    if (!held_) return;
    held_->duration = std::max<int64_t>(until - held_->pts, 1);
    WritePacketLocked(held_);
    av_packet_free(&held_);
}

void Recorder::WritePacketLocked(AVPacket* pkt) {
    // With two tracks the muxer interleaves them by time (it holds packets until both have caught up).
    int rc = ast_ ? av_interleaved_write_frame(fmt_, pkt) : av_write_frame(fmt_, pkt);
    if (rc < 0) LOGW("record: write failed: %s", AvError(rc).c_str());
}

std::wstring Recorder::CloseLocked() {
    if (!fmt_) return L"";
    // The recording runs to now, whether or not the screen changed since the last picture.
    int64_t stopUs = NowUs();
    WriteHeldLocked(std::max((stopUs - t0_) * 9 / 100, lastPts_ + 1));
    if (aenc_) {
        int64_t end = (stopUs - t0_) * trackRate_ / 1000000;  // sound to the end of the picture
        if (end > aSamples_) PadAudioLocked(end - aSamples_);
        EncodeAudioLocked(true);
        CloseAudioLocked();
    }
    av_write_trailer(fmt_);
    avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    st_ = nullptr;
    fileConfig_.clear();
    LOGI("record: finished (%.1f s)", double(stopUs - t0_) / 1e6);
    return path_;
}

std::wstring Recorder::Stop() {
    std::lock_guard<std::mutex> lk(mu_);
    return CloseLocked();
}

bool Recorder::Active() {
    std::lock_guard<std::mutex> lk(mu_);
    return fmt_ != nullptr;
}

bool Recorder::HasSound() {
    std::lock_guard<std::mutex> lk(mu_);
    return fmt_ && aenc_;
}

double Recorder::Seconds() {
    std::lock_guard<std::mutex> lk(mu_);
    return fmt_ ? double(NowUs() - t0_) / 1e6 : 0.0;
}
