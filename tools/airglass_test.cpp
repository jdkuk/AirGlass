// airglass_test: verification tool for AirGlass.
//   airglass_test bplist <dir>              bplist round-trip + cross-check files with Python plistlib
//   airglass_test mdns                      browse _airplay._tcp / _raop._tcp and print records
//   airglass_test stream <a.mp4> [b.mp4] [--host H] [--port P] [--seconds S] [--audible]
//                                           act as an AirPlay sender (full handshake, mirror + audio)
//   airglass_test music <alac.m4a> [--art a.jpg] [--pause-at S] [--pause-for S] [--linger S] [--port P]
//                                           audio-only sender (Music app): ALAC + metadata + DACP remote
//   airglass_test resampler                 resampler quality vs. the old linear interpolation
//   airglass_test resolve <instance> <type> one-shot mDNS SRV lookup
#include "common.h"

#include <cstdio>
#include <map>

#include "crypto/crypto.h"
#include "crypto/fairplay.h"
#include "license.h"
#include "media/resampler.h"
#include "net/bplist.h"
#include "net/mdns.h"
#include "net/netutil.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include "playfair.h"
}

static int Fail(const char* msg) {
    printf("FAIL: %s\n", msg);
    return 1;
}

// ---------------------------------------------------------------------------------------------
// bplist

static std::vector<uint8_t> ReadFile(const std::string& p) {
    std::vector<uint8_t> v;
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    v.resize(size_t(n));
    if (n > 0) fread(v.data(), 1, size_t(n), f);
    fclose(f);
    return v;
}

static int CmdBplist(const std::string& dir) {
    // 1) Build a plist exercising every type we use, write it for Python to verify.
    Plist root = Plist::Dict();
    root.Set("small", Plist::Int(7));
    root.Set("u16", Plist::Int(4000));
    root.Set("u32", Plist::Int(123456789));
    root.Set("big", Plist::Int(0x0123456789ABCDEFull));
    root.Set("huge", Plist::Int(0xF123456789ABCDEFull));  // > INT64_MAX: 16-byte int
    root.Set("real", Plist::Real(1.0 / 120.0));
    root.Set("yes", Plist::Bool(true));
    root.Set("no", Plist::Bool(false));
    root.Set("ascii", Plist::String("AppleTV3,2"));
    root.Set("unicode", Plist::String("Taylor\xE2\x80\x99s iPhone \xF0\x9F\x93\xB1"));
    uint8_t d[72];
    for (int i = 0; i < 72; ++i) d[i] = uint8_t(i * 7);
    root.Set("data", Plist::Data(d, sizeof(d)));
    Plist arr = Plist::Array();
    for (int i = 0; i < 20; ++i) {
        Plist e = Plist::Dict();
        e.Set("type", Plist::Int(uint64_t(100 + i)));
        e.Set("name", Plist::String("stream" + std::to_string(i)));
        arr.Append(e);
    }
    root.Set("streams", arr);
    auto bytes = root.Serialize();
    std::string outPath = dir + "\\cpp.bplist";
    FILE* f = fopen(outPath.c_str(), "wb");
    if (!f) return Fail("cannot write cpp.bplist");
    fwrite(bytes.data(), 1, bytes.size(), f);
    fclose(f);
    printf("wrote %s (%zu bytes)\n", outPath.c_str(), bytes.size());

    // 2) Round trip.
    Plist back;
    if (!Plist::Parse(bytes.data(), bytes.size(), back)) return Fail("round-trip parse");
    if (back.Get("huge")->AsUInt() != 0xF123456789ABCDEFull) return Fail("huge int");
    if (back.Get("big")->AsUInt() != 0x0123456789ABCDEFull) return Fail("big int");
    if (back.Get("unicode")->AsString() != root.Get("unicode")->AsString()) return Fail("unicode");
    if (back.Get("streams")->Items().size() != 20) return Fail("array");
    if (std::fabs(back.Get("real")->AsReal() - 1.0 / 120.0) > 1e-12) return Fail("real");
    printf("round-trip OK: %s\n", back.Describe().substr(0, 160).c_str());

    // 3) Parse the Python-produced file if present.
    auto py = ReadFile(dir + "\\py.bplist");
    if (!py.empty()) {
        Plist p;
        if (!Plist::Parse(py.data(), py.size(), p)) return Fail("parse python bplist");
        printf("python bplist: %s\n", p.Describe().c_str());
        const Plist* s = p.Get("streams");
        if (!s || s->Items().empty()) return Fail("python streams");
        uint64_t id = s->Items()[0].Get("streamConnectionID")->AsUInt();
        printf("streamConnectionID = %llu\n", (unsigned long long)id);
        if (id != 11653283748123456789ull) return Fail("python 16-byte int");
        if (p.Get("name")->AsString() != "Taylor\xE2\x80\x99s iPhone") return Fail("python utf16 string");
        if (p.Get("ekey")->AsData().size() != 72) return Fail("python data");
    }
    printf("PASS bplist\n");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// mDNS browse

static bool ReadName(const uint8_t* p, size_t n, size_t& off, std::string& out) {
    out.clear();
    size_t pos = off;
    bool jumped = false;
    int jumps = 0;
    while (pos < n) {
        uint8_t len = p[pos++];
        if (len == 0) {
            if (!jumped) off = pos;
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            if (pos >= n || ++jumps > 16) return false;
            size_t ptr = (size_t(len & 0x3F) << 8) | p[pos++];
            if (!jumped) off = pos;
            jumped = true;
            pos = ptr;
            continue;
        }
        if (pos + len > n) return false;
        if (!out.empty()) out += '.';
        out.append(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
    }
    return false;
}

static void PutName(std::vector<uint8_t>& o, const std::string& name) {
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) dot = name.size();
        o.push_back(uint8_t(dot - start));
        o.insert(o.end(), name.begin() + long(start), name.begin() + long(dot));
        start = dot + 1;
    }
    o.push_back(0);
}

