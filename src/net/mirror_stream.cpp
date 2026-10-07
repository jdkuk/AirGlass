#include "net/mirror_stream.h"

#include "net/bplist.h"

bool MirrorStream::Start(const uint8_t aesKey[16], uint64_t streamConnectionId, FrameSink* sink, Callbacks cb) {
    Stop();
    // Stream key/IV = SHA-512("AirPlayStreamKey<id>" || aesKey)[0:16], same with "AirPlayStreamIV<id>".
    std::string skey = "AirPlayStreamKey" + std::to_string(streamConnectionId);
    std::string siv = "AirPlayStreamIV" + std::to_string(streamConnectionId);
    uint8_t hk[64], hi[64];
    {
        crypto::Sha512 h;
        h.Update(skey.data(), skey.size());
        h.Update(aesKey, 16);
        h.Final(hk);
    }
    {
        crypto::Sha512 h;
        h.Update(siv.data(), siv.size());
        h.Update(aesKey, 16);
        h.Final(hi);
    }
    if (!ctr_.Init(hk, hi)) {
        LOGE("mirror: AES init failed");
        return false;
    }
    uint16_t port = 0;
    listen_ = net::TcpListen(port);
    if (listen_ == INVALID_SOCKET) {
        LOGE("mirror: cannot open data port (%d)", WSAGetLastError());
        return false;
    }
    port_ = port;
    sink_ = sink;
    cb_ = std::move(cb);
    pendingSpsPps_ = false;
    paused_ = false;
    badPackets_ = 0;
    stop_ = false;
    thread_ = std::thread(&MirrorStream::Run, this);
    LOGI("mirror: listening on TCP %u (stream id %llu)", port_, (unsigned long long)streamConnectionId);
    return true;
}

void MirrorStream::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    net::CloseSocket(conn_);
    net::CloseSocket(listen_);
}

void MirrorStream::Run() {
    // Wait for the sender to connect.
    double deadline = NowSeconds() + 30.0;
    while (!stop_ && conn_ == INVALID_SOCKET) {
        int w = net::WaitReadable(listen_, 200);
        if (w < 0) break;
        if (w == 0) {
            if (NowSeconds() > deadline) break;
            continue;
        }
        sockaddr_storage from{};
        int flen = sizeof(from);
        conn_ = accept(listen_, reinterpret_cast<sockaddr*>(&from), &flen);
        if (conn_ != INVALID_SOCKET) LOGI("mirror: sender connected from %s", net::AddrToString(from).c_str());
    }
    if (conn_ == INVALID_SOCKET) {
        if (!stop_) {
            LOGW("mirror: sender never connected to the data port");
            if (cb_.onEnded) cb_.onEnded();
        }
        return;
    }
    net::SetRecvBuf(conn_, 8 << 20);
    BOOL ka = TRUE;
    setsockopt(conn_, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&ka), sizeof(ka));

    if (!decoder_.Open(sink_)) LOGE("mirror: video decoder unavailable");
    decoder_.onSize = [this](int w, int h) {
        LOGI("mirror: video size now %dx%d", w, h);
        if (cb_.onVideoSize) cb_.onVideoSize(w, h);
    };

    statStart_ = NowSeconds();
    statFrames_ = statBytes_ = 0;
    std::vector<uint8_t> payload;
    uint8_t hdr[128];
    bool clean = false;
    while (!stop_) {
        if (!net::RecvExact(conn_, hdr, sizeof(hdr), &stop_)) {
            clean = true;
            break;
        }
        uint32_t size = RdLE32(hdr);
        uint8_t type = hdr[4];
        if (size > (64u << 20)) {
            LOGE("mirror: bogus payload size %u", size);
            break;
        }
        payload.resize(size);
        if (size && !net::RecvExact(conn_, payload.data(), size, &stop_)) {
            clean = true;
            break;
        }
        switch (type) {
        case 0x00: HandleVideo(payload); break;
        case 0x01: HandleCodec(hdr, payload); break;
        case 0x02: break;  // legacy keep-alive
        case 0x05: {       // streaming statistics (binary plist)
            double now = NowSeconds();
            if (now - lastReportLog_ > 30.0 && size) {
                lastReportLog_ = now;
                size_t plen = size > 25000 ? size - 25000 : size;
                Plist p;
                if (Plist::Parse(payload.data(), plen, p)) LOGD("mirror: sender stats %s", p.Describe().c_str());
            }
            break;
        }
        default: LOGD("mirror: unknown packet type 0x%02x (%u bytes)", type, size); break;
        }

        double now = NowSeconds();
        if (now - statStart_ >= 10.0) {
            double dt = now - statStart_;
            LOGI("mirror: %.1f fps, %.2f Mbit/s, %s decode", double(statFrames_) / dt, double(statBytes_) * 8 / dt / 1e6,
                 decoder_.Hardware() ? "hardware" : "software");
            statStart_ = now;
            statFrames_ = statBytes_ = 0;
        }
    }
    decoder_.Close();
    if (!stop_) {
        LOGI("mirror: stream closed by sender%s", clean ? "" : " (error)");
        if (cb_.onEnded) cb_.onEnded();
    }
}

