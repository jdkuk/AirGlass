// AirPlay NTP-style timing: the receiver polls the sender's timing port every few seconds.
// Also used as a liveness check - if the sender stops answering the session is ended.
#pragma once

#include "common.h"
#include "net/netutil.h"

class NtpClient {
public:
    ~NtpClient() { Stop(); }
    bool Start(const sockaddr_storage& peer, uint16_t remotePort, std::function<void()> onDead);
    void Stop();
    uint16_t LocalPort() const { return localPort_; }

    // Current NTP time (seconds since 1900 in the high 32 bits).
    static uint64_t NtpNow();

private:
    void Run();
    SOCKET sock_ = INVALID_SOCKET;
    sockaddr_storage remote_{};
    uint16_t localPort_ = 0;
    std::function<void()> onDead_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
};
