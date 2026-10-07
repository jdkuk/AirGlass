#include "net/netutil.h"

#include <iphlpapi.h>
#include <mswsock.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace net {

static bool g_loopbackOnly = false;

bool Startup() {
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

void Cleanup() { WSACleanup(); }

void SetLoopbackOnly(bool on) { g_loopbackOnly = on; }

static SOCKET MakeDualStack(int type, int proto) {
    if (g_loopbackOnly) return INVALID_SOCKET;  // forces the IPv4 path below
    SOCKET s = socket(AF_INET6, type, proto);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    DWORD off = 0;
    setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    return s;
}

static bool BindAny(SOCKET s, int family, uint16_t port) {
    if (family == AF_INET6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(port);
        return bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(g_loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
    a.sin_port = htons(port);
    return bind(s, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
}

static uint16_t LocalPort(SOCKET s) {
    sockaddr_storage a{};
    int len = sizeof(a);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&a), &len) != 0) return 0;
    return AddrPort(a);
}

SOCKET TcpListen(uint16_t& port, bool exclusive) {
    int family = AF_INET6;
    SOCKET s = MakeDualStack(SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        family = AF_INET;
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
    }
    if (exclusive) {
        BOOL on = TRUE;
        setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&on), sizeof(on));
    }
    if (!BindAny(s, family, port) || listen(s, 8) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    port = LocalPort(s);
    return s;
}

SOCKET UdpBind(uint16_t& port) {
    int family = AF_INET6;
    SOCKET s = MakeDualStack(SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        family = AF_INET;
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) return INVALID_SOCKET;
    }
    // Don't let ICMP port-unreachable replies surface as recv errors on this socket.
    BOOL off = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &bytes, nullptr, nullptr);
    if (!BindAny(s, family, port)) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    port = LocalPort(s);
    return s;
}

void CloseSocket(SOCKET& s) {
    if (s != INVALID_SOCKET) {
        shutdown(s, SD_BOTH);
        closesocket(s);
        s = INVALID_SOCKET;
    }
}

bool SendAll(SOCKET s, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n) {
        int r = send(s, p, int(std::min<size_t>(n, 1 << 20)), 0);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

int WaitReadable(SOCKET s, int timeoutMs) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(s, &rf);
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    int r = select(0, &rf, nullptr, nullptr, &tv);
    if (r == SOCKET_ERROR) return -1;
    return r > 0 ? 1 : 0;
}

bool RecvExact(SOCKET s, void* buf, size_t n, const std::atomic<bool>* stop) {
    char* p = static_cast<char*>(buf);
    while (n) {
        if (stop && stop->load()) return false;
        int w = WaitReadable(s, 200);
        if (w < 0) return false;
        if (w == 0) continue;
        int r = recv(s, p, int(std::min<size_t>(n, 1 << 20)), 0);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

void SetRecvBuf(SOCKET s, int bytes) {
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof(bytes));
}

void SetNoDelay(SOCKET s) {
    BOOL on = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));
}

std::string AddrToString(const sockaddr_storage& a) {
    char host[INET6_ADDRSTRLEN] = {};
    if (a.ss_family == AF_INET) {
        auto* s4 = reinterpret_cast<const sockaddr_in*>(&a);
        inet_ntop(AF_INET, &s4->sin_addr, host, sizeof(host));
        return host;
    }
    if (a.ss_family == AF_INET6) {
        auto* s6 = reinterpret_cast<const sockaddr_in6*>(&a);
        if (IN6_IS_ADDR_V4MAPPED(&s6->sin6_addr)) {
            inet_ntop(AF_INET, &s6->sin6_addr.s6_addr[12], host, sizeof(host));
        } else {
            inet_ntop(AF_INET6, &s6->sin6_addr, host, sizeof(host));
        }
        return host;
    }
    return "?";
}

uint16_t AddrPort(const sockaddr_storage& a) {
    if (a.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(&a)->sin_port);
    if (a.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&a)->sin6_port);
    return 0;
}

sockaddr_storage WithPort(const sockaddr_storage& a, uint16_t port) {
    sockaddr_storage r = a;
    if (r.ss_family == AF_INET) reinterpret_cast<sockaddr_in*>(&r)->sin_port = htons(port);
    if (r.ss_family == AF_INET6) reinterpret_cast<sockaddr_in6*>(&r)->sin6_port = htons(port);
    return r;
}

int AddrLen(const sockaddr_storage& a) {
    return a.ss_family == AF_INET ? int(sizeof(sockaddr_in)) : int(sizeof(sockaddr_in6));
}

sockaddr_storage ForSocket(SOCKET s, const sockaddr_storage& a) {
    sockaddr_storage local{};
    int len = sizeof(local);
    getsockname(s, reinterpret_cast<sockaddr*>(&local), &len);
    if (local.ss_family == a.ss_family) return a;
    sockaddr_storage r{};
    if (local.ss_family == AF_INET && a.ss_family == AF_INET6) {
        auto* s6 = reinterpret_cast<const sockaddr_in6*>(&a);
        auto* s4 = reinterpret_cast<sockaddr_in*>(&r);
        s4->sin_family = AF_INET;
        s4->sin_port = s6->sin6_port;
        std::memcpy(&s4->sin_addr, &s6->sin6_addr.s6_addr[12], 4);
    } else if (local.ss_family == AF_INET6 && a.ss_family == AF_INET) {
        auto* s4 = reinterpret_cast<const sockaddr_in*>(&a);
        auto* s6 = reinterpret_cast<sockaddr_in6*>(&r);
        s6->sin6_family = AF_INET6;
        s6->sin6_port = s4->sin_port;
        s6->sin6_addr.s6_addr[10] = 0xFF;
        s6->sin6_addr.s6_addr[11] = 0xFF;
        std::memcpy(&s6->sin6_addr.s6_addr[12], &s4->sin_addr, 4);
    } else {
        return a;
    }
    return r;
}

std::vector<Ipv4Iface> ListIpv4Interfaces() {
    std::vector<Ipv4Iface> out;
    ULONG size = 32 * 1024;
    std::vector<uint8_t> buf(size);
    ULONG flags = GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        rc = GetAdaptersAddresses(AF_INET, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()),
                                  &size);
    }
    if (rc != NO_ERROR) return out;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (a->Flags & IP_ADAPTER_NO_MULTICAST) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            Ipv4Iface i;
            i.index = a->IfIndex;
            i.addr = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr;
            ULONG mask = 0;
            ConvertLengthToIpv4Mask(u->OnLinkPrefixLength, &mask);
            i.mask.s_addr = mask;
            i.hasGateway = a->FirstGatewayAddress != nullptr;
            uint8_t b0 = reinterpret_cast<uint8_t*>(&i.addr)[0], b1 = reinterpret_cast<uint8_t*>(&i.addr)[1];
            i.linkLocal = (b0 == 169 && b1 == 254);
            i.name = WideToUtf8(a->FriendlyName ? a->FriendlyName : L"");
            i.description = WideToUtf8(a->Description ? a->Description : L"");
            out.push_back(i);
        }
    }
    return out;
}

}  // namespace net
