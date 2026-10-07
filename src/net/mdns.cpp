#include "net/mdns.h"

#include <mswsock.h>

namespace {

constexpr uint16_t kMdnsPort = 5353;
constexpr uint16_t kTypeA = 1, kTypePTR = 12, kTypeTXT = 16, kTypeSRV = 33, kTypeANY = 255;
constexpr uint32_t kTtlShared = 4500, kTtlHost = 120;

using Labels = std::vector<std::string>;

Labels Split(const std::string& s) {
    Labels l;
    size_t start = 0;
    while (start <= s.size()) {
        size_t dot = s.find('.', start);
        if (dot == std::string::npos) dot = s.size();
        if (dot > start) l.push_back(s.substr(start, dot - start));
        start = dot + 1;
    }
    return l;
}

bool LabelEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        unsigned char x = a[i], y = b[i];
        if (x < 128 && y < 128) {
            if (tolower(x) != tolower(y)) return false;
        } else if (x != y) {
            return false;
        }
    }
    return true;
}

bool NameEq(const Labels& a, const Labels& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!LabelEq(a[i], b[i])) return false;
    return true;
}

void PutU16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(uint8_t(v >> 8));
    o.push_back(uint8_t(v));
}

void PutU32(std::vector<uint8_t>& o, uint32_t v) {
    PutU16(o, uint16_t(v >> 16));
    PutU16(o, uint16_t(v));
}

void PutName(std::vector<uint8_t>& o, const Labels& l) {
    for (auto& s : l) {
        size_t len = std::min<size_t>(s.size(), 63);
        o.push_back(uint8_t(len));
        o.insert(o.end(), s.begin(), s.begin() + len);
    }
    o.push_back(0);
}

bool ReadName(const uint8_t* p, size_t n, size_t& off, Labels& out) {
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
            if (ptr >= n) return false;
            if (!jumped) off = pos;
            jumped = true;
            pos = ptr;
            continue;
        }
        if ((len & 0xC0) || pos + len > n) return false;
        out.emplace_back(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
    }
    return false;
}

struct Record {
    Labels name;
    uint16_t type;
    bool unique;  // cache-flush bit
    uint32_t ttl;
    std::vector<uint8_t> rdata;
};

void PutRecord(std::vector<uint8_t>& o, const Record& r, bool legacy, uint32_t ttlOverride) {
    PutName(o, r.name);
    PutU16(o, r.type);
    PutU16(o, uint16_t((r.unique && !legacy) ? 0x8001 : 0x0001));
    uint32_t ttl = r.ttl;
    if (ttlOverride != UINT32_MAX) ttl = ttlOverride;
    if (legacy) ttl = std::min<uint32_t>(ttl, 10);
    PutU32(o, ttl);
    PutU16(o, uint16_t(r.rdata.size()));
    o.insert(o.end(), r.rdata.begin(), r.rdata.end());
}

}  // namespace

bool MdnsResponder::Want::any() const {
    if (servicesEnum || hostA) return true;
    for (size_t i = 0; i < ptr.size(); ++i)
        if (ptr[i] || srv[i] || txt[i]) return true;
    return false;
}

std::vector<uint8_t> MdnsResponder::EncodeTxt(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::vector<uint8_t> out;
    for (auto& e : kv) {
        std::string item = e.first + "=" + e.second;
        if (item.size() > 255) item.resize(255);
        out.push_back(uint8_t(item.size()));
        out.insert(out.end(), item.begin(), item.end());
    }
    if (out.empty()) out.push_back(0);
    return out;
}

