#include "config.h"

#include "crypto/crypto.h"

namespace {
std::wstring Read(const std::wstring& path, const wchar_t* section, const wchar_t* key) {
    wchar_t buf[512];
    GetPrivateProfileStringW(section, key, L"", buf, 512, path.c_str());
    return buf;
}

void Write(const std::wstring& path, const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), path.c_str());
}

int ReadInt(const std::wstring& path, const wchar_t* section, const wchar_t* key, int def) {
    return int(GetPrivateProfileIntW(section, key, def, path.c_str()));
}

std::string MakeUuid() {
    uint8_t b[16];
    crypto::RandomBytes(b, sizeof(b));
    b[6] = uint8_t((b[6] & 0x0F) | 0x40);
    b[8] = uint8_t((b[8] & 0x3F) | 0x80);
    std::string h = HexEncode(b, 16);
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-" + h.substr(12, 4) + "-" + h.substr(16, 4) + "-" + h.substr(20, 12);
}
}  // namespace

std::string DefaultReceiverName() {
    wchar_t buf[256];
    DWORD n = 256;
    std::wstring host;
    if (GetComputerNameExW(ComputerNameDnsHostname, buf, &n)) host.assign(buf, n);
    if (host.empty()) return "AirGlass";
    return WideToUtf8(host);
}

void Config::Load() {
    path = AppDataDir() + L"\\config.ini";
    bool dirty = false;

    name = WideToUtf8(Read(path, L"receiver", L"name"));
    if (name.empty()) {
        name = DefaultReceiverName();
        dirty = true;
    }
    // Dots would split the DNS-SD instance label; keep names short.
    for (char& c : name)
        if (c == '.') c = ' ';
    if (name.size() > 40) name.resize(40);

    std::vector<uint8_t> v;
    if (HexDecode(WideToUtf8(Read(path, L"receiver", L"deviceid")), v) && v.size() == 6) {
        std::memcpy(mac, v.data(), 6);
    } else {
        crypto::RandomBytes(mac, 6);
        mac[0] = uint8_t((mac[0] & 0xFC) | 0x02);  // locally administered, unicast
        dirty = true;
    }
    if (HexDecode(WideToUtf8(Read(path, L"receiver", L"key")), v) && v.size() == 32) {
        std::memcpy(seed, v.data(), 32);
    } else {
        crypto::RandomBytes(seed, 32);
        dirty = true;
    }
    pi = WideToUtf8(Read(path, L"receiver", L"pi"));
    if (pi.empty()) {
        pi = MakeUuid();
        dirty = true;
    }
    displayUuid = WideToUtf8(Read(path, L"receiver", L"display"));
    if (displayUuid.empty()) {
        displayUuid = MakeUuid();
        dirty = true;
    }
    port = ReadInt(path, L"receiver", L"port", 7000);
    displayW = ReadInt(path, L"video", L"width", 0);
    displayH = ReadInt(path, L"video", L"height", 0);
    displayHz = ReadInt(path, L"video", L"refresh", 0);
    maxFps = ReadInt(path, L"video", L"maxfps", 0);
    pinned = ReadInt(path, L"window", L"pinned", 0) != 0;
    longPortrait = ReadInt(path, L"window", L"portrait", 0);
    longLandscape = ReadInt(path, L"window", L"landscape", 0);
    welcomed = ReadInt(path, L"app", L"welcomed", 0) != 0;
    debugLog = ReadInt(path, L"app", L"debuglog", 0) != 0;
    if (dirty) Save();
}

void Config::Save() const {
    Write(path, L"receiver", L"name", Utf8ToWide(name));
    Write(path, L"receiver", L"deviceid", Utf8ToWide(HexEncode(mac, 6, true)));
    Write(path, L"receiver", L"key", Utf8ToWide(HexEncode(seed, 32)));
    Write(path, L"receiver", L"pi", Utf8ToWide(pi));
    Write(path, L"receiver", L"display", Utf8ToWide(displayUuid));
    Write(path, L"receiver", L"port", std::to_wstring(port));
    Write(path, L"video", L"width", std::to_wstring(displayW));
    Write(path, L"video", L"height", std::to_wstring(displayH));
    Write(path, L"video", L"refresh", std::to_wstring(displayHz));
    Write(path, L"video", L"maxfps", std::to_wstring(maxFps));
    Write(path, L"window", L"pinned", pinned ? L"1" : L"0");
    Write(path, L"window", L"portrait", std::to_wstring(longPortrait));
    Write(path, L"window", L"landscape", std::to_wstring(longLandscape));
    Write(path, L"app", L"welcomed", welcomed ? L"1" : L"0");
    Write(path, L"app", L"debuglog", debugLog ? L"1" : L"0");
}
