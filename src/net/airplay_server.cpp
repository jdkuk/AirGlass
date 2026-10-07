#include "net/airplay_server.h"

#include <cstdio>

#include "crypto/crypto.h"
#include "crypto/fairplay.h"
#include "net/audio_stream.h"
#include "net/bplist.h"
#include "net/dacp.h"
#include "net/mdns.h"
#include "net/mirror_stream.h"
#include "net/netutil.h"
#include "net/ntp_client.h"

namespace {

constexpr const char* kSourceVersion = "220.68";
constexpr const char* kModel = "AppleTV3,2";

struct Request {
    std::string method, uri, protocol;
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;

    const std::string* Header(const char* name) const {
        for (auto& h : headers)
            if (_stricmp(h.first.c_str(), name) == 0) return &h.second;
        return nullptr;
    }
    std::string ContentType() const {
        const std::string* h = Header("Content-Type");
        return h ? *h : std::string();
    }
};

struct Response {
    int code = 200;
    std::string reason = "OK";
    std::vector<std::pair<std::string, std::string>> headers;
    std::vector<uint8_t> body;
    bool closeAfter = false;

    void Add(const std::string& k, const std::string& v) { headers.emplace_back(k, v); }
    void SetPlist(const Plist& p) {
        body = p.Serialize();
        Add("Content-Type", "application/x-apple-binary-plist");
    }
    void SetOctets(const uint8_t* d, size_t n) {
        body.assign(d, d + n);
        Add("Content-Type", "application/octet-stream");
    }
};

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string FeaturesString(uint64_t f) {
    char b[32];
    snprintf(b, sizeof(b), "0x%X,0x%X", unsigned(f & 0xFFFFFFFFu), unsigned(f >> 32));
    return b;
}

// Track metadata from a DMAP ("application/x-dmap-tagged") SET_PARAMETER body: 4-char tag,
// 32-bit big-endian length, value; "mlit" items are containers.
struct TrackInfo {
    std::string title, artist, album;
    int64_t durationMs = -1;
};

void ParseDmap(const uint8_t* p, size_t n, TrackInfo& t, int depth) {
    size_t off = 0;
    while (off + 8 <= n) {
        std::string tag(reinterpret_cast<const char*>(p + off), 4);
        uint32_t len = RdBE32(p + off + 4);
        off += 8;
        if (len > n - off) break;
        const uint8_t* d = p + off;
        if ((tag == "mlit" || tag == "mlcl" || tag == "cmst") && depth < 4) ParseDmap(d, len, t, depth + 1);
        else if (tag == "minm") t.title.assign(reinterpret_cast<const char*>(d), len);
        else if (tag == "asar") t.artist.assign(reinterpret_cast<const char*>(d), len);
        else if (tag == "asal") t.album.assign(reinterpret_cast<const char*>(d), len);
        else if (tag == "astm" && len == 4) t.durationMs = RdBE32(d);
        off += len;
    }
}

bool WriteFileAtomic(const std::wstring& path, const std::vector<uint8_t>& data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data.data(), DWORD(data.size()), &w, nullptr) && w == data.size();
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

float VolumeFraction(float db) { return db <= -100.0f ? 0.0f : std::clamp((db + 30.0f) / 30.0f, 0.0f, 1.0f); }

}  // namespace

// ---------------------------------------------------------------------------------------------

std::string ReceiverInfo::DeviceId() const {
    char b[32];
    snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return b;
}

