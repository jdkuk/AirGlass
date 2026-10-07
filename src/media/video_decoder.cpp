#include "media/video_decoder.h"

#include <d3d11.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

namespace {
void GpuLockFn(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->lock(); }
void GpuUnlockFn(void* ctx) { static_cast<std::recursive_mutex*>(ctx)->unlock(); }

enum AVPixelFormat PickFormat(AVCodecContext* ctx, const enum AVPixelFormat* fmts) {
    for (const enum AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_D3D11) return *p;
    LOGW("video: hardware format not offered for this stream; decoding in software");
    return avcodec_default_get_format(ctx, fmts);
}
}  // namespace

bool VideoDecoder::Open(FrameSink* sink) {
    Close();
    sink_ = sink;
    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        LOGE("video: no H.264 decoder in FFmpeg build");
        return false;
    }
    ctx_ = avcodec_alloc_context3(codec);
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_type = FF_THREAD_SLICE;
    ctx_->thread_count = 0;

    hwDev_ = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (hwDev_ && sink_ && sink_->GpuDevice()) {
        auto* dc = reinterpret_cast<AVHWDeviceContext*>(hwDev_->data);
        auto* d3d = static_cast<AVD3D11VADeviceContext*>(dc->hwctx);
        d3d->device = sink_->GpuDevice();
        d3d->device->AddRef();
        d3d->lock = GpuLockFn;
        d3d->unlock = GpuUnlockFn;
        d3d->lock_ctx = &sink_->GpuLock();
        if (av_hwdevice_ctx_init(hwDev_) >= 0) {
            ctx_->hw_device_ctx = av_buffer_ref(hwDev_);
            ctx_->get_format = PickFormat;
        } else {
            LOGW("video: D3D11VA init failed; software decoding");
            av_buffer_unref(&hwDev_);
        }
    }
    int rc = avcodec_open2(ctx_, codec, nullptr);
    if (rc < 0) {
        LOGE("video: avcodec_open2 failed %d", rc);
        Close();
        return false;
    }
    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    lastW_ = lastH_ = 0;
    loggedFormat_ = false;
    errors_ = 0;
    return true;
}

void VideoDecoder::Close() {
    if (ctx_) avcodec_free_context(&ctx_);
    if (hwDev_) av_buffer_unref(&hwDev_);
    if (pkt_) av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
}

int VideoDecoder::Decode(const uint8_t* data, size_t n) {
    if (!ctx_ || !n) return 0;
    buf_.resize(n + AV_INPUT_BUFFER_PADDING_SIZE);
    std::memcpy(buf_.data(), data, n);
    std::memset(buf_.data() + n, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    pkt_->data = buf_.data();
    pkt_->size = int(n);
    int rc = avcodec_send_packet(ctx_, pkt_);
    if (rc < 0 && rc != AVERROR(EAGAIN)) {
        if (errors_++ < 10) LOGW("video: decoder rejected packet (%d bytes): %d", int(n), rc);
    }
    int frames = 0;
    for (;;) {
        rc = avcodec_receive_frame(ctx_, frame_);
        if (rc < 0) break;
        Output(frame_);
        av_frame_unref(frame_);
        ++frames;
    }
    return frames;
}

void VideoDecoder::Output(AVFrame* f) {
    VideoColor c;
    c.fullRange = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    switch (f->colorspace) {
    case AVCOL_SPC_BT709: c.matrix = 709; break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: c.matrix = 2020; break;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG: c.matrix = 601; break;
    default: c.matrix = f->height >= 720 ? 709 : 601; break;
    }
    int w = f->width & ~1, h = f->height & ~1;
    if (w <= 0 || h <= 0) return;

    bool hw = f->format == AV_PIX_FMT_D3D11;
    if (!loggedFormat_) {
        loggedFormat_ = true;
        LOGI("video: first frame %dx%d, %s decode, matrix BT.%d %s range", w, h, hw ? "hardware (D3D11VA)" : "software",
             c.matrix, c.fullRange ? "full" : "limited");
    }
    lastWasHw_ = hw;
    if (hw) {
        auto* tex = reinterpret_cast<ID3D11Texture2D*>(f->data[0]);
        unsigned index = unsigned(reinterpret_cast<intptr_t>(f->data[1]));
        sink_->PresentHwFrame(tex, index, w, h, c);
    } else if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
        const uint8_t* planes[3] = {f->data[0], f->data[1], f->data[2]};
        int strides[3] = {f->linesize[0], f->linesize[1], f->linesize[2]};
        sink_->PresentSwFrame(planes, strides, false, w, h, c);
    } else if (f->format == AV_PIX_FMT_NV12) {
        const uint8_t* planes[3] = {f->data[0], f->data[1], nullptr};
        int strides[3] = {f->linesize[0], f->linesize[1], 0};
        sink_->PresentSwFrame(planes, strides, true, w, h, c);
    } else {
        if (errors_++ < 5) LOGW("video: unsupported pixel format %d", f->format);
        return;
    }
    if (w != lastW_ || h != lastH_) {
        lastW_ = w;
        lastH_ = h;
        if (onSize) onSize(w, h);
    }
}