static int CmdMdns() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in any{};
    any.sin_family = AF_INET;
    bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    std::vector<uint8_t> q = {0x12, 0x34, 0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0, 0, 0};
    for (const char* n : {"_airplay._tcp.local", "_raop._tcp.local"}) {
        PutName(q, n);
        q.push_back(0);
        q.push_back(12);  // PTR
        q.push_back(0);
        q.push_back(1);
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    sendto(s, reinterpret_cast<const char*>(q.data()), int(q.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    printf("query sent; listening 3 s for answers...\n");
    double end = NowSeconds() + 3.0;
    int answers = 0;
    bool sawAirplay = false, sawRaop = false, sawA = false;
    while (NowSeconds() < end) {
        if (net::WaitReadable(s, 200) <= 0) continue;
        uint8_t buf[9000];
        sockaddr_in from{};
        int flen = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n < 12) continue;
        char ip[32];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
        uint16_t qd = RdBE16(buf + 4), an = RdBE16(buf + 6), ns = RdBE16(buf + 8), ar = RdBE16(buf + 10);
        printf("response from %s:%u (%u answers, %u additional)\n", ip, ntohs(from.sin_port), an, ar);
        size_t off = 12;
        std::string name;
        for (int i = 0; i < qd; ++i) {
            ReadName(buf, size_t(n), off, name);
            off += 4;
        }
        for (int i = 0; i < an + ns + ar && off < size_t(n); ++i) {
            if (!ReadName(buf, size_t(n), off, name) || off + 10 > size_t(n)) break;
            uint16_t type = RdBE16(buf + off), cls = RdBE16(buf + off + 2);
            uint32_t ttl = RdBE32(buf + off + 4);
            uint16_t rdlen = RdBE16(buf + off + 8);
            off += 10;
            size_t rd = off;
            off += rdlen;
            ++answers;
            if (type == 12) {
                size_t r = rd;
                std::string target;
                ReadName(buf, size_t(n), r, target);
                printf("  PTR %s -> %s (ttl %u)\n", name.c_str(), target.c_str(), ttl);
                if (name == "_airplay._tcp.local") sawAirplay = true;
                if (name == "_raop._tcp.local") sawRaop = true;
            } else if (type == 33) {
                size_t r = rd + 6;
                std::string target;
                ReadName(buf, size_t(n), r, target);
                printf("  SRV %s -> %s:%u (ttl %u%s)\n", name.c_str(), target.c_str(), RdBE16(buf + rd + 4), ttl,
                       (cls & 0x8000) ? ", flush" : "");
            } else if (type == 16) {
                printf("  TXT %s:", name.c_str());
                size_t r = rd;
                while (r < rd + rdlen) {
                    uint8_t l = buf[r++];
                    printf(" [%.*s]", int(l), reinterpret_cast<const char*>(buf + r));
                    r += l;
                }
                printf("\n");
            } else if (type == 1 && rdlen == 4) {
                printf("  A   %s -> %u.%u.%u.%u\n", name.c_str(), buf[rd], buf[rd + 1], buf[rd + 2], buf[rd + 3]);
                sawA = true;
            } else {
                printf("  type %u %s\n", type, name.c_str());
            }
        }
    }
    closesocket(s);
    printf("%d records\n", answers);
    if (sawAirplay && sawRaop && sawA) {
        printf("PASS mdns\n");
        return 0;
    }
    return Fail("missing _airplay/_raop/A records");
}

// ---------------------------------------------------------------------------------------------
// AirPlay sender

class Rtsp {
public:
    SOCKET s = INVALID_SOCKET;
    int cseq = 0;
    std::string host;
    std::string always;  // extra headers sent with every request (e.g. DACP-ID / Active-Remote)
    std::vector<uint8_t> buf;

    bool Connect(const std::string& h, uint16_t port) {
        host = h;
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(h.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
        s = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
        bool ok = connect(s, res->ai_addr, int(res->ai_addrlen)) == 0;
        freeaddrinfo(res);
        return ok;
    }

    int Request(const std::string& method, const std::string& uri, const std::string& ctype,
                const std::vector<uint8_t>& body, std::vector<uint8_t>& respBody,
                std::map<std::string, std::string>* respHeaders = nullptr, const std::string& extra = "") {
        std::string req = method + " " + uri + " RTSP/1.0\r\nCSeq: " + std::to_string(++cseq) +
                          "\r\nUser-Agent: AirPlay/780.10.1\r\nX-Apple-ProtocolVersion: 1\r\n" + always + extra;
        if (!ctype.empty()) req += "Content-Type: " + ctype + "\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
        std::vector<uint8_t> out(req.begin(), req.end());
        out.insert(out.end(), body.begin(), body.end());
        if (!net::SendAll(s, out.data(), out.size())) return -1;
        // read response
        size_t hdrEnd;
        for (;;) {
            static const char sep[] = "\r\n\r\n";
            auto it = std::search(buf.begin(), buf.end(), sep, sep + 4);
            if (it != buf.end()) {
                hdrEnd = size_t(it - buf.begin()) + 4;
                break;
            }
            char tmp[8192];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) return -1;
            buf.insert(buf.end(), tmp, tmp + n);
        }
        std::string head(buf.begin(), buf.begin() + long(hdrEnd));
        int code = atoi(head.c_str() + head.find(' ') + 1);
        size_t len = 0;
        std::map<std::string, std::string> hdrs;
        size_t pos = head.find("\r\n") + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            if (e == pos || e == std::string::npos) break;
            std::string line = head.substr(pos, e - pos);
            size_t c = line.find(':');
            if (c != std::string::npos) {
                std::string k = line.substr(0, c), v = line.substr(c + 2);
                hdrs[k] = v;
                if (_stricmp(k.c_str(), "Content-Length") == 0) len = size_t(atoi(v.c_str()));
            }
            pos = e + 2;
        }
        while (buf.size() < hdrEnd + len) {
            char tmp[8192];
            int n = recv(s, tmp, sizeof(tmp), 0);
            if (n <= 0) return -1;
            buf.insert(buf.end(), tmp, tmp + n);
        }
        respBody.assign(buf.begin() + long(hdrEnd), buf.begin() + long(hdrEnd + len));
        buf.erase(buf.begin(), buf.begin() + long(hdrEnd + len));
        if (respHeaders) *respHeaders = hdrs;
        return code;
    }
};

struct Media {
    AVFormatContext* fmt = nullptr;
    int vIdx = -1, aIdx = -1;
    std::vector<uint8_t> avcc;
    int w = 0, h = 0;
    bool Open(const std::string& path) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            auto* par = fmt->streams[i]->codecpar;
            if (par->codec_type == AVMEDIA_TYPE_VIDEO && vIdx < 0) {
                vIdx = int(i);
                avcc.assign(par->extradata, par->extradata + par->extradata_size);
                w = par->width;
                h = par->height;
            }
            if (par->codec_type == AVMEDIA_TYPE_AUDIO && aIdx < 0) aIdx = int(i);
        }
        return vIdx >= 0 && !avcc.empty() && avcc[0] == 1;
    }
    ~Media() {
        if (fmt) avformat_close_input(&fmt);
    }
};