std::string ReceiverInfo::MacHex() const {
    char b[32];
    snprintf(b, sizeof(b), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return b;
}

std::string ReceiverInfo::PkHex() const { return HexEncode(edPub, 32); }

std::vector<std::pair<std::string, std::string>> ReceiverInfo::AirPlayTxt() const {
    return {{"deviceid", DeviceId()}, {"features", FeaturesString(features)},
            {"pw", "false"},          {"flags", "0x4"},
            {"model", kModel},        {"pk", PkHex()},
            {"pi", pi},               {"srcvers", kSourceVersion},
            {"vv", "2"}};
}

std::vector<std::pair<std::string, std::string>> ReceiverInfo::RaopTxt() const {
    return {{"ch", "2"},       {"cn", "0,1,2,3"},      {"da", "true"},    {"et", "0,3,5"},
            {"vv", "2"},       {"ft", FeaturesString(features)}, {"am", kModel}, {"md", "0,1,2"},
            {"rhd", "5.6.0.0"}, {"pw", "false"},       {"sr", "44100"},   {"ss", "16"},
            {"sv", "false"},   {"tp", "UDP"},          {"txtvers", "1"},  {"sf", "0x4"},
            {"vs", kSourceVersion}, {"vn", "65537"},   {"pk", PkHex()}};
}

// ---------------------------------------------------------------------------------------------

class AirPlayServer::Connection : public std::enable_shared_from_this<AirPlayServer::Connection> {
public:
    Connection(AirPlayServer* srv, SOCKET s, const sockaddr_storage& peer, uint64_t id)
        : srv_(srv), sock_(s), peer_(peer), id_(id) {
        dacp_.onAvailable = [this] { NotifyNowPlaying(); };
    }
    ~Connection() {
        StopStreams();
        if (sock_ != INVALID_SOCKET) closesocket(sock_);
    }

    void Start() { thread_ = std::thread(&Connection::Run, this); }
    void Kick() {
        kicked_ = true;
        shutdown(sock_, SD_BOTH);
    }
    void Join() {
        if (thread_.joinable()) thread_.join();
    }
    bool Done() const { return done_; }
    uint64_t Id() const { return id_; }
    void SnapshotNowPlaying(NowPlaying& out);
    std::string Media(const std::string& action, double value);

private:
    enum class PairState { Initial, Setup, Handshake, Finished };

    void Run();
    int ReadRequest(Request& req);
    bool SendResponse(const Request& req, const Response& resp);
    void Handle(const Request& req, Response& resp);

    void Info(const Request& req, Response& resp);
    void PairSetup(const Request& req, Response& resp);
    void PairVerify(const Request& req, Response& resp);
    void FpSetup(const Request& req, Response& resp);
    void Setup(const Request& req, Response& resp);
    void GetParameter(const Request& req, Response& resp);
    void SetParameter(const Request& req, Response& resp);
    void Teardown(const Request& req, Response& resp);

    void DeriveKey(const char* salt, uint8_t out[16]) const;
    void StopMirror();
    void StopAudio();
    void StopStreams();
    void NotifyMirrorStopped();
    void NotifyNowPlaying() {
        if (srv_->ev_.nowPlayingChanged) srv_->ev_.nowPlayingChanged();
    }
    double PositionLocked(double at) const;  // seconds at time `at`; requires npMu_
    void OnPlaying(bool playing, double at);
    void OnProgress(const std::string& value);
    void OnArtwork(const std::string& type, const std::vector<uint8_t>& body);

    AirPlayServer* srv_;
    SOCKET sock_;
    sockaddr_storage peer_;
    uint64_t id_;
    std::thread thread_;
    std::atomic<bool> done_{false};
    std::atomic<bool> kicked_{false};
    std::vector<uint8_t> inbuf_;

    PairState pair_ = PairState::Initial;
    uint8_t ecdhOurs_[32]{}, ecdhTheirs_[32]{}, ecdhSecret_[32]{}, edTheirs_[32]{};
    FairPlay fp_;
    bool keysReady_ = false;
    uint8_t aesKey_[16]{}, aesIv_[16]{};
    std::string devName_, devModel_, devId_;

    std::unique_ptr<NtpClient> ntp_;
    std::unique_ptr<MirrorStream> mirror_;
    std::unique_ptr<AudioStream> audio_;
    std::atomic<bool> mirroring_{false};

    // Music (audio-only) session state, guarded by npMu_.
    std::mutex npMu_;
    bool music_ = false;
    AudioFormat npFormat_;
    TrackInfo npTrack_;
    bool npHaveProgress_ = false, npPlaying_ = false;
    double npAnchorPos_ = 0, npAnchorTime_ = 0;  // position (s) at a moment (NowSeconds)
    uint32_t npArtworkSeq_ = 0;
    std::string npArtworkType_;
    std::wstring npArtworkPath_;
    DacpRemote dacp_;
    bool loggedDacp_ = false;
};

void AirPlayServer::Connection::Run() {
    LOGI("rtsp[%llu]: connection from %s", (unsigned long long)id_, net::AddrToString(peer_).c_str());
    while (!srv_->stop_ && !kicked_) {
        Request req;
        int r = ReadRequest(req);
        if (r <= 0) break;
        Response resp;
        Handle(req, resp);
        if (!SendResponse(req, resp)) break;
        if (resp.closeAfter) break;
    }
    LOGI("rtsp[%llu]: connection ended%s", (unsigned long long)id_, kicked_ ? " (disconnected by receiver)" : "");
    StopStreams();
    done_ = true;
}

int AirPlayServer::Connection::ReadRequest(Request& req) {
    size_t hdrEnd = std::string::npos;
    for (;;) {
        static const char kSep[] = "\r\n\r\n";
        auto it = std::search(inbuf_.begin(), inbuf_.end(), kSep, kSep + 4);
        if (it != inbuf_.end()) {
            hdrEnd = size_t(it - inbuf_.begin()) + 4;
            break;
        }
        if (inbuf_.size() > 64 * 1024) return -1;
        char tmp[16384];
        int n = recv(sock_, tmp, sizeof(tmp), 0);
        if (n <= 0) return 0;
        inbuf_.insert(inbuf_.end(), tmp, tmp + n);
    }
    std::string head(reinterpret_cast<const char*>(inbuf_.data()), hdrEnd);
    size_t lineEnd = head.find("\r\n");
    std::string first = head.substr(0, lineEnd);
    size_t s1 = first.find(' ');
    size_t s2 = first.rfind(' ');
    if (s1 == std::string::npos || s2 == s1) return -1;
    req.method = first.substr(0, s1);
    req.uri = first.substr(s1 + 1, s2 - s1 - 1);
    req.protocol = first.substr(s2 + 1);
    size_t pos = lineEnd + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos || e == pos) break;
        std::string line = head.substr(pos, e - pos);
        size_t colon = line.find(':');
        if (colon != std::string::npos) req.headers.emplace_back(Trim(line.substr(0, colon)), Trim(line.substr(colon + 1)));
        pos = e + 2;
    }
    size_t len = 0;
    if (const std::string* cl = req.Header("Content-Length")) len = size_t(strtoul(cl->c_str(), nullptr, 10));
    if (len > (16u << 20)) return -1;
    while (inbuf_.size() < hdrEnd + len) {
        char tmp[65536];
        int n = recv(sock_, tmp, sizeof(tmp), 0);
        if (n <= 0) return 0;
        inbuf_.insert(inbuf_.end(), tmp, tmp + n);
    }
    req.body.assign(inbuf_.begin() + long(hdrEnd), inbuf_.begin() + long(hdrEnd + len));
    inbuf_.erase(inbuf_.begin(), inbuf_.begin() + long(hdrEnd + len));
    return 1;
}

