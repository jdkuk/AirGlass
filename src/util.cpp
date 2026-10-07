// Logging, string, time and path helpers.
#include "common.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>

namespace {
std::mutex g_logMutex;
FILE* g_logFile = nullptr;
LogLevel g_logMin = LogLevel::Info;
}  // namespace

void LogInit(const std::wstring& path, LogLevel minLevel) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    g_logMin = minLevel;
    // Keep one previous log around.
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) {
        std::wstring old = path + L".old";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_logFile = _wfopen(path.c_str(), L"wb");
}

void LogShutdown() {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = nullptr;
    }
}

void LogWrite(LogLevel level, const char* fmt, ...) {
    if (level < g_logMin) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    static const char* kLevels[] = {"DBG", "INF", "WRN", "ERR"};
    char line[4300];
    int n = snprintf(line, sizeof(line), "%02d:%02d:%02d.%03d [%s] (%5lu) %s\r\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, kLevels[int(level)], GetCurrentThreadId(), msg);
    if (n < 0) return;
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (g_logFile) {
        fwrite(line, 1, strlen(line), g_logFile);
        fflush(g_logFile);
    } else {
        fputs(line, stderr);
    }
    OutputDebugStringA(line);
}

std::string HexEncode(const uint8_t* p, size_t n, bool upper) {
    static const char* lo = "0123456789abcdef";
    static const char* up = "0123456789ABCDEF";
    const char* d = upper ? up : lo;
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

bool HexDecode(const std::string& s, std::vector<uint8_t>& out) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(uint8_t((a << 4) | b));
    }
    return true;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

static double QpcFreq() {
    static double f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return double(li.QuadPart);
    }();
    return f;
}

double NowSeconds() {
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return double(li.QuadPart) / QpcFreq();
}

uint64_t NowMicros() { return uint64_t(NowSeconds() * 1e6); }

std::wstring AppDataDir() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\AirGlass";
        CoTaskMemFree(base);
    } else {
        dir = L".\\AirGlass";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}