static void NtpResponder(SOCKET s, std::atomic<bool>* stop, std::atomic<int>* polls) {
    uint8_t b[128];
    while (!*stop) {
        if (net::WaitReadable(s, 100) <= 0) continue;
        sockaddr_storage from{};
        int flen = sizeof(from);
        int n = recvfrom(s, reinterpret_cast<char*>(b), sizeof(b), 0, reinterpret_cast<sockaddr*>(&from), &flen);
        if (n >= 32 && (b[1] & 0x7F) == 0x52) {
            uint8_t r[32] = {0x80, 0xd3, 0x00, 0x07};
            std::memcpy(r + 8, b + 24, 8);
            uint64_t now = uint64_t(NowSeconds() * 4294967296.0);
            WrBE64(r + 16, now);
            WrBE64(r + 24, now);
            sendto(s, reinterpret_cast<const char*>(r), 32, 0, reinterpret_cast<sockaddr*>(&from), flen);
            ++*polls;
        }
    }
}

// Sender side of the session handshake: /info, pair-setup, pair-verify, FairPlay, SETUP (session).
struct SenderSession {
    Rtsp rtsp;
    std::string host;
    uint8_t aesKey[16]{}, eiv[16]{};
    uint16_t ntpPort = 0;
    SOCKET ntpSock = INVALID_SOCKET;
    std::atomic<bool> stopNtp{false};
    std::atomic<int> ntpPolls{0};
    std::thread ntpThread;

    ~SenderSession() {
        stopNtp = true;
        if (ntpThread.joinable()) ntpThread.join();
        if (ntpSock != INVALID_SOCKET) closesocket(ntpSock);
        if (rtsp.s != INVALID_SOCKET) closesocket(rtsp.s);
    }
    std::string Url() const { return "rtsp://" + host + "/1234567890"; }
    bool Open(const std::string& h, uint16_t port, bool mirroring, const char* deviceName);
};

bool SenderSession::Open(const std::string& h, uint16_t port, bool mirroring, const char* deviceName) {
    auto fail = [](const char* m) {
        Fail(m);
        return false;
    };
    host = h;
    if (!rtsp.Connect(host, port)) return fail("connect");
    std::vector<uint8_t> resp;

    int code = rtsp.Request("GET", "/info", "", {}, resp);
    Plist info;
    if (code != 200 || !Plist::Parse(resp.data(), resp.size(), info)) return fail("GET /info");
    printf("[info] name=%s model=%s features=0x%llx\n", info.Get("name")->AsString().c_str(),
           info.Get("model")->AsString().c_str(), (unsigned long long)info.Get("features")->AsUInt());
    if (const Plist* fmts = info.Get("audioFormats"))
        for (const Plist& f : fmts->Items())
            printf("[info] audioFormats type %llu: input 0x%llx\n", (unsigned long long)f.Get("type")->AsUInt(),
                   (unsigned long long)f.Get("audioInputFormats")->AsUInt());
    std::vector<uint8_t> serverEdPk = info.Get("pk")->AsData();

    uint8_t cSeed[32], cEdPub[32], cEdPriv[64];
    crypto::RandomBytes(cSeed, 32);
    crypto::Ed25519KeypairFromSeed(cSeed, cEdPub, cEdPriv);
    code = rtsp.Request("POST", "/pair-setup", "application/octet-stream", std::vector<uint8_t>(cEdPub, cEdPub + 32), resp);
    if (code != 200 || resp.size() != 32 || std::memcmp(resp.data(), serverEdPk.data(), 32) != 0)
        return fail("pair-setup");

    uint8_t cX[32], cXPub[32], shared[32];
    crypto::RandomBytes(cX, 32);
    crypto::X25519PublicKey(cXPub, cX);
    std::vector<uint8_t> pv = {1, 0, 0, 0};
    pv.insert(pv.end(), cXPub, cXPub + 32);
    pv.insert(pv.end(), cEdPub, cEdPub + 32);
    code = rtsp.Request("POST", "/pair-verify", "application/octet-stream", pv, resp);
    if (code != 200 || resp.size() != 96) return fail("pair-verify 1");
    uint8_t sXPub[32];
    std::memcpy(sXPub, resp.data(), 32);
    crypto::X25519(shared, cX, sXPub);
    auto derive = [&](const char* salt, uint8_t out[16]) {
        crypto::Sha512 hh;
        hh.Update(salt, strlen(salt));
        hh.Update(shared, 32);
        uint8_t d[64];
        hh.Final(d);
        std::memcpy(out, d, 16);
    };
    uint8_t pk[16], piv[16];
    derive("Pair-Verify-AES-Key", pk);
    derive("Pair-Verify-AES-IV", piv);
    uint8_t ssig[64];
    std::memcpy(ssig, resp.data() + 32, 64);
    {
        crypto::AesCtr c;
        c.Init(pk, piv);
        c.Process(ssig, 64);
    }
    uint8_t msg[64];
    std::memcpy(msg, sXPub, 32);
    std::memcpy(msg + 32, cXPub, 32);
    if (!crypto::Ed25519Verify(ssig, msg, 64, serverEdPk.data())) return fail("server pair-verify signature");
    std::memcpy(msg, cXPub, 32);
    std::memcpy(msg + 32, sXPub, 32);
    uint8_t csig[64];
    crypto::Ed25519Sign(csig, msg, 64, cEdPub, cEdPriv);
    {
        crypto::AesCtr c;
        c.Init(pk, piv);
        uint8_t skip[64] = {};
        c.Process(skip, 64);
        c.Process(csig, 64);
    }
    pv = {0, 0, 0, 0};
    pv.insert(pv.end(), csig, csig + 64);
    if (rtsp.Request("POST", "/pair-verify", "application/octet-stream", pv, resp) != 200) return fail("pair-verify 2");
    printf("[pair] pair-setup + pair-verify OK\n");

    std::vector<uint8_t> fp1 = {0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x02, 0x00, 0x01, 0xbb};
    code = rtsp.Request("POST", "/fp-setup", "application/octet-stream", fp1, resp, nullptr, "X-Apple-ET: 32\r\n");
    if (code != 200 || resp.size() != 142 || std::memcmp(resp.data(), "FPLY", 4) != 0) return fail("fp-setup 1");
    std::vector<uint8_t> fp2(164);
    crypto::RandomBytes(fp2.data(), fp2.size());
    const uint8_t hdr2[] = {0x46, 0x50, 0x4c, 0x59, 0x03, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x98};
    std::memcpy(fp2.data(), hdr2, sizeof(hdr2));
    fp2[12] = fp1[14];  // the key message carries the FairPlay mode chosen in step 1
    code = rtsp.Request("POST", "/fp-setup", "application/octet-stream", fp2, resp, nullptr, "X-Apple-ET: 32\r\n");
    if (code != 200 || resp.size() != 32 || std::memcmp(resp.data() + 12, fp2.data() + 144, 20) != 0)
        return fail("fp-setup 2");
    printf("[fairplay] handshake OK\n");

    ntpSock = net::UdpBind(ntpPort);
    ntpThread = std::thread(NtpResponder, ntpSock, &stopNtp, &ntpPolls);

    uint8_t ekey[72];
    crypto::RandomBytes(ekey, 72);
    crypto::RandomBytes(eiv, 16);
    Plist s1 = Plist::Dict();
    s1.Set("ekey", Plist::Data(ekey, 72));
    s1.Set("eiv", Plist::Data(eiv, 16));
    s1.Set("et", Plist::Int(32));
    s1.Set("timingProtocol", Plist::String("NTP"));
    s1.Set("timingPort", Plist::Int(ntpPort));
    s1.Set("deviceID", Plist::String("12:34:56:78:9A:BC"));
    s1.Set("macAddress", Plist::String("12:34:56:78:9A:BD"));
    s1.Set("name", Plist::String(deviceName));
    s1.Set("model", Plist::String("iPhone16,2"));
    s1.Set("osName", Plist::String("iPhone OS"));
    s1.Set("osVersion", Plist::String("27.0"));
    s1.Set("sourceVersion", Plist::String("780.10.1"));
    s1.Set("isScreenMirroringSession", Plist::Bool(mirroring));
    code = rtsp.Request("SETUP", Url(), "application/x-apple-binary-plist", s1.Serialize(), resp);
    Plist r1;
    if (code != 200 || !Plist::Parse(resp.data(), resp.size(), r1)) return fail("SETUP 1");
    printf("[setup] %s\n", r1.Describe().c_str());

    // Session key, computed exactly as the receiver must: FairPlay-decrypt, then hash with ECDH secret.
    uint8_t fpKey[16];
    {
        uint8_t m[164], e[72];
        std::memcpy(m, fp2.data(), 164);
        std::memcpy(e, ekey, 72);
        playfair_decrypt(m, e, fpKey);
    }
    crypto::Sha512 hk;
    hk.Update(fpKey, 16);
    hk.Update(shared, 32);
    uint8_t d[64];
    hk.Final(d);
    std::memcpy(aesKey, d, 16);
    return true;
}

