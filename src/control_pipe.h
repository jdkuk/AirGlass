// Local control channel for TV Mode: named pipe \\.\pipe\AirGlass.Control, one JSON object per
// line in both directions (protocol: tv-remote/docs/airplay-integration.md).
//   AirGlass -> client: {"event":"state"|"started"|"size"|"layoutChanged"|"stopped"|"userClosed"|
//                        "nowplaying"|"mediaResult", ...}
//   client -> AirGlass: {"cmd":"defaults"|"layout"|"highlight"|"press"|"stop"|"state"|"media", ...}
#pragma once

#include <map>

#include "common.h"

// Minimal JSON for flat command objects (nested objects/arrays are parsed and skipped).
struct JsonValue {
    enum class Type { Null, Bool, Number, String, Other } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
};
using JsonObject = std::map<std::string, JsonValue>;
bool ParseJsonObject(const std::string& text, JsonObject& out);
std::string JsonQuote(const std::string& s);

// Builds one flat JSON object.
class JsonWriter {
public:
    JsonWriter& Str(const char* k, const std::string& v) { return Key(k).Append(JsonQuote(v)); }
    JsonWriter& Int(const char* k, long long v) { return Key(k).Append(std::to_string(v)); }
    JsonWriter& Num(const char* k, double v);
    JsonWriter& Bool(const char* k, bool v) { return Key(k).Append(v ? "true" : "false"); }
    JsonWriter& Null(const char* k) { return Key(k).Append("null"); }
    JsonWriter& Raw(const char* k, const std::string& json) { return Key(k).Append(json); }
    std::string Done() const { return s_ + "}"; }

private:
    JsonWriter& Key(const char* k) {
        s_ += s_.size() > 1 ? "," : "";
        s_ += JsonQuote(k) + ":";
        return *this;
    }
    JsonWriter& Append(const std::string& v) {
        s_ += v;
        return *this;
    }
    std::string s_ = "{";
};

class ControlPipe {
public:
    ~ControlPipe() { Stop(); }
    // onLine: one command line from a client; onClients: the client count changed. Both are
    // called on pipe threads.
    bool Start(const std::wstring& name, std::function<void(const std::string&)> onLine,
               std::function<void(int)> onClients);
    void Stop();
    void Broadcast(const std::string& jsonLine);  // never blocks for long on a stuck client
    int Clients();

private:
    struct Client {
        HANDLE pipe = INVALID_HANDLE_VALUE;
        std::mutex writeMu;
        std::atomic<bool> dead{false};
        std::thread thread;
    };
    void AcceptLoop();
    void ClientLoop(std::shared_ptr<Client> c);
    bool Write(Client& c, const std::string& line);
    void Reap(bool all);
    HANDLE CreateInstance(bool first);

    std::wstring name_;
    std::function<void(const std::string&)> onLine_;
    std::function<void(int)> onClients_;
    HANDLE stopEvent_ = nullptr;
    void* sd_ = nullptr;  // security descriptor (current user + SYSTEM only)
    std::thread acceptThread_;
    std::mutex mu_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::atomic<bool> running_{false};
};
