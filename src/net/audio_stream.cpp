#include "net/audio_stream.h"

namespace {
constexpr size_t kSlots = 512;
constexpr double kPauseAfterSeconds = 1.0;  // no packets for this long = paused on the sender

// An ALAC frame starts with a 3-bit element type (CPE=1 stereo, SCE=0 mono), a zero instance
// tag and 12 unused zero bits; anything else is not audio for this stream.
bool LooksLikeAlac(const std::vector<uint8_t>& p, int channels) {
    if (p.size() < 3) return false;
    uint8_t first = channels == 1 ? 0x00 : 0x20;
    return p[0] == first && p[1] == 0x00 && (p[2] & 0xE0) == 0;
}
}  // namespace

bool AudioStream::Start(const sockaddr_storage& peer, uint16_t peerControlPort, const AudioFormat& fmt, bool music,
                        const uint8_t aesKey[16], const uint8_t aesIv[16], AudioOutput* out) {
    Stop();
    fmt_ = fmt;
    music_ = music;
    gapWait_ = music ? 0.20 : 0.08;
    out_ = out;
    std::memcpy(iv_, aesIv, 16);
    if (!cbc_.Init(aesKey)) return false;
    if (!dec_.Open(fmt)) return false;
    dataPort_ = ctrlPort_ = 0;
    data_ = net::UdpBind(dataPort_);
    ctrl_ = net::UdpBind(ctrlPort_);
    if (data_ == INVALID_SOCKET || ctrl_ == INVALID_SOCKET) {
        LOGE("audio: cannot open UDP ports");
        net::CloseSocket(data_);
        net::CloseSocket(ctrl_);
        return false;
    }
    net::SetRecvBuf(data_, 2 << 20);
    net::SetRecvBuf(ctrl_, 1 << 20);
    haveCtrlPeer_ = peerControlPort != 0;
    if (haveCtrlPeer_) ctrlPeer_ = net::ForSocket(ctrl_, net::WithPort(peer, peerControlPort));
    slots_.assign(kSlots, Slot{});
    haveNext_ = false;
    gapSince_ = 0;
    resendTries_ = 0;
    dropBefore_ = -1;
    playing_ = false;
    packets_ = lost_ = badFrames_ = notAudio_ = 0;
    wchar_t dumpPath[512];
    if (music && GetEnvironmentVariableW(L"AIRGLASS_DEBUG_AUDIODUMP", dumpPath, 512)) {
        dump_ = _wfopen(dumpPath, L"wb");
        LOGI("debug: dumping decoded audio to %s", WideToUtf8(dumpPath).c_str());
    }
    if (out_) {
        out_->Acquire();
        out_->Configure(fmt.sampleRate, music ? AudioOutput::Profile::Music : AudioOutput::Profile::Mirror);
        acquired_ = true;
    }
    stop_ = false;
    thread_ = std::thread(&AudioStream::Run, this);
    LOGI("audio: %s, %d samples/frame, %s, on UDP data %u / control %u", fmt.Describe().c_str(), fmt.spf,
         music ? "music session" : "mirroring", dataPort_, ctrlPort_);
    return true;
}

void AudioStream::Stop() {
    stop_ = true;
    if (thread_.joinable()) {
        thread_.join();
        LOGI("audio: stream stopped (%llu packets, %llu lost, %llu not audio)", (unsigned long long)packets_,
             (unsigned long long)lost_, (unsigned long long)notAudio_);
    }
    if (acquired_ && out_) {
        out_->Release();
        acquired_ = false;
    }
    net::CloseSocket(data_);
    net::CloseSocket(ctrl_);
    dec_.Close();
    if (dump_) {
        fclose(dump_);
        dump_ = nullptr;
    }
}

void AudioStream::Flush(int nextSeq) {
    flushSeq_ = nextSeq;
    flushReq_ = true;
}

void AudioStream::SetPlaying(bool playing, double at) {
    if (playing_ == playing) return;
    playing_ = playing;
    if (onPlaying) onPlaying(playing, at);
}

void AudioStream::RequestResend(uint16_t seq, uint16_t count) {
    if (!haveCtrlPeer_ || count == 0) return;
    uint8_t pkt[8];
    pkt[0] = 0x80;
    pkt[1] = 0x55 | 0x80;
    WrBE16(pkt + 2, ctrlSeq_++);
    WrBE16(pkt + 4, seq);
    WrBE16(pkt + 6, count);
    sendto(ctrl_, reinterpret_cast<const char*>(pkt), sizeof(pkt), 0, reinterpret_cast<const sockaddr*>(&ctrlPeer_),
           net::AddrLen(ctrlPeer_));
}

void AudioStream::DecodeAndPlay(std::vector<uint8_t>& payload) {
    size_t len = payload.size();
    size_t enc = len / 16 * 16;
    if (enc) cbc_.Decrypt(iv_, payload.data(), payload.data(), enc);
    if (fmt_.ct == 2 && !LooksLikeAlac(payload, fmt_.channels)) {
        if (notAudio_++ < 5)
            LOGI("audio: skipping a %zu-byte packet that is not an ALAC frame (0x%02x%02x)", len,
                 payload.empty() ? 0 : payload[0], len > 1 ? payload[1] : 0);
        return;
    }
    if (fmt_.ct == 8 && !payload.empty() && (payload[0] & 0xF0) != 0x80 && badFrames_++ < 5)
        LOGW("audio: frame does not look like AAC-ELD after decryption (0x%02x) - key mismatch?", payload[0]);
    pcm_.clear();
    int frames = dec_.Decode(payload.data(), len, pcm_);
    if (dump_ && frames > 0) {
        std::vector<int16_t> s(pcm_.size());
        for (size_t i = 0; i < s.size(); ++i) s[i] = int16_t(std::clamp(std::lrint(pcm_[i] * 32768.0f), -32768L, 32767L));
        fwrite(s.data(), sizeof(int16_t), s.size(), dump_);
    }
    if (frames > 0 && out_) out_->Push(pcm_.data(), size_t(frames));
    else if (frames < 0 && music_ && out_) out_->PushSilence(size_t(fmt_.spf));  // keep timing
}

