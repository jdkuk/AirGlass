// Winsock helpers: dual-stack sockets, blocking I/O with stop flags, interface enumeration.
#pragma once

#include "common.h"

namespace net {

bool Startup();
void Cleanup();
// Test mode: all listening sockets bind to 127.0.0.1 only.
void SetLoopbackOnly(bool on);

// Dual-stack (IPv6 + IPv4-mapped) sockets. port: in = requested (0 = any), out = bound port.
SOCKET TcpListen(uint16_t& port, bool exclusive = false);
SOCKET UdpBind(uint16_t& port);
void CloseSocket(SOCKET& s);

bool SendAll(SOCKET s, const void* data, size_t n);
// Receives exactly n bytes. Uses select() with short timeouts so that *stop can abort it.
// Returns false on close, error or stop.
bool RecvExact(SOCKET s, void* buf, size_t n, const std::atomic<bool>* stop);
// Waits for readability; returns 1 readable, 0 timeout, -1 error.
int WaitReadable(SOCKET s, int timeoutMs);

void SetRecvBuf(SOCKET s, int bytes);
void SetNoDelay(SOCKET s);

std::string AddrToString(const sockaddr_storage& a);
uint16_t AddrPort(const sockaddr_storage& a);
sockaddr_storage WithPort(const sockaddr_storage& a, uint16_t port);
int AddrLen(const sockaddr_storage& a);
// Converts an address to the family of the given socket (IPv4 <-> IPv4-mapped IPv6).
sockaddr_storage ForSocket(SOCKET s, const sockaddr_storage& a);

struct Ipv4Iface {
    uint32_t index = 0;
    in_addr addr{};
    in_addr mask{};
    bool hasGateway = false;
    bool linkLocal = false;
    std::string name;
    std::string description;
};
std::vector<Ipv4Iface> ListIpv4Interfaces();

}  // namespace net