static int CmdStream(int argc, char** argv) {
    std::vector<std::string> files;
    std::string host = "127.0.0.1";
    uint16_t port = 7000;
    double seconds = 0;
    bool audible = false;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = uint16_t(atoi(argv[++i]));
        else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--audible") audible = true;
        else files.push_back(a);
    }
    if (files.empty()) return Fail("no input files");

    SenderSession ss;
    if (!ss.Open(host, port, true, "AirGlass Test iPhone")) return 1;
    Rtsp& rtsp = ss.rtsp;
    const uint8_t* aesKey = ss.aesKey;
    uint8_t* eiv = ss.eiv;
    std::vector<uint8_t> resp;

    std::map<std::string, std::string> hdrs;
    int code = rtsp.Request("RECORD", "rtsp://" + host + "/1234567890", "", {}, resp, &hdrs);
    printf("[record] %d Audio-Latency=%s\n", code, hdrs["Audio-Latency"].c_str());

    // Media
    std::vector<std::unique_ptr<Media>> media;
    for (auto& f : files) {
        auto m = std::make_unique<Media>();
        if (!m->Open(f)) return Fail(("cannot open " + f).c_str());
        printf("[media] %s: %dx%d, avcC %zu bytes, audio=%s\n", f.c_str(), m->w, m->h, m->avcc.size(),
               m->aIdx >= 0 ? "yes" : "no");
        media.push_back(std::move(m));
    }

    // SETUP mirror stream
    uint64_t scid = 0x9E3779B97F4A7C15ull;  // top bit set -> 16-byte bplist integer on the wire
    Plist s2 = Plist::Dict();
    {
        Plist st = Plist::Dict();
        st.Set("type", Plist::Int(110));
        st.Set("streamConnectionID", Plist::Int(scid));
        st.Set("latencyMs", Plist::Int(90));
        Plist arr = Plist::Array();
        arr.Append(st);
        s2.Set("streams", arr);
    }
    code = rtsp.Request("SETUP", "rtsp://" + host + "/1234567890", "application/x-apple-binary-plist", s2.Serialize(), resp);
    Plist r2;
    if (code != 200 || !Plist::Parse(resp.data(), resp.size(), r2)) return Fail("SETUP mirror");
    uint16_t dataPort = uint16_t(r2.Get("streams")->Items()[0].Get("dataPort")->AsUInt());
    printf("[setup] mirror data port %u\n", dataPort);

    // Volume + audio stream
    std::string vol = audible ? "volume: -18.000000\r\n" : "volume: -144.000000\r\n";
    rtsp.Request("SET_PARAMETER", "rtsp://" + host + "/1234567890", "text/parameters",
                 std::vector<uint8_t>(vol.begin(), vol.end()), resp);
    uint16_t audioData = 0, ctrlPortLocal = 0;
    SOCKET ctrlSock = net::UdpBind(ctrlPortLocal);
    bool haveAudio = media[0]->aIdx >= 0;
    if (haveAudio) {
        Plist s3 = Plist::Dict();
        Plist st = Plist::Dict();
        st.Set("type", Plist::Int(96));
        st.Set("ct", Plist::Int(4));
        st.Set("spf", Plist::Int(1024));
        st.Set("audioFormat", Plist::Int(0x400000));
        st.Set("controlPort", Plist::Int(ctrlPortLocal));
        st.Set("usingScreen", Plist::Bool(true));
        st.Set("isMedia", Plist::Bool(true));
        Plist arr = Plist::Array();
        arr.Append(st);
        s3.Set("streams", arr);
        code = rtsp.Request("SETUP", "rtsp://" + host + "/1234567890", "application/x-apple-binary-plist", s3.Serialize(), resp);
        Plist r3;
        if (code != 200 || !Plist::Parse(resp.data(), resp.size(), r3)) return Fail("SETUP audio");
        audioData = uint16_t(r3.Get("streams")->Items()[0].Get("dataPort")->AsUInt());
        printf("[setup] audio data port %u\n", audioData);
    }

    // Connect the mirror data socket.
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    getaddrinfo(host.c_str(), std::to_string(dataPort).c_str(), &hints, &res);
    SOCKET ds = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    if (connect(ds, res->ai_addr, int(res->ai_addrlen)) != 0) return Fail("connect mirror data port");
    freeaddrinfo(res);
    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in audioTo{};
    audioTo.sin_family = AF_INET;
    audioTo.sin_port = htons(audioData);
    inet_pton(AF_INET, host == "localhost" ? "127.0.0.1" : host.c_str(), &audioTo.sin_addr);

    // Stream keys.
    std::string skey = "AirPlayStreamKey" + std::to_string(scid), siv = "AirPlayStreamIV" + std::to_string(scid);
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
    crypto::AesCtr ctr;
    ctr.Init(hk, hi);
    crypto::AesCbc cbc;
    cbc.Init(aesKey);

    auto sendCodec = [&](const Media& m) {
        uint8_t h[128] = {};
        WrLE32(h, uint32_t(m.avcc.size()));
        h[4] = 0x01;
        h[6] = 0x16;
        h[7] = 0x01;
        float fw = float(m.w), fh = float(m.h);
        for (int off : {16, 40, 56}) {
            std::memcpy(h + off, &fw, 4);
            std::memcpy(h + off + 4, &fh, 4);
        }
        net::SendAll(ds, h, 128);
        net::SendAll(ds, m.avcc.data(), m.avcc.size());
    };

    uint16_t seq = 1000;
    uint32_t rtpTs = 0;
    double t0 = NowSeconds();
    double segStart = 0;
    double lastFeedback = t0;
    int videoFrames = 0, audioPackets = 0;
    for (size_t mi = 0; mi < media.size(); ++mi) {
        Media& m = *media[mi];
        sendCodec(m);
        printf("[stream] segment %zu: %dx%d\n", mi, m.w, m.h);
        AVPacket* pkt = av_packet_alloc();
        AVRational vtb = m.fmt->streams[m.vIdx]->time_base;
        AVRational atb = m.aIdx >= 0 ? m.fmt->streams[m.aIdx]->time_base : AVRational{1, 1};
        double segT0 = NowSeconds();
        while (av_read_frame(m.fmt, pkt) >= 0) {
            bool isV = pkt->stream_index == m.vIdx, isA = pkt->stream_index == m.aIdx && mi == 0;
            double pts = (pkt->pts == AV_NOPTS_VALUE ? 0 : double(pkt->pts)) * av_q2d(isV ? vtb : atb);
            if (seconds > 0 && segStart + pts > seconds) {
                av_packet_unref(pkt);
                break;
            }
            double due = segT0 + pts;
            double now = NowSeconds();
            if (due > now) Sleep(DWORD((due - now) * 1000));
            if (isV) {
                std::vector<uint8_t> p(pkt->data, pkt->data + pkt->size);
                ctr.Process(p.data(), p.size());
                uint8_t h[128] = {};
                WrLE32(h, uint32_t(p.size()));
                h[4] = 0x00;
                h[5] = (pkt->flags & AV_PKT_FLAG_KEY) ? 0x10 : 0x00;
                WrLE64(h + 8, uint64_t((segStart + pts) * 4294967296.0));
                if (!net::SendAll(ds, h, 128) || !net::SendAll(ds, p.data(), p.size())) {
                    av_packet_unref(pkt);
                    return Fail("mirror socket closed");
                }
                ++videoFrames;
            } else if (isA && haveAudio) {
                std::vector<uint8_t> rtp(12 + size_t(pkt->size));
                rtp[0] = 0x80;
                rtp[1] = 0x60;
                WrBE16(&rtp[2], seq++);
                WrBE32(&rtp[4], rtpTs);
                rtpTs += 1024;
                std::memcpy(&rtp[12], pkt->data, size_t(pkt->size));
                size_t enc = size_t(pkt->size) / 16 * 16;
                cbc.Encrypt(eiv, &rtp[12], &rtp[12], enc);
                for (int copy = 0; copy < 2; ++copy)  // redundant copies, like iOS
                    sendto(us, reinterpret_cast<const char*>(rtp.data()), int(rtp.size()), 0,
                           reinterpret_cast<sockaddr*>(&audioTo), sizeof(audioTo));
                ++audioPackets;
            }
            av_packet_unref(pkt);
            if (NowSeconds() - lastFeedback > 2.0) {
                lastFeedback = NowSeconds();
                rtsp.Request("POST", "/feedback", "", {}, resp);
            }
        }
        av_packet_free(&pkt);
        segStart += NowSeconds() - segT0;
    }
    printf("[stream] sent %d video frames, %d audio packets in %.1f s; receiver polled timing %d times\n", videoFrames,
           audioPackets, NowSeconds() - t0, ss.ntpPolls.load());

    Plist td = Plist::Dict();
    Plist tarr = Plist::Array();
    Plist t96 = Plist::Dict();
    t96.Set("type", Plist::Int(96));
    tarr.Append(t96);
    td.Set("streams", tarr);
    rtsp.Request("TEARDOWN", "rtsp://" + host + "/1234567890", "application/x-apple-binary-plist", td.Serialize(), resp);
    Plist td2 = Plist::Dict();
    Plist tarr2 = Plist::Array();
    Plist t110 = Plist::Dict();
    t110.Set("type", Plist::Int(110));
    tarr2.Append(t110);
    td2.Set("streams", tarr2);
    code = rtsp.Request("TEARDOWN", "rtsp://" + host + "/1234567890", "application/x-apple-binary-plist", td2.Serialize(), resp);
    printf("[teardown] %d\n", code);
    closesocket(ds);
    rtsp.Request("TEARDOWN", "rtsp://" + host + "/1234567890", "", {}, resp);
    closesocket(ctrlSock);
    closesocket(us);
    printf("PASS stream\n");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Music: audio-only AirPlay (ALAC) with metadata, artwork, progress, pause/resume and a fake DACP