void AudioStream::OnRtp(const uint8_t* p, size_t n) {
    static const uint8_t kNoData[4] = {0x00, 0x68, 0x34, 0x00};
    if (n <= 12) return;
    if (n == 16 && std::memcmp(p + 12, kNoData, 4) == 0) return;
    uint16_t seq = RdBE16(p + 2);
    if (dropBefore_ >= 0) {
        if (int16_t(uint16_t(seq - uint16_t(dropBefore_))) < 0) return;  // in flight from before the flush
        dropBefore_ = -1;
    }
    lastPacket_ = NowSeconds();
    if (!haveNext_) {
        haveNext_ = true;
        nextSeq_ = highSeq_ = seq;
        if (music_)
            LOGI("audio: stream %s at seq %u (%zu-byte packets)", packets_ ? "resumed" : "started", seq, n);
    }
    SetPlaying(true, lastPacket_);
    int16_t ahead = int16_t(uint16_t(seq - nextSeq_));
    if (ahead < 0) return;  // late or duplicate
    if (ahead >= int16_t(kSlots)) {
        for (auto& s : slots_) s.filled = false;
        nextSeq_ = highSeq_ = seq;
        ahead = 0;
    }
    Slot& s = slots_[seq % kSlots];
    if (s.filled && s.seq == seq) return;  // redundant copy
    s.filled = true;
    s.seq = seq;
    s.payload.assign(p + 12, p + n);
    ++packets_;
    if (int16_t(uint16_t(seq - highSeq_)) > 0) highSeq_ = seq;
    Drain(false);
}

void AudioStream::Drain(bool timeUp) {
    for (;;) {
        Slot& s = slots_[nextSeq_ % kSlots];
        if (s.filled && s.seq == nextSeq_) {
            s.filled = false;
            DecodeAndPlay(s.payload);
            ++nextSeq_;
            gapSince_ = 0;
            resendTries_ = 0;
            continue;
        }
        int16_t pending = int16_t(uint16_t(highSeq_ - nextSeq_));
        if (pending <= 0) return;  // nothing newer buffered
        double now = NowSeconds();
        if (gapSince_ == 0) {
            gapSince_ = now;
            resendTries_ = 0;
            lastResend_ = 0;
        }
        // Ask for the missing run again every 60 ms while we can still wait for it.
        if (resendTries_ < 3 && now - lastResend_ > 0.06) {
            uint16_t count = 0;
            for (uint16_t q = nextSeq_; q != highSeq_ && count < 64; ++q) {
                const Slot& m = slots_[q % kSlots];
                if (m.filled && m.seq == q) break;
                ++count;
            }
            RequestResend(nextSeq_, count);
            lastResend_ = now;
            ++resendTries_;
        }
        if (timeUp || pending > (music_ ? 96 : 24) || now - gapSince_ > gapWait_) {
            ++lost_;
            ++nextSeq_;
            gapSince_ = 0;
            if (music_ && out_) out_->PushSilence(size_t(fmt_.spf));  // keep timing
            if (music_ && (lost_ <= 10 || lost_ % 100 == 0))
                LOGI("audio: packet %u lost after %d resend requests (%llu lost so far)", uint16_t(nextSeq_ - 1),
                     resendTries_, (unsigned long long)lost_);
            continue;
        }
        return;
    }
}

void AudioStream::Run() {
    std::vector<uint8_t> buf(64 * 1024);
    while (!stop_) {
        if (flushReq_.exchange(false)) {
            for (auto& s : slots_) s.filled = false;
            haveNext_ = false;
            gapSince_ = 0;
            dropBefore_ = flushSeq_.load();
            if (out_) out_->Flush();
            SetPlaying(false, NowSeconds());
            LOGI("audio: flush (next seq %d)", dropBefore_);
        }
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(data_, &rf);
        FD_SET(ctrl_, &rf);
        timeval tv{0, 20000};
        int r = select(0, &rf, nullptr, nullptr, &tv);
        if (r < 0) break;
        if (r == 0) {
            if (gapSince_ != 0 && NowSeconds() - gapSince_ > gapWait_) Drain(true);
            else if (gapSince_ != 0) Drain(false);  // keep re-requesting
            if (playing_ && NowSeconds() - lastPacket_ > kPauseAfterSeconds)
                SetPlaying(false, lastPacket_ + double(fmt_.spf) / fmt_.sampleRate);
            continue;
        }
        if (FD_ISSET(ctrl_, &rf)) {
            sockaddr_storage from{};
            int flen = sizeof(from);
            int n = recvfrom(ctrl_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &flen);
            if (n >= 4) {
                if (!haveCtrlPeer_) {
                    ctrlPeer_ = from;
                    haveCtrlPeer_ = true;
                }
                uint8_t type = buf[1] & 0x7F;
                if (type == 0x56 && n >= 16) OnRtp(buf.data() + 4, size_t(n - 4));  // retransmitted packet
            }
        }
        if (FD_ISSET(data_, &rf)) {
            int n = recv(data_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
            if (n > 0) OnRtp(buf.data(), size_t(n));
        }
        if (playing_ && NowSeconds() - lastPacket_ > kPauseAfterSeconds)
            SetPlaying(false, lastPacket_ + double(fmt_.spf) / fmt_.sampleRate);
    }
}