std::vector<std::string> MdnsResponder::BrowseReceiverNames(double seconds) {
    std::vector<std::string> names;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return names;
    sockaddr_in any{};
    any.sin_family = AF_INET;
    bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    std::vector<uint8_t> q = {0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0, 0, 0, 0, 0, 0};
    PutName(q, Labels{"_airplay", "_tcp", "local"});
    PutU16(q, kTypePTR);
    PutU16(q, 1);
    PutName(q, Labels{"_raop", "_tcp", "local"});
    PutU16(q, kTypePTR);
    PutU16(q, 1);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kMdnsPort);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    // Ask on every interface.
    for (auto& i : net::ListIpv4Interfaces()) {
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&i.addr), sizeof(i.addr));
        sendto(s, reinterpret_cast<const char*>(q.data()), int(q.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    double end = NowSeconds() + seconds;
    std::vector<uint8_t> buf(9000);
    while (NowSeconds() < end) {
        if (net::WaitReadable(s, 100) <= 0) continue;
        int n = recv(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
        if (n < 12 || !(RdBE16(buf.data() + 2) & 0x8000)) continue;
        size_t off = 12;
        Labels name;
        uint16_t qd = RdBE16(buf.data() + 4);
        int records = RdBE16(buf.data() + 6) + RdBE16(buf.data() + 8) + RdBE16(buf.data() + 10);
        for (int i = 0; i < qd; ++i) {
            if (!ReadName(buf.data(), size_t(n), off, name)) break;
            off += 4;
        }
        for (int i = 0; i < records && off + 10 <= size_t(n); ++i) {
            if (!ReadName(buf.data(), size_t(n), off, name) || off + 10 > size_t(n)) break;
            uint16_t type = RdBE16(buf.data() + off);
            uint16_t rdlen = RdBE16(buf.data() + off + 8);
            size_t rd = off + 10;
            off = rd + rdlen;
            if (type != kTypePTR || name.size() != 3 || name[1] != "_tcp") continue;
            Labels target;
            size_t r = rd;
            if (!ReadName(buf.data(), size_t(n), r, target) || target.empty()) continue;
            std::string inst = target[0];
            if (name[0] == "_raop") {
                size_t at = inst.find('@');
                if (at != std::string::npos) inst = inst.substr(at + 1);
            }
            if (std::find(names.begin(), names.end(), inst) == names.end()) names.push_back(inst);
        }
    }
    closesocket(s);
    return names;
}

bool MdnsResponder::ResolveService(const std::string& instance, const std::string& type, double seconds,
                                   uint16_t& port, in_addr* addr) {
    Labels want{instance};
    for (auto& l : Split(type)) want.push_back(l);
    want.push_back("local");
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in any{};
    any.sin_family = AF_INET;
    bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
    // Legacy unicast query (from a non-5353 port): responders answer straight back to us.
    std::vector<uint8_t> q = {0x12, 0x34, 0x00, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
    PutName(q, want);
    PutU16(q, kTypeSRV);
    PutU16(q, 1);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kMdnsPort);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    auto ask = [&] {
        for (auto& i : net::ListIpv4Interfaces()) {
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&i.addr), sizeof(i.addr));
            sendto(s, reinterpret_cast<const char*>(q.data()), int(q.size()), 0, reinterpret_cast<sockaddr*>(&to),
                   sizeof(to));
        }
    };
    ask();
    bool found = false;
    Labels target;
    double start = NowSeconds(), end = start + seconds;
    bool askedAgain = false;
    std::vector<uint8_t> buf(9000);
    while (NowSeconds() < end && !(found && (!addr || addr->s_addr))) {
        if (!askedAgain && NowSeconds() - start > 0.5) {
            ask();
            askedAgain = true;
        }
        if (net::WaitReadable(s, 100) <= 0) continue;
        int n = recv(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
        if (n < 12 || !(RdBE16(buf.data() + 2) & 0x8000)) continue;
        size_t off = 12;
        Labels name;
        uint16_t qd = RdBE16(buf.data() + 4);
        int records = RdBE16(buf.data() + 6) + RdBE16(buf.data() + 8) + RdBE16(buf.data() + 10);
        for (int i = 0; i < qd; ++i) {
            if (!ReadName(buf.data(), size_t(n), off, name)) break;
            off += 4;
        }
        for (int i = 0; i < records && off + 10 <= size_t(n); ++i) {
            if (!ReadName(buf.data(), size_t(n), off, name) || off + 10 > size_t(n)) break;
            uint16_t rtype = RdBE16(buf.data() + off);
            uint16_t rdlen = RdBE16(buf.data() + off + 8);
            size_t rd = off + 10;
            off = rd + rdlen;
            if (off > size_t(n)) break;
            if (rtype == kTypeSRV && rdlen >= 7 && NameEq(name, want)) {
                port = RdBE16(buf.data() + rd + 4);
                size_t t = rd + 6;
                ReadName(buf.data(), size_t(n), t, target);
                found = true;
            } else if (rtype == kTypeA && rdlen == 4 && addr && found && NameEq(name, target)) {
                std::memcpy(&addr->s_addr, buf.data() + rd, 4);
            }
        }
    }
    closesocket(s);
    return found;
}

bool MdnsResponder::Start(const std::string& hostLabel, std::vector<Service> services) {
    Stop();
    host_ = hostLabel;
    services_ = std::move(services);
    txtRdata_.clear();
    for (auto& s : services_) txtRdata_.push_back(EncodeTxt(s.txt));

    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) {
        LOGE("mdns: socket failed %d", WSAGetLastError());
        return false;
    }
    BOOL on = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof(on));
    sockaddr_in any{};
    any.sin_family = AF_INET;
    any.sin_port = htons(kMdnsPort);
    any.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_, reinterpret_cast<sockaddr*>(&any), sizeof(any)) != 0) {
        LOGE("mdns: bind 5353 failed %d", WSAGetLastError());
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }
    DWORD ttl = 255;
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl), sizeof(ttl));
    DWORD loop = 1;
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop), sizeof(loop));
    setsockopt(sock_, IPPROTO_IP, IP_PKTINFO, reinterpret_cast<const char*>(&on), sizeof(on));

    GUID guid = WSAID_WSARECVMSG;
    DWORD bytes = 0;
    LPFN_WSARECVMSG fn = nullptr;
    if (WSAIoctl(sock_, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &fn, sizeof(fn), &bytes, nullptr,
                 nullptr) == 0) {
        recvMsg_ = reinterpret_cast<void*>(fn);
    }

    RefreshInterfaces(false);
    stop_ = false;
    thread_ = std::thread(&MdnsResponder::Run, this);
    for (auto& s : services_) LOGI("mdns: advertising \"%s.%s.local\" port %u", s.instance.c_str(), s.type.c_str(), s.port);
    return true;
}