// remote, like the Music app on an iPhone.

static const char* kDacpId = "1A2B3C4D5E6F7788";
static const char* kActiveRemote = "1986535575";

// Minimal DACP control server: records "GET /ctrl-int/1/<command>" requests.
struct FakeDacp {
    SOCKET ls = INVALID_SOCKET;
    uint16_t port = 0;
    std::thread th;
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::vector<std::string> commands;

    bool Start(uint16_t wanted) {
        port = wanted;
        ls = net::TcpListen(port);
        if (ls == INVALID_SOCKET) return false;
        th = std::thread([this] { Run(); });
        return true;
    }
    void Run() {
        while (!stop) {
            if (net::WaitReadable(ls, 100) <= 0) continue;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::string req;
            char b[2048];
            while (req.find("\r\n\r\n") == std::string::npos && net::WaitReadable(c, 1000) > 0) {
                int n = recv(c, b, sizeof(b), 0);
                if (n <= 0) break;
                req.append(b, size_t(n));
            }
            size_t sp = req.find(' ', 4);
            std::string path = req.size() > 4 && sp != std::string::npos ? req.substr(4, sp - 4) : req;
            bool tokenOk = req.find(std::string("Active-Remote: ") + kActiveRemote) != std::string::npos;
            {
                std::lock_guard<std::mutex> lk(mu);
                commands.push_back(path + (tokenOk ? "" : " (wrong Active-Remote)"));
            }
            printf("[dacp] received %s%s\n", path.c_str(), tokenOk ? "" : "  <-- wrong Active-Remote");
            const char* resp = "HTTP/1.1 204 No Content\r\nDAAP-Server: AirGlassTest/1\r\nContent-Length: 0\r\n\r\n";
            send(c, resp, int(strlen(resp)), 0);
            closesocket(c);
        }
    }
    ~FakeDacp() {
        stop = true;
        if (th.joinable()) th.join();
        if (ls != INVALID_SOCKET) closesocket(ls);
    }
};

