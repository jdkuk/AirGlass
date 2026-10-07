// Minimal multicast DNS responder (RFC 6762 / 6763) advertising the AirPlay services.
// Runs its own socket on UDP 5353 (shared with other responders via SO_REUSEADDR) and
// answers per-interface so each client gets the address of the interface it asked on.
#pragma once

#include "common.h"
#include "net/netutil.h"

class MdnsResponder {
public:
    struct Service {
        std::string instance;  // e.g. "Office" or "AABBCCDDEEFF@Office"
        std::string type;      // e.g. "_airplay._tcp"
        uint16_t port = 0;
        std::vector<std::pair<std::string, std::string>> txt;
    };

    ~MdnsResponder() { Stop(); }
    bool Start(const std::string& hostLabel, std::vector<Service> services);
    void Stop();

    // TXT rdata encoding (length-prefixed "key=value" strings).
    static std::vector<uint8_t> EncodeTxt(const std::vector<std::pair<std::string, std::string>>& kv);

    // Asks the network which AirPlay/RAOP receiver names are in use (RAOP names without the
    // "MAC@" prefix). Blocks for `seconds`.
    static std::vector<std::string> BrowseReceiverNames(double seconds);

    // One-shot SRV lookup of "<instance>.<type>.local" (e.g. a sender's
    // "iTunes_Ctrl_<id>._dacp._tcp"). Blocks for up to `seconds`; returns false if nobody
    // answered. addr is filled when the answer carried the target's A record.
    static bool ResolveService(const std::string& instance, const std::string& type, double seconds,
                               uint16_t& port, in_addr* addr);

private:
    struct Want {
        std::vector<bool> ptr, srv, txt;  // per service
        bool servicesEnum = false;
        bool hostA = false;
        bool any() const;
    };
    void Run();
    void RefreshInterfaces(bool announceNew);
    void SendAnnouncement(const net::Ipv4Iface& iface, uint32_t ttl);
    void HandlePacket(const uint8_t* p, int n, const sockaddr_in& from, uint32_t ifIndex, in_addr dst);
    bool BuildResponse(std::vector<uint8_t>& out, const Want& want, in_addr hostAddr, bool legacy, uint16_t id,
                       const uint8_t* questions, size_t questionsLen, uint16_t qdcount, uint32_t ttlOverride);
    const net::Ipv4Iface* IfaceFor(uint32_t ifIndex, in_addr src) const;
    void SendTo(const std::vector<uint8_t>& pkt, const sockaddr_in& to, const net::Ipv4Iface* iface);

    std::string host_;  // "Office-AirGlass"
    std::vector<Service> services_;
    std::vector<std::vector<uint8_t>> txtRdata_;
    SOCKET sock_ = INVALID_SOCKET;
    void* recvMsg_ = nullptr;  // LPFN_WSARECVMSG
    std::vector<net::Ipv4Iface> ifaces_;
    std::vector<uint32_t> answered_;  // peers we have answered at least once (for the log)
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mu_;
};