bool AirPlayServer::Connection::SendResponse(const Request& req, const Response& resp) {
    std::string h = (req.protocol.empty() ? std::string("RTSP/1.0") : req.protocol) + " " + std::to_string(resp.code) +
                    " " + resp.reason + "\r\n";
    const std::string* cseq = req.Header("CSeq");
    if (cseq && req.method != "RECORD") h += "Audio-Jack-Status: connected; type=digital\r\n";
    bool hasType = false;
    for (auto& kv : resp.headers) {
        h += kv.first + ": " + kv.second + "\r\n";
        if (_stricmp(kv.first.c_str(), "Content-Type") == 0) hasType = true;
    }
    h += std::string("Server: AirTunes/") + kSourceVersion + "\r\n";
    if (cseq) h += "CSeq: " + *cseq + "\r\n";
    if (!resp.body.empty()) {
        h += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
    } else if (hasType) {
        h += "Content-Length: 0\r\n";
    }
    h += "\r\n";
    std::vector<uint8_t> out(h.begin(), h.end());
    out.insert(out.end(), resp.body.begin(), resp.body.end());
    return net::SendAll(sock_, out.data(), out.size());
}

void AirPlayServer::Connection::Handle(const Request& req, Response& resp) {
    const std::string& m = req.method;
    LogWrite((m == "POST" && req.uri == "/feedback") ? LogLevel::Debug : LogLevel::Info,
             "rtsp[%llu]: %s %s %s (%zu bytes%s%s)", (unsigned long long)id_, m.c_str(), req.uri.c_str(),
             req.protocol.c_str(), req.body.size(), req.ContentType().empty() ? "" : ", ", req.ContentType().c_str());
    if (req.protocol != "RTSP/1.0") {
        LOGI("rtsp[%llu]: ignoring %s %s %s (AirPlay video/HLS is not supported)", (unsigned long long)id_, m.c_str(),
             req.uri.c_str(), req.protocol.c_str());
        resp.code = 501;
        resp.reason = "Not Implemented";
        return;
    }
    // Music senders identify their DACP remote-control service on every request.
    const std::string* dacpId = req.Header("DACP-ID");
    const std::string* activeRemote = req.Header("Active-Remote");
    if (dacpId && activeRemote) {
        if (!loggedDacp_) {
            loggedDacp_ = true;
            LOGI("rtsp[%llu]: sender offers remote control (DACP-ID %s)", (unsigned long long)id_, dacpId->c_str());
        }
        dacp_.Configure(*dacpId, *activeRemote, peer_);
    }
    const std::string& path = req.uri;
    if (m == "POST") {
        if (path == "/pair-setup") PairSetup(req, resp);
        else if (path == "/pair-verify") PairVerify(req, resp);
        else if (path == "/fp-setup") FpSetup(req, resp);
        else if (path == "/feedback" || path == "/audioMode") {}
        else LOGI("rtsp[%llu]: unhandled POST %s", (unsigned long long)id_, path.c_str());
    } else if (m == "GET") {
        if (path.find("/info") != std::string::npos) Info(req, resp);
        else LOGI("rtsp[%llu]: unhandled GET %s", (unsigned long long)id_, path.c_str());
    } else if (m == "OPTIONS") {
        resp.Add("Public", "SETUP, RECORD, FLUSH, TEARDOWN, OPTIONS, GET_PARAMETER, SET_PARAMETER");
    } else if (m == "SETUP") {
        Setup(req, resp);
    } else if (m == "GET_PARAMETER") {
        GetParameter(req, resp);
    } else if (m == "SET_PARAMETER") {
        SetParameter(req, resp);
    } else if (m == "RECORD") {
        resp.Add("Audio-Latency", "11025");
        resp.Add("Audio-Jack-Status", "connected; type=analog");
    } else if (m == "FLUSH") {
        // "RTP-Info: seq=123;rtptime=456": packets before seq belong to the old position.
        int nextSeq = -1;
        if (const std::string* info = req.Header("RTP-Info")) {
            size_t p = info->find("seq=");
            if (p != std::string::npos) nextSeq = int(strtoul(info->c_str() + p + 4, nullptr, 10) & 0xFFFF);
        }
        if (audio_) audio_->Flush(nextSeq);
    } else if (m == "TEARDOWN") {
        Teardown(req, resp);
    } else {
        resp.code = 501;
        resp.reason = "Not Implemented";
    }
}

