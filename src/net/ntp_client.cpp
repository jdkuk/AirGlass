#include "net/ntp_client.h"

uint64_t NtpClient::NtpNow() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t t100 = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;  // since 1601
    const uint64_t k1601to1900 = 9435484800ull;                                 // seconds
    uint64_t secs = t100 / 10000000ull - k1601to1900;
    uint64_t frac = ((t100 % 10000000ull) << 32) / 10000000ull;
    return (secs << 32) | frac;
}

bool NtpClient::Start(const sockaddr_storage& peer, uint16_t remotePort, std::function<void()> onDead) {
    Stop();
    uint16_t port = 0;
    sock_ = net::UdpBind(port);
    if (sock_ == INVALID_SOCKET) return false;
    localPort_ = port;
    remote_ = net::ForSocket(sock_, net::WithPort(peer, remotePort));
    onDead_ = std::move(onDead);
    stop_ = false;
    thread_ = std::thread(&NtpClient::Run, this);
    LOGI("ntp: polling %s:%u from local port %u", net::AddrToString(peer).c_str(), remotePort, localPort_);
    return true;
}

void NtpClient::Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    net::CloseSocket(sock_);
}

void NtpClient::Run() {
    int misses = 0;
    bool reported = false;
    double nextSend = 0;
    bool awaiting = false;
    double sentAt = 0;
    uint8_t buf[128];
    while (!stop_) {
        double now = NowSeconds();
        if (!awaiting && now >= nextSend && net::AddrPort(remote_)) {
            uint8_t req[32] = {0x80, 0xd2, 0x00, 0x07};
            WrBE64(req + 24, NtpNow());
            sendto(sock_, reinterpret_cast<const char*>(req), sizeof(req), 0,
                   reinterpret_cast<const sockaddr*>(&remote_), net::AddrLen(remote_));
            awaiting = true;
            sentAt = now;
        }
        if (awaiting && now - sentAt > 1.0) {
            awaiting = false;
            nextSend = now + 2.0;
            if (++misses >= 8 && !reported) {
                reported = true;
                LOGW("ntp: sender stopped answering timing requests");
                if (onDead_) onDead_();
            }
        }
        if (net::WaitReadable(sock_, 100) <= 0) continue;
        sockaddr_storage from{};
        int flen = sizeof(from);
        int n = recvfrom(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                         &flen);
        if (n < 32) continue;
        uint8_t type = buf[1] & 0x7F;
        if (type == 0x53) {  // timing response
            uint64_t t3 = NtpNow();
            uint64_t t0 = RdBE64(buf + 8), t1 = RdBE64(buf + 16), t2 = RdBE64(buf + 24);
            double rtt = (double(int64_t(t3 - t0)) - double(int64_t(t2 - t1))) / 4294967296.0;
            if (misses > 0 || reported) LOGI("ntp: sender answering again (rtt %.1f ms)", rtt * 1000);
            misses = 0;
            awaiting = false;
            nextSend = NowSeconds() + 3.0;
        } else if (type == 0x52) {  // the sender polling us: answer
            uint8_t resp[32] = {0x80, 0xd3, 0x00, 0x07};
            uint64_t now64 = NtpNow();
            std::memcpy(resp + 8, buf + 24, 8);
            WrBE64(resp + 16, now64);
            WrBE64(resp + 24, now64);
            sendto(sock_, reinterpret_cast<const char*>(resp), sizeof(resp), 0,
                   reinterpret_cast<const sockaddr*>(&from), flen);
        }
    }
}
