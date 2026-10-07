// DACP remote control of the sending device (play/pause, next/previous, volume), the way an
// Apple TV / AirPort Express does it: the sender advertises "iTunes_Ctrl_<DACP-ID>._dacp._tcp"
// and accepts HTTP GET /ctrl-int/1/<command> carrying its Active-Remote token.
#pragma once

#include "common.h"

class DacpRemote {
public:
    ~DacpRemote();
    // DACP-ID / Active-Remote headers from the sender's RTSP requests; resolves the control
    // service in the background whenever they change.
    void Configure(const std::string& dacpId, const std::string& activeRemote, const sockaddr_storage& peer);
    bool Available() const { return port_.load() != 0; }
    // Sends e.g. "playpause", "nextitem", "setproperty?dmcp.device-volume=-15.000000".
    // Blocks for up to ~2 s. Returns "" on success, otherwise "no-remote" or "failed".
    std::string Send(const std::string& command);
    std::function<void()> onAvailable;  // called on a background thread once resolved

private:
    void Resolve(uint64_t gen, std::string id);

    std::mutex mu_;
    std::string id_, token_;
    sockaddr_storage peer_{};
    std::atomic<uint16_t> port_{0};
    std::atomic<uint64_t> gen_{0};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