void AirPlayServer::Connection::Info(const Request& req, Response& resp) {
    const ReceiverInfo& ri = srv_->info_;
    bool hasCseq = req.Header("CSeq") != nullptr;
    std::string ct = req.ContentType();
    bool txtAirPlay = false, txtRaop = false;
    if (ct.find("application/x-apple-binary-plist") != std::string::npos) {
        Plist p;
        if (Plist::Parse(req.body.data(), req.body.size(), p)) {
            const Plist* q = p.Get("qualifier");
            if (q && q->IsArray() && !q->Items().empty()) {
                const std::string& s = q->Items()[0].AsString();
                txtAirPlay = s == "txtAirPlay";
                txtRaop = s == "txtRAOP";
            }
        }
    }
    if (!hasCseq) {
        txtAirPlay = req.uri.find("txtAirPlay") != std::string::npos;
        txtRaop = req.uri.find("txtRAOP") != std::string::npos;
    }
    Plist d = Plist::Dict();
    if (txtAirPlay) d.Set("txtAirPlay", Plist::Data(MdnsResponder::EncodeTxt(ri.AirPlayTxt())));
    if (txtRaop) d.Set("txtRAOP", Plist::Data(MdnsResponder::EncodeTxt(ri.RaopTxt())));
    if (!ct.empty()) {
        resp.SetPlist(d);
        return;
    }
    d.Set("deviceID", Plist::String(ri.DeviceId()));
    d.Set("macAddress", Plist::String(ri.DeviceId()));
    d.Set("pk", Plist::Data(ri.edPub, 32));
    d.Set("features", Plist::Int(ri.features));
    d.Set("name", Plist::String(ri.name));
    d.Set("pi", Plist::String(ri.pi));
    d.Set("vv", Plist::Int(2));
    d.Set("statusFlags", Plist::Int(68));
    d.Set("keepAliveLowPower", Plist::Int(1));
    d.Set("sourceVersion", Plist::String(kSourceVersion));
    d.Set("keepAliveSendStatsAsBody", Plist::Bool(true));
    d.Set("model", Plist::String(kModel));
    if (!hasCseq) {
        resp.SetPlist(d);
        return;
    }
    d.Set("initialVolume", Plist::Real(srv_->audio_ ? srv_->audio_->VolumeDb() : 0.0));

    Plist latencies = Plist::Array();
    for (int type : {100, 101}) {
        Plist e = Plist::Dict();
        e.Set("type", Plist::Int(uint64_t(type)));
        e.Set("inputLatencyMicros", Plist::Int(0));
        e.Set("audioType", Plist::String("default"));
        e.Set("outputLatencyMicros", Plist::Bool(false));
        latencies.Append(e);
    }
    d.Set("audioLatencies", latencies);

    Plist formats = Plist::Array();
    for (int type : {100, 101}) {
        Plist e = Plist::Dict();
        e.Set("audioOutputFormats", Plist::Int(0x3fffffc));
        e.Set("type", Plist::Int(uint64_t(type)));
        e.Set("audioInputFormats", Plist::Int(0x3fffffc));
        formats.Append(e);
    }
    d.Set("audioFormats", formats);

    Plist disp = Plist::Dict();
    disp.Set("uuid", Plist::String(ri.displayUuid));
    disp.Set("widthPhysical", Plist::Int(0));
    disp.Set("heightPhysical", Plist::Int(0));
    disp.Set("width", Plist::Int(uint64_t(ri.width)));
    disp.Set("height", Plist::Int(uint64_t(ri.height)));
    disp.Set("widthPixels", Plist::Int(uint64_t(ri.width)));
    disp.Set("heightPixels", Plist::Int(uint64_t(ri.height)));
    disp.Set("rotation", Plist::Bool(false));
    disp.Set("refreshRate", Plist::Real(1.0 / double(std::max(ri.refresh, 1))));
    disp.Set("maxFPS", Plist::Int(uint64_t(ri.maxFps)));
    disp.Set("overscanned", Plist::Bool(false));
    disp.Set("features", Plist::Int(14));
    Plist displays = Plist::Array();
    displays.Append(disp);
    d.Set("displays", displays);
    resp.SetPlist(d);
}

void AirPlayServer::Connection::PairSetup(const Request& req, Response& resp) {
    if (req.body.size() != 32) {
        LOGE("rtsp[%llu]: pair-setup with %zu bytes (PIN/HomeKit pairing is not supported)", (unsigned long long)id_,
             req.body.size());
        return;
    }
    resp.SetOctets(srv_->info_.edPub, 32);
    pair_ = PairState::Setup;
}

void AirPlayServer::Connection::DeriveKey(const char* salt, uint8_t out[16]) const {
    crypto::Sha512 h;
    h.Update(salt, strlen(salt));
    h.Update(ecdhSecret_, 32);
    uint8_t d[64];
    h.Final(d);
    std::memcpy(out, d, 16);
}

void AirPlayServer::Connection::PairVerify(const Request& req, Response& resp) {
    if (pair_ != PairState::Setup && pair_ != PairState::Handshake) {
        LOGW("rtsp[%llu]: pair-verify before pair-setup", (unsigned long long)id_);
        return;
    }
    const auto& b = req.body;
    if (b.size() < 4) return;
    const ReceiverInfo& ri = srv_->info_;
    if (b[0] == 1) {
        if (b.size() != 4 + 32 + 32) return;
        std::memcpy(ecdhTheirs_, b.data() + 4, 32);
        std::memcpy(edTheirs_, b.data() + 36, 32);
        uint8_t priv[32];
        crypto::RandomBytes(priv, 32);
        crypto::X25519PublicKey(ecdhOurs_, priv);
        crypto::X25519(ecdhSecret_, priv, ecdhTheirs_);
        SecureZeroMemory(priv, sizeof(priv));
        pair_ = PairState::Handshake;

        uint8_t msg[64], sig[64];
        std::memcpy(msg, ecdhOurs_, 32);
        std::memcpy(msg + 32, ecdhTheirs_, 32);
        crypto::Ed25519Sign(sig, msg, 64, ri.edPub, ri.edPriv);
        uint8_t key[16], iv[16];
        DeriveKey("Pair-Verify-AES-Key", key);
        DeriveKey("Pair-Verify-AES-IV", iv);
        crypto::AesCtr ctr;
        ctr.Init(key, iv);
        ctr.Process(sig, 64);
        uint8_t out[96];
        std::memcpy(out, ecdhOurs_, 32);
        std::memcpy(out + 32, sig, 64);
        resp.SetOctets(out, sizeof(out));
    } else if (b[0] == 0) {
        if (b.size() != 4 + 64 || pair_ != PairState::Handshake) {
            LOGE("rtsp[%llu]: pair-verify step 2 out of order", (unsigned long long)id_);
            resp.closeAfter = true;
            return;
        }
        uint8_t key[16], iv[16];
        DeriveKey("Pair-Verify-AES-Key", key);
        DeriveKey("Pair-Verify-AES-IV", iv);
        crypto::AesCtr ctr;
        ctr.Init(key, iv);
        uint8_t skip[64] = {};
        ctr.Process(skip, 64);  // our signature used the first 64 keystream bytes
        uint8_t sig[64];
        std::memcpy(sig, b.data() + 4, 64);
        ctr.Process(sig, 64);
        uint8_t msg[64];
        std::memcpy(msg, ecdhTheirs_, 32);
        std::memcpy(msg + 32, ecdhOurs_, 32);
        if (!crypto::Ed25519Verify(sig, msg, 64, edTheirs_)) {
            LOGE("rtsp[%llu]: pair-verify signature check FAILED", (unsigned long long)id_);
            resp.closeAfter = true;
            return;
        }
        pair_ = PairState::Finished;
        resp.Add("Content-Type", "application/octet-stream");
        LOGI("rtsp[%llu]: paired (pair-verify ok)", (unsigned long long)id_);
    }
}