static void PutDmap(std::vector<uint8_t>& o, const char* tag, const void* data, size_t n) {
    o.insert(o.end(), tag, tag + 4);
    uint8_t len[4];
    WrBE32(len, uint32_t(n));
    o.insert(o.end(), len, len + 4);
    const uint8_t* d = static_cast<const uint8_t*>(data);
    o.insert(o.end(), d, d + n);
}

// Encodes interleaved s16 stereo PCM into compressed ALAC frames of exactly `spf` samples, the way
// iOS frames AirPlay audio (FFmpeg's encoder uses 4096-sample frames, so each 352-sample chunk is
// encoded as its own "partial" frame, which carries its sample count like Apple's).
static bool EncodeAlacFrames(const std::vector<int16_t>& pcm, int sr, int spf, std::vector<std::vector<uint8_t>>& out) {
    const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_ALAC);
    if (!enc) return false;
    size_t frames = pcm.size() / 2;
    AVFrame* f = av_frame_alloc();
    AVPacket* p = av_packet_alloc();
    for (size_t pos = 0; pos < frames; pos += size_t(spf)) {
        int n = int(std::min<size_t>(size_t(spf), frames - pos));
        AVCodecContext* c = avcodec_alloc_context3(enc);
        c->sample_rate = sr;
        c->sample_fmt = AV_SAMPLE_FMT_S16P;
        av_channel_layout_default(&c->ch_layout, 2);
        if (avcodec_open2(c, enc, nullptr) < 0) return false;
        av_frame_unref(f);
        f->nb_samples = n;
        f->format = AV_SAMPLE_FMT_S16P;
        f->sample_rate = sr;
        av_channel_layout_default(&f->ch_layout, 2);
        if (av_frame_get_buffer(f, 0) < 0) return false;
        for (int i = 0; i < n; ++i) {
            reinterpret_cast<int16_t*>(f->data[0])[i] = pcm[(pos + size_t(i)) * 2];
            reinterpret_cast<int16_t*>(f->data[1])[i] = pcm[(pos + size_t(i)) * 2 + 1];
        }
        avcodec_send_frame(c, f);
        avcodec_send_frame(c, nullptr);
        bool got = false;
        while (avcodec_receive_packet(c, p) >= 0) {
            out.emplace_back(p->data, p->data + p->size);
            got = true;
            av_packet_unref(p);
        }
        avcodec_free_context(&c);
        if (!got) return false;
    }
    av_frame_free(&f);
    av_packet_free(&p);
    return true;
}

