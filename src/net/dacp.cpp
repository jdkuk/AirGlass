#include "net/dacp.h"

#include "net/mdns.h"
#include "net/netutil.h"

DacpRemote::~DacpRemote() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
}

void DacpRemote::Configure(const std::string& dacpId, const std::string& activeRemote, const sockaddr_storage& peer) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        token_ = activeRemote;
        peer_ = peer;
        if (dacpId == id_) return;
        id_ = dacpId;
    }
    port_ = 0;
    uint64_t gen = ++gen_;
    if (thread_.joinable()) thread_.join();  // the previous lookup notices the new generation
    LOGI("dacp: sender remote id %s; looking for its control service", dacpId.c_str());
    thread_ = std::thread(&DacpRemote::Resolve, this, gen, dacpId);
}

void DacpRemote::Resolve(uint64_t gen, std::string id) {
    char env[16];
    if (GetEnvironmentVariableA("AIRGLASS_DEBUG_DACP_PORT", env, sizeof(env))) {  // loopback tests
        port_ = uint16_t(atoi(env));
        LOGI("dacp: using test remote on port %u", unsigned(port_.load()));
        if (onAvailable) onAvailable();
        return;
    }
    // The sender may register the service a moment after it starts streaming.
    const double kDelays[] = {0.0, 2.0, 6.0, 15.0, 30.0};
    for (double delay : kDelays) {
        double until = NowSeconds() + delay;
        while (NowSeconds() < until) {
            if (stop_ || gen_ != gen) return;
            Sleep(100);
        }
        if (stop_ || gen_ != gen) return;
        uint16_t port = 0;
        if (MdnsResponder::ResolveService("iTunes_Ctrl_" + id, "_dacp._tcp", 1.5, port, nullptr) && port) {
            if (stop_ || gen_ != gen) return;
            port_ = port;
            LOGI("dacp: remote control available (port %u)", port);
            if (onAvailable) onAvailable();
            return;
        }
    }
    LOGI("dacp: the sender did not advertise a remote-control service; controls unavailable");
}

std::string DacpRemote::Send(const std::string& command) {
    uint16_t port = port_;
    if (!port) return "no-remote";
    std::string token;
    sockaddr_storage peer;
    {
        std::lock_guard<std::mutex> lk(mu_);
        token = token_;
        peer = peer_;
    }
    sockaddr_storage to = net::WithPort(peer, port);
    SOCKET s = socket(to.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return "failed";
    if (to.ss_family == AF_INET6) {
        DWORD off = 0;  // IPv4-mapped peers need a dual-stack socket
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    }
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, reinterpret_cast<const sockaddr*>(&to), net::AddrLen(to));
    fd_set wf;
    FD_ZERO(&wf);
    FD_SET(s, &wf);
    timeval tv{1, 500000};
    if (select(0, nullptr, &wf, nullptr, &tv) <= 0) {
        closesocket(s);
        LOGW("dacp: cannot reach %s", net::AddrToString(to).c_str());
        return "failed";
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    std::string req = "GET /ctrl-int/1/" + command + " HTTP/1.1\r\nHost: starlight.local.\r\nActive-Remote: " + token +
                      "\r\nConnection: close\r\n\r\n";
    std::string status;
    if (net::SendAll(s, req.data(), req.size()) && net::WaitReadable(s, 2000) > 0) {
        char buf[512];
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) status.assign(buf, size_t(n));
    }
    closesocket(s);
    size_t sp = status.find(' ');
    int code = sp == std::string::npos ? 0 : atoi(status.c_str() + sp + 1);
    LOGI("dacp: %s -> %d", command.c_str(), code);
    return code >= 200 && code < 300 ? "" : "failed";
}