void AirPlayServer::Connection::FpSetup(const Request& req, Response& resp) {
    const auto& b = req.body;
    if (b.size() == 16) {
        if (b[4] != 0x03) {
            LOGE("rtsp[%llu]: unsupported FairPlay version 0x%02x", (unsigned long long)id_, b[4]);
            resp.code = 501;
            resp.reason = "Not Implemented";
            return;
        }
        uint8_t out[142];
        if (fp_.Setup(b.data(), b.size(), out)) resp.SetOctets(out, sizeof(out));
    } else if (b.size() == 164) {
        uint8_t out[32];
        if (fp_.Handshake(b.data(), b.size(), out)) {
            resp.SetOctets(out, sizeof(out));
            LOGI("rtsp[%llu]: FairPlay handshake done", (unsigned long long)id_);
        }
    } else {
        LOGE("rtsp[%llu]: fp-setup with unexpected length %zu", (unsigned long long)id_, b.size());
    }
}

void AirPlayServer::Connection::Setup(const Request& req, Response& resp) {
    Plist root;
    if (!Plist::Parse(req.body.data(), req.body.size(), root)) root = Plist::Dict();
    LOGD("rtsp[%llu]: SETUP %s", (unsigned long long)id_, root.Describe().c_str());
    Plist res = Plist::Dict();

    const Plist* ekey = root.Get("ekey");
    const Plist* eiv = root.Get("eiv");
    if (ekey && eiv && ekey->type() == Plist::Type::Data && eiv->type() == Plist::Type::Data) {
        auto str = [&](const char* k) {
            const Plist* p = root.Get(k);
            return p ? p->AsString() : std::string();
        };
        devId_ = str("deviceID");
        devName_ = str("name");
        devModel_ = str("model");
        const std::string* ua = req.Header("User-Agent");
        LOGI("rtsp[%llu]: session from \"%s\" (%s %s, %s)", (unsigned long long)id_, devName_.c_str(), devModel_.c_str(),
             str("osVersion").c_str(), ua ? ua->c_str() : "?");

        if (eiv->AsData().size() >= 16) std::memcpy(aesIv_, eiv->AsData().data(), 16);
        uint8_t key[16] = {};
        if (!fp_.DecryptKey(ekey->AsData().data(), ekey->AsData().size(), key))
            LOGE("rtsp[%llu]: no FairPlay key message; stream decryption will fail", (unsigned long long)id_);
        bool oldProtocol = ua && ua->find("AirMyPC") != std::string::npos;
        if (pair_ != PairState::Initial && !oldProtocol) {
            crypto::Sha512 h;
            h.Update(key, 16);
            h.Update(ecdhSecret_, 32);
            uint8_t d[64];
            h.Final(d);
            std::memcpy(key, d, 16);
        }
        std::memcpy(aesKey_, key, 16);
        keysReady_ = true;

        uint16_t timingPort = 0;
        if (const Plist* tp = root.Get("timingPort")) timingPort = uint16_t(tp->AsUInt());
        if (const Plist* proto = root.Get("timingProtocol"))
            if (proto->AsString() != "NTP") LOGW("rtsp[%llu]: timingProtocol %s (expected NTP)", (unsigned long long)id_,
                                                  proto->AsString().c_str());
        if (ntp_) ntp_->Stop();
        ntp_.reset();
        if (timingPort) {
            ntp_ = std::make_unique<NtpClient>();
            if (!ntp_->Start(peer_, timingPort, [this] { Kick(); })) ntp_.reset();
        }
        res.Set("timingPort", Plist::Int(ntp_ ? ntp_->LocalPort() : 0));
        res.Set("eventPort", Plist::Int(0));
    }

    const Plist* streams = root.Get("streams");
    if (streams && streams->IsArray()) {
        Plist out = Plist::Array();
        for (const Plist& s : streams->Items()) {
            uint64_t type = s.Get("type") ? s.Get("type")->AsUInt() : 0;
            if (type == 110) {
                if (!keysReady_) {
                    LOGE("rtsp[%llu]: mirror SETUP before key exchange", (unsigned long long)id_);
                    resp.closeAfter = true;
                    continue;
                }
                uint64_t scid = s.Get("streamConnectionID") ? s.Get("streamConnectionID")->AsUInt() : 0;
                if (mirror_) mirror_->Stop();
                mirror_ = std::make_unique<MirrorStream>();
                MirrorStream::Callbacks cb;
                cb.onVideoSize = [this](int w, int h) {
                    if (srv_->ev_.videoSize) srv_->ev_.videoSize(id_, w, h);
                };
                cb.onEnded = [this] { NotifyMirrorStopped(); };
                if (!mirror_->Start(aesKey_, scid, srv_->video_, cb)) {
                    mirror_.reset();
                    resp.closeAfter = true;
                    continue;
                }
                srv_->ClaimMirror(shared_from_this());
                if (!mirroring_.exchange(true)) {
                    if (srv_->ev_.mirrorStarted) srv_->ev_.mirrorStarted(id_, devName_, devModel_);
                }
                Plist st = Plist::Dict();
                st.Set("dataPort", Plist::Int(mirror_->Port()));
                st.Set("type", Plist::Int(110));
                out.Append(st);
            } else if (type == 96) {
                if (!keysReady_) {
                    LOGE("rtsp[%llu]: audio SETUP before key exchange", (unsigned long long)id_);
                    resp.closeAfter = true;
                    continue;
                }
                int ct = s.Get("ct") ? int(s.Get("ct")->AsUInt()) : 0;
                int spf = s.Get("spf") ? int(s.Get("spf")->AsUInt()) : 0;
                int sr = s.Get("sr") ? int(s.Get("sr")->AsUInt()) : 0;
                uint16_t cport = s.Get("controlPort") ? uint16_t(s.Get("controlPort")->AsUInt()) : 0;
                uint64_t fmt = s.Get("audioFormat") ? s.Get("audioFormat")->AsUInt() : 0;
                LOGI("rtsp[%llu]: audio stream %s", (unsigned long long)id_, s.Describe().c_str());
                AudioFormat af = AudioFormat::FromSetup(ct, fmt, spf, sr);
                // Audio-only (Music, podcasts...) arrives as ALAC/AAC on a connection that is not
                // mirroring; mirroring audio is AAC-ELD next to a 110 stream.
                bool music = ct != 8 && !mirror_ && !mirroring_;
                if (audio_) {
                    audio_->Stop();
                    audio_.reset();
                }
                auto stream = std::make_unique<AudioStream>();
                if (music) stream->onPlaying = [this](bool playing, double at) { OnPlaying(playing, at); };
                if (!stream->Start(peer_, cport, af, music, aesKey_, aesIv_, srv_->audio_)) {
                    LOGE("rtsp[%llu]: audio stream setup failed", (unsigned long long)id_);
                    continue;
                }
                audio_ = std::move(stream);
                if (music) {
                    {
                        std::lock_guard<std::mutex> lk(npMu_);
                        bool fresh = !music_;
                        music_ = true;
                        npFormat_ = af;
                        if (fresh) {
                            npTrack_ = TrackInfo{};
                            npHaveProgress_ = npPlaying_ = false;
                            npArtworkSeq_ = 0;
                        }
                    }
                    srv_->ClaimAudio(shared_from_this());
                    LOGI("rtsp[%llu]: music from \"%s\": %s", (unsigned long long)id_, devName_.c_str(),
                         af.Describe().c_str());
                    NotifyNowPlaying();
                }
                Plist st = Plist::Dict();
                st.Set("dataPort", Plist::Int(audio_->DataPort()));
                st.Set("controlPort", Plist::Int(audio_->ControlPort()));
                st.Set("type", Plist::Int(96));
                out.Append(st);
            } else {
                LOGE("rtsp[%llu]: SETUP of unknown stream type %llu", (unsigned long long)id_, (unsigned long long)type);
                resp.closeAfter = true;
            }
        }
        res.Set("streams", out);
    }
    resp.SetPlist(res);
}