static int CmdMusic(int argc, char** argv) {
    std::string file, art, host = "127.0.0.1";
    uint16_t port = 7010;
    double seconds = 0, pauseAt = -1, pauseFor = 2.0, linger = 0;
    bool audible = false;
    uint16_t dacpPort = 0;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--host" && i + 1 < argc) host = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = uint16_t(atoi(argv[++i]));
        else if (a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
        else if (a == "--art" && i + 1 < argc) art = argv[++i];
        else if (a == "--dacp-port" && i + 1 < argc) dacpPort = uint16_t(atoi(argv[++i]));
        else if (a == "--pause-at" && i + 1 < argc) pauseAt = atof(argv[++i]);
        else if (a == "--pause-for" && i + 1 < argc) pauseFor = atof(argv[++i]);
        else if (a == "--linger" && i + 1 < argc) linger = atof(argv[++i]);
        else if (a == "--audible") audible = true;
        else file = a;
    }
    if (file.empty()) return Fail("no input file (raw s16le stereo 44.1 kHz)");
    if (host == "127.0.0.1" || host == "localhost") net::SetLoopbackOnly(true);

    const int sr = 44100, bits = 16, spf = 352;
    std::vector<uint8_t> raw = ReadFile(file);
    std::vector<int16_t> pcm(raw.size() / 2);
    std::memcpy(pcm.data(), raw.data(), pcm.size() * 2);
    std::vector<std::vector<uint8_t>> alac;
    if (pcm.empty() || !EncodeAlacFrames(pcm, sr, spf, alac)) return Fail("cannot read/encode the input");
    uint32_t totalSamples = uint32_t(pcm.size() / 2);
    size_t alacBytes = 0;
    for (auto& a : alac) alacBytes += a.size();
    printf("[music] %s: %.1f s, %zu ALAC frames of %d samples, %.0f kbit/s (PCM %.0f kbit/s)\n", file.c_str(),
           double(totalSamples) / sr, alac.size(), spf, alacBytes * 8.0 / (double(totalSamples) / sr) / 1000.0,
           sr * 32.0 / 1000.0);
    std::vector<uint8_t> artwork = art.empty() ? std::vector<uint8_t>() : ReadFile(art);

    FakeDacp dacp;
    if (!dacp.Start(dacpPort)) return Fail("dacp listen");
    printf("[dacp] fake remote-control server on port %u (receiver needs AIRGLASS_DEBUG_DACP_PORT=%u)\n", dacp.port,
           dacp.port);

    SenderSession ss;
    ss.rtsp.always = std::string("DACP-ID: ") + kDacpId + "\r\nActive-Remote: " + kActiveRemote + "\r\n";
    if (!ss.Open(host, port, false, "AirGlass Test iPhone")) return 1;
    Rtsp& rtsp = ss.rtsp;
    std::vector<uint8_t> resp;

    uint16_t ctrlLocal = 0;
    SOCKET ctrlSock = net::UdpBind(ctrlLocal);
    Plist s2 = Plist::Dict();
    {
        Plist st = Plist::Dict();
        st.Set("type", Plist::Int(96));
        st.Set("ct", Plist::Int(2));
        st.Set("spf", Plist::Int(uint64_t(spf)));
        st.Set("sr", Plist::Int(uint64_t(sr)));
        st.Set("audioFormat", Plist::Int(bits == 24 ? (sr == 48000 ? 0x200000 : 0x80000) : (sr == 48000 ? 0x100000 : 0x40000)));
        st.Set("controlPort", Plist::Int(ctrlLocal));
        st.Set("latencyMin", Plist::Int(11025));
        st.Set("latencyMax", Plist::Int(88200));
        st.Set("isMedia", Plist::Bool(true));
        st.Set("audioMode", Plist::String("default"));
        st.Set("redundantAudio", Plist::Int(0));
        Plist arr = Plist::Array();
        arr.Append(st);
        s2.Set("streams", arr);
    }
    int code = rtsp.Request("SETUP", ss.Url(), "application/x-apple-binary-plist", s2.Serialize(), resp);
    Plist r2;
    if (code != 200 || !Plist::Parse(resp.data(), resp.size(), r2)) return Fail("SETUP audio");
    const Plist& rs = r2.Get("streams")->Items()[0];
    uint16_t dataPort = uint16_t(rs.Get("dataPort")->AsUInt());
    uint16_t peerCtrl = uint16_t(rs.Get("controlPort")->AsUInt());
    printf("[setup] audio data port %u, control port %u\n", dataPort, peerCtrl);

    std::map<std::string, std::string> hdrs;
    code = rtsp.Request("RECORD", ss.Url(), "", {}, resp, &hdrs);
    printf("[record] %d Audio-Latency=%s\n", code, hdrs["Audio-Latency"].c_str());

    auto setParam = [&](const std::string& ctype, const std::vector<uint8_t>& body) {
        return rtsp.Request("SET_PARAMETER", ss.Url(), ctype, body, resp);
    };
    auto text = [](const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); };
    setParam("text/parameters", text(audible ? "volume: -15.000000\r\n" : "volume: -144.000000\r\n"));
    {
        std::vector<uint8_t> items;
        std::string title = "Lossless Check", artist = "AirGlass", album = "Test Tones";
        PutDmap(items, "minm", title.data(), title.size());
        PutDmap(items, "asar", artist.data(), artist.size());
        PutDmap(items, "asal", album.data(), album.size());
        uint8_t ms[4];
        WrBE32(ms, uint32_t(double(totalSamples) * 1000.0 / sr));
        PutDmap(items, "astm", ms, 4);
        std::vector<uint8_t> mlit;
        PutDmap(mlit, "mlit", items.data(), items.size());
        printf("[meta] DMAP -> %d\n", setParam("application/x-dmap-tagged", mlit));
    }
    if (!artwork.empty()) {
        bool png = art.size() > 4 && _stricmp(art.c_str() + art.size() - 4, ".png") == 0;
        printf("[meta] artwork %zu bytes -> %d\n", artwork.size(), setParam(png ? "image/png" : "image/jpeg", artwork));
    }
    const uint32_t rtp0 = 0x10203040;
    auto progress = [&](uint32_t current) {
        char p[96];
        snprintf(p, sizeof(p), "progress: %u/%u/%u\r\n", rtp0, current, rtp0 + totalSamples);
        setParam("text/parameters", text(p));
    };
    progress(rtp0);

    SOCKET us = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(dataPort);
    inet_pton(AF_INET, host == "localhost" ? "127.0.0.1" : host.c_str(), &to.sin_addr);
    crypto::AesCbc cbc;
    cbc.Init(ss.aesKey);

    // Retransmissions: answer "resend" requests (0x55) on the control port from a history.
    std::mutex histMu;
    std::map<uint16_t, std::vector<uint8_t>> history;
    std::atomic<bool> stopCtrl{false};
    std::atomic<int> resends{0};
    std::thread ctrlThread([&] {
        uint8_t b[64];
        while (!stopCtrl) {
            if (net::WaitReadable(ctrlSock, 100) <= 0) continue;
            sockaddr_storage from{};
            int flen = sizeof(from);
            int n = recvfrom(ctrlSock, reinterpret_cast<char*>(b), sizeof(b), 0, reinterpret_cast<sockaddr*>(&from), &flen);
            if (n < 8 || (b[1] & 0x7F) != 0x55) continue;
            uint16_t first = RdBE16(b + 4), count = RdBE16(b + 6);
            std::lock_guard<std::mutex> lk(histMu);
            for (uint16_t q = 0; q < count; ++q) {
                auto it = history.find(uint16_t(first + q));
                if (it == history.end()) continue;
                std::vector<uint8_t> pkt = {0x80, 0xD6, 0, 0};
                WrBE16(&pkt[2], uint16_t(first + q));
                pkt.insert(pkt.end(), it->second.begin(), it->second.end());
                sendto(ctrlSock, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0,
                       reinterpret_cast<sockaddr*>(&from), flen);
                ++resends;
            }
        }
    });

    uint16_t seq = 0x7700;
    uint32_t ts = rtp0;
    uint64_t sent = 0, samples = 0;
    double t0 = NowSeconds(), lastFeedback = t0;
    bool paused = false;
    for (size_t fi = 0; fi < alac.size(); ++fi) {
        const std::vector<uint8_t>& frame = alac[fi];
        double audioTime = double(samples) / sr;
        if (seconds > 0 && audioTime >= seconds) break;
        if (!paused && pauseAt >= 0 && audioTime >= pauseAt) {
            paused = true;
            char info[96];
            snprintf(info, sizeof(info), "RTP-Info: seq=%u;rtptime=%u\r\n", seq, ts);
            code = rtsp.Request("FLUSH", ss.Url(), "", {}, resp, nullptr, info);
            printf("[pause] FLUSH -> %d; paused for %.1f s\n", code, pauseFor);
            Sleep(DWORD(pauseFor * 1000));
            t0 += pauseFor;
            progress(ts);
            printf("[pause] resumed\n");
        }
        double due = t0 + audioTime;
        double now = NowSeconds();
        if (due > now) Sleep(DWORD((due - now) * 1000));
        std::vector<uint8_t> rtp(12 + frame.size());
        rtp[0] = 0x80;
        rtp[1] = sent == 0 ? 0xE0 : 0x60;  // marker on the first packet
        WrBE16(&rtp[2], seq);
        WrBE32(&rtp[4], ts);
        WrBE32(&rtp[8], 0x12345678);
        std::memcpy(&rtp[12], frame.data(), frame.size());
        size_t enc = frame.size() / 16 * 16;
        cbc.Encrypt(ss.eiv, &rtp[12], &rtp[12], enc);
        sendto(us, reinterpret_cast<const char*>(rtp.data()), int(rtp.size()), 0, reinterpret_cast<sockaddr*>(&to),
               sizeof(to));
        {
            std::lock_guard<std::mutex> lk(histMu);
            history[seq] = rtp;
            if (history.size() > 1024) history.erase(history.begin());
        }
        int n = int(std::min<uint64_t>(uint64_t(spf), uint64_t(totalSamples) - samples));
        ++seq;
        ts += uint32_t(n);
        samples += uint64_t(n);
        ++sent;
        if (NowSeconds() - lastFeedback > 2.0) {
            lastFeedback = NowSeconds();
            rtsp.Request("POST", "/feedback", "", {}, resp);
        }
    }
    printf("[music] sent %llu ALAC packets (%llu samples, %.2f s of audio), %d resends\n", (unsigned long long)sent,
           (unsigned long long)samples, double(samples) / sr, resends.load());
    for (double t = 0; t < linger; t += 1.0) {
        Sleep(1000);
        rtsp.Request("POST", "/feedback", "", {}, resp);
    }
    stopCtrl = true;
    ctrlThread.join();

    Plist td = Plist::Dict();
    Plist tarr = Plist::Array();
    Plist t96 = Plist::Dict();
    t96.Set("type", Plist::Int(96));
    tarr.Append(t96);
    td.Set("streams", tarr);
    rtsp.Request("TEARDOWN", ss.Url(), "application/x-apple-binary-plist", td.Serialize(), resp);
    code = rtsp.Request("TEARDOWN", ss.Url(), "", {}, resp);
    printf("[teardown] %d\n", code);
    closesocket(us);
    closesocket(ctrlSock);
    {
        std::lock_guard<std::mutex> lk(dacp.mu);
        printf("[dacp] %zu remote-control commands received\n", dacp.commands.size());
    }
    printf("PASS music\n");
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Resampler quality: new windowed-sinc interpolator vs. the old linear interpolation.

