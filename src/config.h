// Persistent settings in %LOCALAPPDATA%\AirGlass\config.ini.
#pragma once

#include "common.h"

struct Config {
    std::wstring path;
    std::string name;          // receiver name shown on the sender
    uint8_t mac[6]{};          // persistent device id
    uint8_t seed[32]{};        // Ed25519 identity seed
    std::string pi, displayUuid;
    int port = 7000;
    int displayW = 0, displayH = 0, displayHz = 0, maxFps = 0;  // 0 = automatic
    bool pinned = false;
    int longPortrait = 0, longLandscape = 0;
    bool welcomed = false;
    bool debugLog = false;
    std::string licenseKey, licenseInstance;  // AirGlass Pro (Lemon Squeezy key + activation id)

    void Load();   // creates identity on first run
    void Save() const;
};

std::string DefaultReceiverName();