void AirPlayServer::Connection::GetParameter(const Request& req, Response& resp) {
    std::string ct = req.ContentType();
    if (ct.empty()) {
        resp.code = 451;
        resp.reason = "Parameter not understood";
        return;
    }
    if (ct == "text/parameters") {
        std::string body(req.body.begin(), req.body.end());
        if (body.find("volume") != std::string::npos) {
            char v[64];
            snprintf(v, sizeof(v), "volume: %9.6f\r\n", srv_->audio_ ? srv_->audio_->VolumeDb() : 0.0f);
            resp.body.assign(v, v + strlen(v));
            resp.Add("Content-Type", "text/parameters");
        }
    }
}

void AirPlayServer::Connection::SetParameter(const Request& req, Response& resp) {
    std::string ct = req.ContentType();
    if (ct.empty()) {
        resp.code = 451;
        resp.reason = "Parameter not understood";
        return;
    }
    bool music;
    {
        std::lock_guard<std::mutex> lk(npMu_);
        music = music_;
    }
    if (ct == "text/parameters") {
        std::string body(req.body.begin(), req.body.end());
        size_t pos = 0;
        while (pos < body.size()) {
            size_t e = body.find('\n', pos);
            std::string line = Trim(body.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
            pos = e == std::string::npos ? body.size() : e + 1;
            if (line.rfind("volume:", 0) == 0) {
                float db = float(atof(line.c_str() + 7));
                if (srv_->audio_) srv_->audio_->SetVolumeDb(db);
                LOGI("rtsp[%llu]: volume %.2f dB", (unsigned long long)id_, db);
                if (music) NotifyNowPlaying();
            } else if (line.rfind("progress:", 0) == 0) {
                OnProgress(Trim(line.substr(9)));
            }
        }
    } else if (ct == "application/x-dmap-tagged") {
        TrackInfo t;
        ParseDmap(req.body.data(), req.body.size(), t, 0);
        LOGI("rtsp[%llu]: now playing \"%s\" by \"%s\" from \"%s\"", (unsigned long long)id_, t.title.c_str(),
             t.artist.c_str(), t.album.c_str());
        {
            std::lock_guard<std::mutex> lk(npMu_);
            npTrack_.title = t.title;
            npTrack_.artist = t.artist;
            npTrack_.album = t.album;
            if (t.durationMs > 0) npTrack_.durationMs = t.durationMs;
        }
        NotifyNowPlaying();
    } else if (ct.rfind("image/", 0) == 0) {
        OnArtwork(ct, req.body);
    } else {
        LOGD("rtsp[%llu]: SET_PARAMETER %s ignored", (unsigned long long)id_, ct.c_str());
    }
}

double AirPlayServer::Connection::PositionLocked(double at) const {
    return npAnchorPos_ + (npPlaying_ ? std::max(0.0, at - npAnchorTime_) : 0.0);
}

void AirPlayServer::Connection::OnPlaying(bool playing, double at) {
    {
        std::lock_guard<std::mutex> lk(npMu_);
        if (npPlaying_ == playing) return;
        npAnchorPos_ = PositionLocked(at);
        npAnchorTime_ = at;
        npPlaying_ = playing;
    }
    LOGI("rtsp[%llu]: music %s", (unsigned long long)id_, playing ? "playing" : "paused");
    NotifyNowPlaying();
}

void AirPlayServer::Connection::OnProgress(const std::string& value) {
    // "start/current/end" RTP timestamps of the track at the stream's sample rate.
    unsigned long start = 0, cur = 0, end = 0;
    if (sscanf(value.c_str(), "%lu/%lu/%lu", &start, &cur, &end) != 3) return;
    {
        std::lock_guard<std::mutex> lk(npMu_);
        double sr = npFormat_.sampleRate > 0 ? npFormat_.sampleRate : 44100;
        int32_t elapsed = int32_t(uint32_t(cur) - uint32_t(start));
        uint32_t length = uint32_t(end) - uint32_t(start);
        npTrack_.durationMs = length ? int64_t(double(length) * 1000.0 / sr) : -1;
        npAnchorPos_ = std::max(0.0, double(elapsed) / sr);
        npAnchorTime_ = NowSeconds();
        npHaveProgress_ = true;
    }
    NotifyNowPlaying();
}

void AirPlayServer::Connection::OnArtwork(const std::string& type, const std::vector<uint8_t>& body) {
    std::string t = type.substr(0, type.find(';'));
    if (t == "image/none" || body.empty()) {
        {
            std::lock_guard<std::mutex> lk(npMu_);
            npArtworkSeq_ = 0;
        }
        NotifyNowPlaying();
        return;
    }
    std::wstring path = AppDataDir() + (t == "image/png" ? L"\\nowplaying.png" : L"\\nowplaying.jpg");
    if (!WriteFileAtomic(path, body)) {
        LOGW("rtsp[%llu]: could not save artwork", (unsigned long long)id_);
        return;
    }
    uint32_t seq = ++srv_->artworkSeq_;
    {
        std::lock_guard<std::mutex> lk(npMu_);
        npArtworkSeq_ = seq;
        npArtworkType_ = t;
        npArtworkPath_ = path;
    }
    LOGI("rtsp[%llu]: artwork %s, %zu bytes (#%u)", (unsigned long long)id_, t.c_str(), body.size(), seq);
    NotifyNowPlaying();
}

void AirPlayServer::Connection::SnapshotNowPlaying(NowPlaying& out) {
    std::lock_guard<std::mutex> lk(npMu_);
    out = NowPlaying{};
    out.session = id_;
    out.active = music_;
    out.device = devName_;
    out.title = npTrack_.title;
    out.artist = npTrack_.artist;
    out.album = npTrack_.album;
    out.durationMs = npTrack_.durationMs;
    if (npHaveProgress_) {
        double pos = PositionLocked(NowSeconds());
        if (out.durationMs > 0) pos = std::min(pos, out.durationMs / 1000.0);
        out.positionMs = int64_t(pos * 1000.0);
    }
    out.playing = npPlaying_;
    out.artworkSeq = npArtworkSeq_;
    out.artworkType = npArtworkType_;
    out.artworkPath = npArtworkPath_;
    out.volume = srv_->audio_ ? VolumeFraction(srv_->audio_->VolumeDb()) : 1.0f;
    out.controls = dacp_.Available();
    out.format = npFormat_;
}

std::string AirPlayServer::Connection::Media(const std::string& action, double value) {
    std::string cmd;
    if (action == "playpause") cmd = "playpause";
    else if (action == "play") cmd = "play";
    else if (action == "pause") cmd = "pause";
    else if (action == "next") cmd = "nextitem";
    else if (action == "prev" || action == "previous") cmd = "previtem";
    else if (action == "volume") {
        double v = std::clamp(value, 0.0, 1.0);
        float db = v < 0.001 ? -144.0f : float(-30.0 + 30.0 * v);
        if (srv_->audio_) srv_->audio_->SetVolumeDb(db);  // heard immediately even without a remote
        NotifyNowPlaying();
        // Also move the sender's own slider when it accepts remote volume (best effort).
        char b[64];
        snprintf(b, sizeof(b), "setproperty?dmcp.device-volume=%.6f", double(db));
        if (dacp_.Available()) dacp_.Send(b);
        return std::string();
    } else {
        return "failed";
    }
    return dacp_.Send(cmd);
}

void AirPlayServer::Connection::Teardown(const Request& req, Response& resp) {
    bool t96 = false, t110 = false;
    Plist root;
    if (Plist::Parse(req.body.data(), req.body.size(), root)) {
        if (const Plist* streams = root.Get("streams")) {
            for (const Plist& s : streams->Items()) {
                uint64_t type = s.Get("type") ? s.Get("type")->AsUInt() : 0;
                if (type == 96) t96 = true;
                if (type == 110) t110 = true;
            }
        }
    }
    LOGI("rtsp[%llu]: TEARDOWN%s%s", (unsigned long long)id_, t96 ? " audio" : "", t110 ? " mirror" : "");
    resp.Add("Connection", "close");
    if (t96) StopAudio();
    if (t110) StopMirror();
    if (!t96 && !t110) {
        // Full teardown (iOS 27+ no longer sends a separate 110 teardown when mirroring stops).
        StopAudio();
        StopMirror();
    }
}

void AirPlayServer::Connection::NotifyMirrorStopped() {
    if (mirroring_.exchange(false)) {
        srv_->ReleaseMirror(this);
        if (srv_->ev_.mirrorStopped) srv_->ev_.mirrorStopped(id_);
    }
}

void AirPlayServer::Connection::StopMirror() {
    if (mirror_) {
        mirror_->Stop();
        mirror_.reset();
    }
    NotifyMirrorStopped();
}

void AirPlayServer::Connection::StopAudio() {
    if (audio_) {
        audio_->Stop();
        audio_.reset();
    }
    bool wasMusic;
    {
        std::lock_guard<std::mutex> lk(npMu_);
        wasMusic = music_;
        music_ = false;
        npPlaying_ = false;
    }
    if (wasMusic) {
        srv_->ReleaseAudio(this);
        LOGI("rtsp[%llu]: music session ended", (unsigned long long)id_);
        NotifyNowPlaying();
    }
}

void AirPlayServer::Connection::StopStreams() {
    StopMirror();
    StopAudio();
    if (ntp_) {
        ntp_->Stop();
        ntp_.reset();
    }
}

// ---------------------------------------------------------------------------------------------

AirPlayServer::AirPlayServer() = default;
AirPlayServer::~AirPlayServer() { Stop(); }

bool AirPlayServer::Start(const ReceiverInfo& info, uint16_t preferredPort, FrameSink* video, AudioOutput* audio,
                          Events ev) {
    info_ = info;
    video_ = video;
    audio_ = audio;
    ev_ = std::move(ev);
    for (uint16_t candidate : {preferredPort, uint16_t(7000), uint16_t(7100), uint16_t(7200), uint16_t(0)}) {
        uint16_t p = candidate;
        listen_ = net::TcpListen(p, true);
        if (listen_ != INVALID_SOCKET) {
            port_ = p;
            break;
        }
        LOGW("rtsp: port %u unavailable (%d)", candidate, WSAGetLastError());
    }
    if (listen_ == INVALID_SOCKET) return false;
    stop_ = false;
    acceptThread_ = std::thread(&AirPlayServer::AcceptLoop, this);
    LOGI("rtsp: listening on TCP %u", port_);
    return true;
}

void AirPlayServer::Stop() {
    stop_ = true;
    if (acceptThread_.joinable()) acceptThread_.join();
    std::vector<std::shared_ptr<Connection>> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        all = conns_;
    }
    for (auto& c : all) c->Kick();
    for (auto& c : all) c->Join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        conns_.clear();
    }
    net::CloseSocket(listen_);
}