static int CmdResampler() {
    const double inRate = 44100.0;
    const int T = SincResampler::kTaps;
    printf("Error of a resampled 0.5 FS sine vs. the ideal signal (higher dB = cleaner), 44.1 kHz input\n");
    printf("%-22s %8s | %12s %10s | %12s %10s\n", "conversion", "tone", "sinc SNR", "sinc gain", "linear SNR",
           "lin gain");
    bool ok = true;
    struct Case {
        double outRate, drift;
        const char* name;
    };
    for (Case c : {Case{48000.0, 1.0, "44.1k -> 48k"}, Case{44100.0, 1.00007, "44.1k drift +70 ppm"},
                   Case{96000.0, 1.0, "44.1k -> 96k"}}) {
        SincResampler rs;
        rs.Init(inRate, c.outRate);
        for (double f : {1000.0, 5000.0, 10000.0, 15000.0, 18000.0, 19500.0}) {
            const int N = 44100 * 2;
            std::vector<float> x(size_t(N) * 2);
            for (int n = 0; n < N; ++n) x[2 * size_t(n)] = x[2 * size_t(n) + 1] = float(0.5 * std::sin(2.0 * 3.14159265358979 * f * n / inRate));
            double step = inRate / c.outRate * c.drift;
            size_t base = 0;
            double frac = 0.37;
            double sig = 0, errS = 0, errL = 0, xyS = 0, xyL = 0;
            while (base + size_t(T) + 2 < size_t(N)) {
                float l, r;
                rs.Process(&x[base * 2], frac, l, r);
                double pos = double(base) + T / 2 - 1 + frac;
                double ideal = 0.5 * std::sin(2.0 * 3.14159265358979 * f * pos / inRate);
                size_t i0 = size_t(pos);
                double t = pos - double(i0);
                double lin = x[i0 * 2] + t * (x[(i0 + 1) * 2] - x[i0 * 2]);
                sig += ideal * ideal;
                errS += (l - ideal) * (l - ideal);
                errL += (lin - ideal) * (lin - ideal);
                xyS += l * ideal;
                xyL += lin * ideal;
                frac += step;
                size_t adv = size_t(frac);
                frac -= double(adv);
                base += adv;
            }
            double snrS = 10.0 * std::log10(sig / std::max(errS, 1e-30));
            double snrL = 10.0 * std::log10(sig / std::max(errL, 1e-30));
            double gS = 20.0 * std::log10(xyS / sig), gL = 20.0 * std::log10(xyL / sig);
            printf("%-22s %6.1f k | %9.1f dB %7.3f dB | %9.1f dB %7.3f dB\n", c.name, f / 1000.0, snrS, gS, snrL, gL);
            if (f <= 18000.0 && snrS < 90.0) ok = false;
        }
    }
    if (!ok) return Fail("sinc resampler below 90 dB in the passband");
    printf("PASS resampler\n");
    return 0;
}

// One-shot mDNS SRV lookup (the code AirGlass uses to find a sender's DACP remote).
static int CmdResolve(std::string instance, const std::string& type) {
    uint16_t port = 0;
    in_addr addr{};
    double t = NowSeconds();
    bool ok = MdnsResponder::ResolveService(instance, type, 3.0, port, &addr);
    size_t q = instance.find('\'');
    if (!ok && q != std::string::npos) {  // Apple device names use a curly apostrophe
        instance.replace(q, 1, "\xE2\x80\x99");
        t = NowSeconds();
        ok = MdnsResponder::ResolveService(instance, type, 3.0, port, &addr);
    }
    char ip[32] = "?";
    inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    printf("%s.%s.local -> %s port %u (A %s) in %.2f s\n", instance.c_str(), type.c_str(), ok ? "found" : "not found",
           port, ip, NowSeconds() - t);
    return ok ? 0 : 1;
}

// Lemon Squeezy license round trip (the same calls the Pro dialog and start-up check make).
static int CmdLicense(const std::string& action, const std::string& key, const std::string& arg) {
    license::Result r = action == "validate" ? license::Validate(key, arg) : license::Activate(key, arg);
    printf("%s: reached=%d valid=%d instance=%s error=%s\n", action.c_str(), r.ok, r.valid, r.instanceId.c_str(),
           r.error.c_str());
    return r.ok ? 0 : 1;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    net::Startup();
    if (!crypto::SelfTest()) return Fail("crypto self-test");
    if (argc < 2) {
        printf("usage: airglass_test bplist <dir> | mdns | stream <a.mp4> [b.mp4] [--host H] [--port P] [--seconds S]\n");
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "bplist" && argc >= 3) return CmdBplist(argv[2]);
    if (cmd == "mdns") return CmdMdns();
    if (cmd == "stream") return CmdStream(argc, argv);
    if (cmd == "music") return CmdMusic(argc, argv);
    if (cmd == "resampler") return CmdResampler();
    if (cmd == "resolve" && argc >= 4) return CmdResolve(argv[2], argv[3]);
    if (cmd == "license" && argc >= 5) return CmdLicense(argv[2], argv[3], argv[4]);  // activate|validate KEY NAME|ID
    return Fail("unknown command");
}
