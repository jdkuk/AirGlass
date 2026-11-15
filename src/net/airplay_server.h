// AirPlay receiver: RTSP control connections, pairing, FairPlay, stream setup.
#pragma once

#include "common.h"
#include "media/audio_decoder.h"
#include "media/audio_output.h"
#include "media/frame_sink.h"

// Snapshot of an audio-only (music) AirPlay session for TV Mode's Now Playing player.
struct NowPlaying {
    uint64_t session = 0;
    bool active = false;
    std::string device, title, artist, album;
    int64_t durationMs = -1, positionMs = -1;  // -1 = unknown
    bool playing = false;
    uint32_t artworkSeq = 0;  // 0 = no artwork
    std::string artworkType;  // "image/jpeg" / "image/png"
    std::wstring artworkPath;
    float volume = 1.0f;     // 0..1 (sender's AirPlay volume)
    bool controls = false;   // the sender's DACP remote control was found
    AudioFormat format;
};

struct ReceiverInfo {
    std::string name;             // shown on the sender, e.g. "Office"
    uint8_t mac[6]{};             // device id
    uint8_t edPub[32]{}, edPriv[64]{};
    std::string pi;               // persistent UUID
    std::string displayUuid;
    uint64_t features = 0;
    bool pin = false;             // a code is required to connect (TXT "pw"; see AirPlayServer::SetPinRequired)
    int width = 1920, height = 1080, refresh = 60, maxFps = 60;

    std::string DeviceId() const;  // "AA:BB:CC:DD:EE:FF"
    std::string MacHex() const;    // "AABBCCDDEEFF"
    std::string PkHex() const;
    std::vector<std::pair<std::string, std::string>> AirPlayTxt() const;
    std::vector<std::pair<std::string, std::string>> RaopTxt() const;
};

class AirPlayServer {
public:
    struct Events {
        std::function<void(uint64_t sid, const std::string& deviceName, const std::string& model)> mirrorStarted;
        std::function<void(uint64_t sid)> mirrorStopped;
        std::function<void(uint64_t sid, int w, int h)> videoSize;
        // Anything about the music session changed; read it with GetNowPlaying(). Any thread.
        std::function<void()> nowPlayingChanged;
        // Allow-list: asked on the connection's thread when a sender starts a mirroring or audio session, before
        // any keys or streams are set up; false turns the sender away (RTSP 403, connection closed). It may block
        // while the user decides (the caller times out); `gone` says whether the sender hung up meanwhile.
        std::function<bool(const std::string& senderId, const std::string& name, const std::function<bool()>& gone)>
            admit;
        // PIN pairing ("AirPlay Code"). Called on connection threads; they post and return, never block.
        // pinCode: show `code` on the PC for the sender that is about to ask for it; "" hides it again (paired,
        // wrong, expired or the sender gave up).
        std::function<void(const std::string& code)> pinCode;
        // A sender proved it knows the code: remember its Ed25519 key (64 hex digits) so it isn't asked again.
        // `name` is its device id at first and the name it gives once it starts a session.
        std::function<void(const std::string& keyHex, const std::string& name)> pinPaired;
    };

    AirPlayServer();
    ~AirPlayServer();
    bool Start(const ReceiverInfo& info, uint16_t preferredPort, FrameSink* video, AudioOutput* audio, Events ev);
    void Stop();
    uint16_t Port() const { return port_; }
    // Live rename: /info answers with the new name (mDNS is re-announced by the caller).
    void SetName(const std::string& name);
    std::string Name();
    // Drop the active mirroring sender (user closed the window).
    void DisconnectMirror();
    // Current music session; false (and out.active = false) when there is none.
    bool GetNowPlaying(NowPlaying& out);
    // Remote control of the music sender: playpause, play, pause, next, prev, volume (0..1).
    // Blocks for up to ~2 s. Returns "" on success or "no-session" / "no-remote" / "failed".
    std::string MediaCommand(const std::string& action, double value);
    // PIN pairing. When required, a sender must have paired with a code shown on the PC (now, or earlier with a
    // key in SetPairedKeys) before it may set up a session; /info advertises it too (mDNS is the caller's).
    // Senders that ask for a code by themselves get one either way.
    void SetPinRequired(bool on) { pinRequired_ = on; }
    bool PinRequired() const { return pinRequired_; }
    // Ed25519 keys (hex) of senders that paired with a code before (config.ini [devices]); replaces the set.
    void SetPairedKeys(const std::vector<std::string>& keysHex);
    // Loopback tests only: every code is this one, so the test sender can type it.
    void SetTestPin(const std::string& pin);
    static constexpr double kPinCodeSeconds = 120;  // a code on the PC is good for this long

private:
    class Connection;
    friend class Connection;
    void AcceptLoop();
    void ClaimMirror(const std::shared_ptr<Connection>& c);
    void ReleaseMirror(Connection* c);
    void ClaimAudio(const std::shared_ptr<Connection>& c);
    void ReleaseAudio(Connection* c);
    void Reap();
    // PIN pairing (any connection thread). One code is showing at a time; a new attempt replaces it.
    std::string IssuePin(uint64_t conn, int* lockedFor);  // "" while locked out after wrong codes
    bool TakePin(uint64_t conn, std::string* code);       // the showing code, once, if it hasn't expired
    void PinTried(bool right);                            // wrong codes lead to a growing lock-out
    void PinDone(uint64_t conn);  // hides the card if it shows conn's code (paired, wrong, or connection ended)
    bool IsPaired(const uint8_t key[32]);
    void AddPaired(const uint8_t key[32]);

    ReceiverInfo info_;
    std::mutex nameMu_;
    std::string name_;  // info_.name, renamable while running (guarded by nameMu_)
    FrameSink* video_ = nullptr;
    AudioOutput* audio_ = nullptr;
    Events ev_;
    SOCKET listen_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::thread acceptThread_;
    std::atomic<bool> stop_{false};

    std::mutex mu_;
    std::vector<std::shared_ptr<Connection>> conns_;
    std::weak_ptr<Connection> mirror_;
    std::weak_ptr<Connection> audioConn_;  // the music (audio-only) session, if any
    uint64_t nextId_ = 1;
    std::atomic<uint32_t> artworkSeq_{0};

    std::atomic<bool> pinRequired_{false};
    std::mutex pinMu_;  // guards the rest
    std::vector<std::string> pairedKeys_;  // lowercase hex
    std::string pinCode_, testPin_;
    uint64_t pinConn_ = 0;
    double pinIssued_ = 0;
    int pinWrong_ = 0, pinLockSeconds_ = 0;
    double pinLockedUntil_ = 0;
};