void AirPlayServer::AcceptLoop() {
    while (!stop_) {
        int w = net::WaitReadable(listen_, 500);
        Reap();
        if (w <= 0) continue;
        sockaddr_storage peer{};
        int len = sizeof(peer);
        SOCKET s = accept(listen_, reinterpret_cast<sockaddr*>(&peer), &len);
        if (s == INVALID_SOCKET) continue;
        net::SetNoDelay(s);
        std::shared_ptr<Connection> c;
        {
            std::lock_guard<std::mutex> lk(mu_);
            c = std::make_shared<Connection>(this, s, peer, nextId_++);
            conns_.push_back(c);
        }
        c->Start();
    }
}

void AirPlayServer::Reap() {
    std::vector<std::shared_ptr<Connection>> dead;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto it = conns_.begin(); it != conns_.end();) {
            if ((*it)->Done()) {
                dead.push_back(*it);
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& d : dead) d->Join();
}

void AirPlayServer::ClaimMirror(const std::shared_ptr<Connection>& c) {
    std::shared_ptr<Connection> old;
    {
        std::lock_guard<std::mutex> lk(mu_);
        old = mirror_.lock();
        mirror_ = c;
    }
    if (old && old != c) {
        LOGI("rtsp: connection %llu takes over mirroring from %llu", (unsigned long long)c->Id(),
             (unsigned long long)old->Id());
        old->Kick();
    }
}

