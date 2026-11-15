// Persistent settings in %LOCALAPPDATA%\AirGlass\config.ini.
#pragma once

#include "common.h"

#include <map>

struct Config {
    std::wstring path;
    std::string name;          // receiver name shown on the sender
    bool nameManaged = false;  // set machine-wide in %ProgramData%\AirGlass\airglass.ini (docs/deployment.md)
    uint8_t mac[6]{};          // persistent device id
    uint8_t seed[32]{};        // Ed25519 identity seed
    std::string pi, displayUuid;
    int port = 7000;
    int displayW = 0, displayH = 0, displayHz = 0, maxFps = 0;  // 0 = automatic
    bool pinned = false;
    int longPortrait = 0, longLandscape = 0;
    bool obsMode = false;  // Pro: OBS mode, [window] obs
    bool welcomed = false;
    bool debugLog = false;
    bool statusHttp = false;   // [status] http=1: serve GET /status on the AirPlay port (docs/status-http.md)
    bool keepAwake = true;     // [display] keepawake=0: let the display sleep during a mirroring session
    bool wakeDisplay = true;   // [display] wake=0: don't wake a sleeping display when mirroring starts
    bool startFull = false;    // [window] full=1: every window opens full screen (e.g. a PC whose screen is a TV)
    std::string licenseKey, licenseInstance;  // AirGlass Pro (Lemon Squeezy key + activation id)
    long long proUnlocked = 0;                // unix time Pro was unlocked on this PC (refund window)
    int proSessions = 0;                      // mirroring sessions since then (refund: fewer than 10)
    long long trialStarted = 0;               // unix time of the first mirroring session (7-day Pro trial)
    bool trialEndShown = false;               // the "trial ended" dialog was shown once
    // Allow-list (Pro): ask on the PC before a device that isn't known yet mirrors or plays audio.
    // [security] askNewDevices; an IT department can turn it on for the machine in %ProgramData%\AirGlass\
    // airglass.ini (askManaged: then it can't be turned off here and isn't written to config.ini).
    bool askNewDevices = false, askManaged = false;
    struct KnownDevice {
        bool allowed = false;  // "Always allow" (true) or "Block" (false)
        std::string name;      // as the sender called itself, for people reading config.ini
    };
    std::map<std::string, KnownDevice> devices;  // [devices] <sender id>=allow|block <name>
    // PIN pairing (Pro): require a code shown on the PC before a device connects. [security] requirePin; also
    // machine-wide in airglass.ini (pinManaged, as askManaged). Devices that paired with a code are kept by their
    // Ed25519 key: [devices] PIN:<64 hex digits>=paired <name>.
    bool requirePin = false, pinManaged = false;
    std::map<std::string, std::string> paired;  // key hex -> name

    void Load();   // creates identity on first run
    void Save() const;
};

std::string DefaultReceiverName();
// A usable receiver name: no dots (they would split the DNS-SD instance label) or control characters,
// trimmed, at most 40 bytes. Empty if nothing is left.
std::string SanitizeReceiverName(std::string name);
// %ProgramData%\AirGlass\airglass.ini: machine-wide settings an IT department deploys ("" if unknown).
std::wstring MachineConfigPath();