void MdnsResponder::Stop() {
    if (!thread_.joinable()) {
        if (sock_ != INVALID_SOCKET) {
            closesocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        return;
    }
    stop_ = true;
    thread_.join();
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& i : ifaces_) SendAnnouncement(i, 0);  // goodbye
    }
    closesocket(sock_);
    sock_ = INVALID_SOCKET;
}

void MdnsResponder::RefreshInterfaces(bool announceNew) {
    auto now = net::ListIpv4Interfaces();
    std::vector<net::Ipv4Iface> added;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& n : now) {
            bool known = false;
            for (auto& o : ifaces_)
                if (o.index == n.index && o.addr.s_addr == n.addr.s_addr) known = true;
            if (known) continue;
            ip_mreq mreq{};
            inet_pton(AF_INET, "224.0.0.251", &mreq.imr_multiaddr);
            mreq.imr_interface = n.addr;
            int rc = setsockopt(sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<const char*>(&mreq),
                                sizeof(mreq));
            char ip[32];
            inet_ntop(AF_INET, &n.addr, ip, sizeof(ip));
            LOGI("mdns: interface %u %s (%s)%s%s", n.index, ip, n.name.c_str(), n.hasGateway ? " [gateway]" : "",
                 rc == 0 ? "" : " join failed");
            added.push_back(n);
        }
        ifaces_ = now;
    }
    if (announceNew) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& a : added) SendAnnouncement(a, UINT32_MAX);
    }
}

const net::Ipv4Iface* MdnsResponder::IfaceFor(uint32_t ifIndex, in_addr src) const {
    // Prefer the interface whose subnet contains the sender, then the receiving interface.
    for (auto& i : ifaces_)
        if ((i.addr.s_addr & i.mask.s_addr) == (src.s_addr & i.mask.s_addr) && i.mask.s_addr) return &i;
    for (auto& i : ifaces_)
        if (i.index == ifIndex) return &i;
    for (auto& i : ifaces_)
        if (i.hasGateway && !i.linkLocal) return &i;
    return ifaces_.empty() ? nullptr : &ifaces_[0];
}

