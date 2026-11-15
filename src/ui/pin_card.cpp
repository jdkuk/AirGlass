#include "ui/pin_card.h"

#include <shellscalingapi.h>

namespace ui {
namespace {

constexpr UINT_PTR kTimerExpire = 1;
constexpr int kW = 380, kH = 210, kRadius = 22;  // DIPs
constexpr COLORREF kBack = RGB(28, 28, 30), kText = RGB(255, 255, 255), kDim = RGB(174, 174, 178);

HFONT Font(float dp, int px, int weight) {
    return CreateFontW(-int(float(px) * dp), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

}  // namespace

PinCard::~PinCard() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void PinCard::Show(const std::wstring& code, const std::wstring& receiver, HMONITOR mon, double seconds) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    if (!hwnd_) {
        static bool registered = false;
        if (!registered) {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = Proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = L"AirGlassPinCard";
            RegisterClassExW(&wc);
            registered = true;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"AirGlassPinCard",
                                L"AirPlay code", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, inst, this);
        if (!hwnd_) {
            LOGE("pin: card window failed (error %lu)", GetLastError());
            return;
        }
    }
    code_ = code;
    receiver_ = receiver;
    deadline_ = NowSeconds() + seconds;
    UINT dx = 96, dy = 96;
    if (!mon) mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    dp_ = float(dx) / 96.0f;
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    int w = int(float(kW) * dp_), h = int(float(kH) * dp_);
    const RECT& wa = mi.rcWork;
    int x = wa.left + (wa.right - wa.left - w) / 2, y = wa.top + (wa.bottom - wa.top - h) * 2 / 5;
    int r = int(float(kRadius) * dp_);
    SetWindowRgn(hwnd_, CreateRoundRectRgn(0, 0, w + 1, h + 1, 2 * r, 2 * r), FALSE);
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(hwnd_, nullptr, TRUE);
    SetTimer(hwnd_, kTimerExpire, 500, nullptr);
    MessageBeep(MB_ICONINFORMATION);
    LOGI("pin: showing the code card");
}

void PinCard::Hide() {
    if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
    KillTimer(hwnd_, kTimerExpire);
    ShowWindow(hwnd_, SW_HIDE);
    SecureZeroMemory(code_.data(), code_.size() * sizeof(wchar_t));
    code_.clear();
    LOGI("pin: code card hidden");
}

void PinCard::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    HBRUSH back = CreateSolidBrush(kBack);
    FillRect(dc, &rc, back);
    DeleteObject(back);
    SetBkMode(dc, TRANSPARENT);
    auto text = [&](const std::wstring& s, int px, int weight, COLORREF color, int top, int bottom, UINT flags) {
        HFONT f = Font(dp_, px, weight);
        HGDIOBJ old = SelectObject(dc, f);
        SetTextColor(dc, color);
        int pad = int(24 * dp_);
        RECT r{rc.left + pad, int(float(top) * dp_), rc.right - pad, int(float(bottom) * dp_)};
        DrawTextW(dc, s.c_str(), int(s.size()), &r, DT_CENTER | DT_NOPREFIX | flags);
        SelectObject(dc, old);
        DeleteObject(f);
    };
    std::wstring spaced;
    for (wchar_t c : code_) {
        if (!spaced.empty()) spaced += L" ";  // thin space: "4 8 2 1" reads digit by digit
        spaced += c;
    }
    text(L"AirPlay code", 16, FW_SEMIBOLD, kDim, 20, 46, DT_SINGLELINE | DT_VCENTER);
    text(spaced, 72, FW_SEMIBOLD, kText, 46, 140, DT_SINGLELINE | DT_VCENTER);
    text(L"Enter this code on the iPhone, iPad or Mac connecting to “" + receiver_ + L"”.", 14, FW_NORMAL, kDim, 146,
         196, DT_WORDBREAK);
    SecureZeroMemory(spaced.data(), spaced.size() * sizeof(wchar_t));
}

LRESULT CALLBACK PinCard::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<PinCard*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
        break;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_LBUTTONUP:
        if (self) self->Hide();
        return 0;
    case WM_TIMER:
        if (self && w == kTimerExpire && NowSeconds() >= self->deadline_) self->Hide();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (self) self->Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        if (self) self->hwnd_ = nullptr;
        break;
    default: break;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace ui