void AirPlayServer::ReleaseMirror(Connection* c) {
    std::lock_guard<std::mutex> lk(mu_);
    auto m = mirror_.lock();
    if (m.get() == c) mirror_.reset();
}

void AirPlayServer::ClaimAudio(const std::shared_ptr<Connection>& c) {
    std::lock_guard<std::mutex> lk(mu_);
    audioConn_ = c;
}

void AirPlayServer::ReleaseAudio(Connection* c) {
    std::lock_guard<std::mutex> lk(mu_);
    auto a = audioConn_.lock();
    if (a.get() == c) audioConn_.reset();
}

bool AirPlayServer::GetNowPlaying(NowPlaying& out) {
    std::shared_ptr<Connection> c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        c = audioConn_.lock();
    }
    out = NowPlaying{};
    if (!c) return false;
    c->SnapshotNowPlaying(out);
    return out.active;
}

std::string AirPlayServer::MediaCommand(const std::string& action, double value) {
    std::shared_ptr<Connection> c;
    {
        std::lock_guard<std::mutex> lk(mu_);
        c = audioConn_.lock();
    }
    if (!c) return "no-session";
    return c->Media(action, value);
}

void AirPlayServer::DisconnectMirror() {
    std::shared_ptr<Connection> m;
    {
        std::lock_guard<std::mutex> lk(mu_);
        m = mirror_.lock();
    }
    if (m) {
        LOGI("rtsp: disconnecting mirroring sender %llu", (unsigned long long)m->Id());
        m->Kick();
    }
}