void MdnsResponder::SendTo(const std::vector<uint8_t>& pkt, const sockaddr_in& to, const net::Ipv4Iface* iface) {
    if (iface) {
        setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&iface->addr),
                   sizeof(iface->addr));
    }
    sendto(sock_, reinterpret_cast<const char*>(pkt.data()), int(pkt.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

void MdnsResponder::SendAnnouncement(const net::Ipv4Iface& iface, uint32_t ttl) {
    Want w;
    size_t n = services_.size();
    w.ptr.assign(n, true);
    w.srv.assign(n, true);
    w.txt.assign(n, true);
    w.hostA = true;
    std::vector<uint8_t> pkt;
    if (!BuildResponse(pkt, w, iface.addr, false, 0, nullptr, 0, 0, ttl)) return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kMdnsPort);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    SendTo(pkt, to, &iface);
}

bool MdnsResponder::BuildResponse(std::vector<uint8_t>& out, const Want& want, in_addr hostAddr, bool legacy,
                                  uint16_t id, const uint8_t* questions, size_t questionsLen, uint16_t qdcount,
                                  uint32_t ttlOverride) {
    Labels hostName{host_, "local"};
    Labels enumName{"_services", "_dns-sd", "_udp", "local"};
    std::vector<Record> answers, additionals;

    auto typeLabels = [&](size_t i) {
        Labels l = Split(services_[i].type);
        l.push_back("local");
        return l;
    };
    auto instLabels = [&](size_t i) {
        Labels l{services_[i].instance};
        Labels t = typeLabels(i);
        l.insert(l.end(), t.begin(), t.end());
        return l;
    };
    auto ptrRec = [&](size_t i) {
        Record r{typeLabels(i), kTypePTR, false, kTtlShared, {}};
        PutName(r.rdata, instLabels(i));
        return r;
    };
    auto srvRec = [&](size_t i) {
        Record r{instLabels(i), kTypeSRV, true, kTtlHost, {}};
        PutU16(r.rdata, 0);
        PutU16(r.rdata, 0);
        PutU16(r.rdata, services_[i].port);
        PutName(r.rdata, hostName);
        return r;
    };
    auto txtRec = [&](size_t i) { return Record{instLabels(i), kTypeTXT, true, kTtlShared, txtRdata_[i]}; };
    auto aRec = [&]() {
        Record r{hostName, kTypeA, true, kTtlHost, {}};
        const uint8_t* b = reinterpret_cast<const uint8_t*>(&hostAddr);
        r.rdata.assign(b, b + 4);
        return r;
    };

    bool needA = false;
    if (want.servicesEnum) {
        for (size_t i = 0; i < services_.size(); ++i) {
            Record r{enumName, kTypePTR, false, kTtlShared, {}};
            PutName(r.rdata, typeLabels(i));
            answers.push_back(r);
        }
    }
    for (size_t i = 0; i < services_.size(); ++i) {
        if (want.ptr[i]) answers.push_back(ptrRec(i));
        if (want.srv[i]) {
            answers.push_back(srvRec(i));
            needA = true;
        }
        if (want.txt[i]) answers.push_back(txtRec(i));
    }
    if (want.hostA) answers.push_back(aRec());
    for (size_t i = 0; i < services_.size(); ++i) {
        if (want.ptr[i]) {
            if (!want.srv[i]) additionals.push_back(srvRec(i));
            if (!want.txt[i]) additionals.push_back(txtRec(i));
            needA = true;
        }
    }
    if (needA && !want.hostA) additionals.push_back(aRec());
    if (answers.empty()) return false;

    out.clear();
    PutU16(out, legacy ? id : 0);
    PutU16(out, 0x8400);  // response, authoritative
    PutU16(out, legacy ? qdcount : 0);
    PutU16(out, uint16_t(answers.size()));
    PutU16(out, 0);
    PutU16(out, uint16_t(additionals.size()));
    if (legacy && questions && questionsLen) out.insert(out.end(), questions, questions + questionsLen);
    for (auto& r : answers) PutRecord(out, r, legacy, ttlOverride);
    for (auto& r : additionals) PutRecord(out, r, legacy, ttlOverride);
    return true;
}

void MdnsResponder::HandlePacket(const uint8_t* p, int n, const sockaddr_in& from, uint32_t ifIndex, in_addr dst) {
    (void)dst;
    if (n < 12) return;
    uint16_t id = RdBE16(p);
    uint16_t flags = RdBE16(p + 2);
    if (flags & 0x8000) return;           // a response from someone else
    if ((flags >> 11) & 0xF) return;      // non-standard opcode
    uint16_t qd = RdBE16(p + 4);
    if (qd == 0 || qd > 64) return;

    size_t count = services_.size();
    Want want;
    want.ptr.assign(count, false);
    want.srv.assign(count, false);
    want.txt.assign(count, false);

    Labels hostName{host_, "local"};
    Labels enumName{"_services", "_dns-sd", "_udp", "local"};
    size_t off = 12;
    bool unicastWanted = false;
    for (uint16_t q = 0; q < qd; ++q) {
        Labels name;
        if (!ReadName(p, size_t(n), off, name) || off + 4 > size_t(n)) return;
        uint16_t qtype = RdBE16(p + off);
        uint16_t qclass = RdBE16(p + off + 2);
        off += 4;
        if (qclass & 0x8000) unicastWanted = true;
        if ((qclass & 0x7FFF) != 1 && (qclass & 0x7FFF) != 255) continue;
        bool isAny = qtype == kTypeANY;
        if (NameEq(name, enumName) && (qtype == kTypePTR || isAny)) want.servicesEnum = true;
        for (size_t i = 0; i < count; ++i) {
            Labels t = Split(services_[i].type);
            t.push_back("local");
            if (NameEq(name, t) && (qtype == kTypePTR || isAny)) want.ptr[i] = true;
            Labels inst{services_[i].instance};
            inst.insert(inst.end(), t.begin(), t.end());
            if (NameEq(name, inst)) {
                if (qtype == kTypeSRV || isAny) want.srv[i] = true;
                if (qtype == kTypeTXT || isAny) want.txt[i] = true;
            }
        }
        if (NameEq(name, hostName) && (qtype == kTypeA || isAny)) want.hostA = true;
    }
    if (!want.any()) return;

    std::lock_guard<std::mutex> lk(mu_);
    const net::Ipv4Iface* iface = IfaceFor(ifIndex, from.sin_addr);
    if (!iface) return;
    bool legacy = ntohs(from.sin_port) != kMdnsPort;
    std::vector<uint8_t> pkt;
    if (!BuildResponse(pkt, want, iface->addr, legacy, id, p + 12, off - 12, qd, UINT32_MAX)) return;
    if (pkt.size() > 9000) return;

    char ip[32];
    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
    if (std::find(answered_.begin(), answered_.end(), from.sin_addr.s_addr) == answered_.end()) {
        answered_.push_back(from.sin_addr.s_addr);
        LOGI("mdns: first answer to %s (a device is looking for AirPlay receivers)", ip);
    }
    LOGD("mdns: answering query from %s:%u (%s)", ip, ntohs(from.sin_port), legacy ? "legacy unicast" : "multicast");

    if (legacy) {
        SendTo(pkt, from, nullptr);
        return;
    }
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kMdnsPort);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    SendTo(pkt, to, iface);
    if (unicastWanted) SendTo(pkt, from, nullptr);
}

