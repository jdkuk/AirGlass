// Shared includes and small helpers used across AirGlass.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log.h"

// Minimal COM smart pointer (avoids depending on WRL).
template <class T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(const ComPtr& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComPtr(ComPtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComPtr() { Reset(); }
    ComPtr& operator=(const ComPtr& o) {
        if (this != &o) { Reset(); p_ = o.p_; if (p_) p_->AddRef(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& o) noexcept {
        if (this != &o) { Reset(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    ComPtr& operator=(std::nullptr_t) { Reset(); return *this; }
    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }
    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    T** ReleaseAndGetAddressOf() { Reset(); return &p_; }
    T** GetAddressOf() { return &p_; }
    explicit operator bool() const { return p_ != nullptr; }
    void Attach(T* p) { Reset(); p_ = p; }
    T* Detach() { T* t = p_; p_ = nullptr; return t; }
    static ComPtr FromRaw(T* p) { ComPtr c; c.p_ = p; if (p) p->AddRef(); return c; }
    template <class U>
    HRESULT As(ComPtr<U>& out) const {
        return p_ ? p_->QueryInterface(__uuidof(U), reinterpret_cast<void**>(out.ReleaseAndGetAddressOf()))
                  : E_POINTER;
    }
private:
    T* p_ = nullptr;
};

struct RectF {
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    float W() const { return x1 - x0; }
    float H() const { return y1 - y0; }
    float CX() const { return (x0 + x1) * 0.5f; }
    float CY() const { return (y0 + y1) * 0.5f; }
    RectF Offset(float dx, float dy) const { return {x0 + dx, y0 + dy, x1 + dx, y1 + dy}; }
    RectF Inflate(float d) const { return {x0 - d, y0 - d, x1 + d, y1 + d}; }
    bool Contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
    static RectF FromRECT(const RECT& r) { return {float(r.left), float(r.top), float(r.right), float(r.bottom)}; }
    RECT ToRECT() const {
        return {LONG(std::lround(x0)), LONG(std::lround(y0)), LONG(std::lround(x1)), LONG(std::lround(y1))};
    }
    static RectF Union(const RectF& a, const RectF& b) {
        return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
    }
};

// Big/little endian helpers.
inline uint16_t RdBE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
inline uint32_t RdBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline uint64_t RdBE64(const uint8_t* p) { return (uint64_t(RdBE32(p)) << 32) | RdBE32(p + 4); }
inline uint32_t RdLE32(const uint8_t* p) {
    return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t RdLE64(const uint8_t* p) { return RdLE32(p) | (uint64_t(RdLE32(p + 4)) << 32); }
inline float RdLEFloat(const uint8_t* p) {
    uint32_t u = RdLE32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
inline void WrBE16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void WrBE32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}
inline void WrBE64(uint8_t* p, uint64_t v) { WrBE32(p, uint32_t(v >> 32)); WrBE32(p + 4, uint32_t(v)); }
inline void WrLE32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}
inline void WrLE64(uint8_t* p, uint64_t v) { WrLE32(p, uint32_t(v)); WrLE32(p + 4, uint32_t(v >> 32)); }

std::string HexEncode(const uint8_t* p, size_t n, bool upper = false);
bool HexDecode(const std::string& s, std::vector<uint8_t>& out);
std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);

// Monotonic time.
double NowSeconds();
uint64_t NowMicros();

// Manual-reset style event wrapper.
class Event {
public:
    explicit Event(bool manualReset = false) { h_ = CreateEventW(nullptr, manualReset, FALSE, nullptr); }
    ~Event() { if (h_) CloseHandle(h_); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
    void Set() { SetEvent(h_); }
    void Reset() { ResetEvent(h_); }
    bool Wait(DWORD ms) { return WaitForSingleObject(h_, ms) == WAIT_OBJECT_0; }
    HANDLE Handle() const { return h_; }
private:
    HANDLE h_ = nullptr;
};

// %LOCALAPPDATA%\AirGlass (created on demand).
std::wstring AppDataDir();