void MirrorStream::HandleCodec(const uint8_t* hdr, std::vector<uint8_t>& payload) {
    float w = RdLEFloat(hdr + 56), h = RdLEFloat(hdr + 60);
    float ws = RdLEFloat(hdr + 40), hs = RdLEFloat(hdr + 44);
    uint8_t opt = hdr[6];
    if (opt == 0x56 || opt == 0x5e) {
        if (!paused_) LOGI("mirror: sender paused the stream (screen locked / idle)");
        paused_ = true;
    } else if (paused_) {
        LOGI("mirror: sender resumed the stream");
        paused_ = false;
    }
    if (payload.empty()) {
        LOGE("mirror: sender chose a codec we do not support (no H.264 config)");
        return;
    }
    if (payload.size() >= 8 && std::memcmp(payload.data() + 4, "hvc1", 4) == 0) {
        LOGE("mirror: sender sent HEVC, which is not advertised/supported");
        return;
    }
    // avcC: [1 version][1 profile][1 compat][1 level][1 0xFF][1 0xE1][2 spsLen][sps][1 numPps][2 ppsLen][pps]
    if (payload.size() < 8) return;
    size_t spsLen = RdBE16(&payload[6]);
    if (8 + spsLen + 3 > payload.size()) return;
    size_t ppsLen = RdBE16(&payload[8 + spsLen + 1]);
    if (8 + spsLen + 3 + ppsLen > payload.size()) return;
    static const uint8_t sc[4] = {0, 0, 0, 1};
    spsPps_.clear();
    spsPps_.insert(spsPps_.end(), sc, sc + 4);
    spsPps_.insert(spsPps_.end(), payload.begin() + 8, payload.begin() + 8 + long(spsLen));
    spsPps_.insert(spsPps_.end(), sc, sc + 4);
    spsPps_.insert(spsPps_.end(), payload.begin() + 8 + long(spsLen) + 3,
                   payload.begin() + 8 + long(spsLen) + 3 + long(ppsLen));
    pendingSpsPps_ = true;
    LOGI("mirror: H.264 config %.0fx%.0f (source %.0fx%.0f) profile %u level %u", w, h, ws, hs, payload[1], payload[3]);
}

void MirrorStream::HandleVideo(std::vector<uint8_t>& payload) {
    const size_t size = payload.size();
    if (!size) return;
    ctr_.Process(payload.data(), size);

    // AVCC (4-byte big-endian lengths) -> Annex-B start codes, validating as we go.
    size_t pos = 0;
    bool valid = true;
    while (pos + 4 <= size) {
        uint32_t len = RdBE32(&payload[pos]);
        if (len > size - pos - 4) {
            valid = false;
            break;
        }
        payload[pos] = 0;
        payload[pos + 1] = 0;
        payload[pos + 2] = 0;
        payload[pos + 3] = 1;
        if (len && (payload[pos + 4] & 0x80)) {
            valid = false;
            break;
        }
        pos += 4 + len;
    }
    if (pos != size) valid = false;
    if (!valid) {
        if (badPackets_++ < 5) LOGW("mirror: video packet failed to decrypt/parse (%zu bytes) - key mismatch?", size);
        return;
    }
    statFrames_++;
    statBytes_ += size;
    if (pendingSpsPps_) {
        joined_.clear();
        joined_.insert(joined_.end(), spsPps_.begin(), spsPps_.end());
        joined_.insert(joined_.end(), payload.begin(), payload.end());
        pendingSpsPps_ = false;
        decoder_.Decode(joined_.data(), joined_.size());
    } else {
        decoder_.Decode(payload.data(), size);
    }
}
