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
    };

    AirPlayServer();
    ~AirPlayServer();
    bool Start(const ReceiverInfo& info, uint16_t preferredPort, FrameSink* video, AudioOutput* audio, Events ev);
    void Stop();
    uint16_t Port() const { return port_; }
    // Drop the active mirroring sender (user closed the window).
    void DisconnectMirror();
    // Current music session; false (and out.active = false) when there is none.
    bool GetNowPlaying(NowPlaying& out);
    // Remote control of the music sender: playpause, play, pause, next, prev, volume (0..1).
    // Blocks for up to ~2 s. Returns "" on success or "no-session" / "no-remote" / "failed".
    std::string MediaCommand(const std::string& action, double value);

private:
    class Connection;
    friend class Connection;
    void AcceptLoop();
    void ClaimMirror(const std::shared_ptr<Connection>& c);
    void ReleaseMirror(Connection* c);
    void ClaimAudio(const std::shared_ptr<Connection>& c);
    void ReleaseAudio(Connection* c);
    void Reap();

    ReceiverInfo info_;
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
};
