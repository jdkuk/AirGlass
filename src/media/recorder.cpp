#include "media/recorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace {

constexpr size_t kMaxGopBytes = size_t(256) << 20;  // cache cap; beyond it we wait for the next keyframe

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
                if (fmt_) WriteLocked(gop_.back());
            }
        } else if (fmt_) {
            p.data.assign(data, data + n);
            WriteLocked(p);
        }
    }
    if (!stoppedPath.empty() && onAutoStop) onAutoStop(stoppedPath, "the device rotated");
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
    fmt_->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;  // pre-click frames stay negative -> edit list
    if ((rc = avio_open(&fmt_->pb, file.c_str(), AVIO_FLAG_WRITE)) < 0) {
        *error = "can't create the file: " + AvError(rc);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        return false;
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    av_dict_set(&opts, "use_editlist", "1", 0);
    rc = avformat_write_header(fmt_, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        *error = "mp4 header: " + AvError(rc);
        avio_closep(&fmt_->pb);
        avformat_free_context(fmt_);
        fmt_ = nullptr;
        _wremove(path_.c_str());
        return false;
    }
    fileConfig_ = config_;
    t0_ = NowUs();  // the click: everything cached before it is timed below zero
    lastPts_ = INT64_MIN;
    lastUs_ = t0_;
    for (const Packet& p : gop_) WriteLocked(p);
    LOGI("record: started %dx%d -> %s (%zu cached packets)", w_, h_, file.c_str(), gop_.size());
    return true;
}

void Recorder::WriteLocked(const Packet& p) {
    int64_t pts = (p.us - t0_) * 9 / 100;  // microseconds -> 90 kHz
    if (lastPts_ != INT64_MIN && pts <= lastPts_) pts = lastPts_ + 1;
    lastPts_ = pts;
    if (p.us > lastUs_) lastUs_ = p.us;
    AVPacket* pkt = av_packet_alloc();
    if (av_new_packet(pkt, int(p.data.size())) == 0) {
        std::memcpy(pkt->data, p.data.data(), p.data.size());
        pkt->pts = pkt->dts = pts;
        pkt->stream_index = st_->index;
        if (p.key) pkt->flags |= AV_PKT_FLAG_KEY;
        int rc = av_write_frame(fmt_, pkt);
        if (rc < 0) LOGW("record: write failed: %s", AvError(rc).c_str());
    }
    av_packet_free(&pkt);
}

std::wstring Recorder::CloseLocked() {
    if (!fmt_) return L"";
    av_write_trailer(fmt_);
    avio_closep(&fmt_->pb);
    avformat_free_context(fmt_);
    fmt_ = nullptr;
    st_ = nullptr;
    fileConfig_.clear();
    LOGI("record: finished (%.1f s)", double(lastUs_ - t0_) / 1e6);
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

double Recorder::Seconds() {
    std::lock_guard<std::mutex> lk(mu_);
    return fmt_ ? double(lastUs_ - t0_) / 1e6 : 0.0;
}