void MdnsResponder::Run() {
    int announced = 0;
    double nextAnnounce = NowSeconds();
    double nextRefresh = NowSeconds() + 15.0;
    std::vector<uint8_t> buf(9000);
    while (!stop_) {
        double now = NowSeconds();
        if (announced < 3 && now >= nextAnnounce) {
            std::lock_guard<std::mutex> lk(mu_);
            for (auto& i : ifaces_) SendAnnouncement(i, UINT32_MAX);
            ++announced;
            nextAnnounce = now + double(announced);  // 1 s, then 2 s
        }
        if (now >= nextRefresh) {
            RefreshInterfaces(true);
            nextRefresh = now + 15.0;
        }
        if (net::WaitReadable(sock_, 250) <= 0) continue;

        sockaddr_in from{};
        uint32_t ifIndex = 0;
        in_addr dst{};
        int got = 0;
        if (recvMsg_) {
            WSABUF wb{ULONG(buf.size()), reinterpret_cast<char*>(buf.data())};
            char ctrl[256];
            WSAMSG msg{};
            msg.name = reinterpret_cast<sockaddr*>(&from);
            msg.namelen = sizeof(from);
            msg.lpBuffers = &wb;
            msg.dwBufferCount = 1;
            msg.Control.buf = ctrl;
            msg.Control.len = sizeof(ctrl);
            DWORD bytes = 0;
            auto fn = reinterpret_cast<LPFN_WSARECVMSG>(recvMsg_);
            if (fn(sock_, &msg, &bytes, nullptr, nullptr) != 0) continue;
            got = int(bytes);
            for (WSACMSGHDR* c = WSA_CMSG_FIRSTHDR(&msg); c; c = WSA_CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_PKTINFO) {
                    auto* pi = reinterpret_cast<IN_PKTINFO*>(WSA_CMSG_DATA(c));
                    ifIndex = pi->ipi_ifindex;
                    dst = pi->ipi_addr;
                }
            }
        } else {
            int flen = sizeof(from);
            got = recvfrom(sock_, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0,
                           reinterpret_cast<sockaddr*>(&from), &flen);
        }
        if (got > 0) HandlePacket(buf.data(), got, from, ifIndex, dst);
    }
}
