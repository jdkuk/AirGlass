#include "ui/allow.h"

#include "ui/dialog_template.h"

namespace ui {
namespace {

constexpr int kIdQuestion = 501, kIdCountdown = 502, kIdOnce = 503, kIdAlways = 504, kIdBlock = 505;
constexpr UINT_PTR kTimerPoll = 1;

struct AllowState {
    HICON icon = nullptr;
    std::wstring device;
    double deadline = 0;
    std::function<bool()> settled;
};

void ShowCountdown(HWND h, const AllowState* st) {
    int left = std::max(0, int(std::ceil(st->deadline - NowSeconds())));
    wchar_t t[96];
    swprintf(t, 96, L"If nobody answers, it is turned away in %d s.", left);
    SetDlgItemTextW(h, kIdCountdown, t);
}

INT_PTR CALLBACK AllowProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<AllowState*>(GetWindowLongPtrW(h, DWLP_USER));
    switch (m) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(h, DWLP_USER, l);
        st = reinterpret_cast<AllowState*>(l);
        SendMessageW(h, WM_SETICON, ICON_BIG, LPARAM(st->icon));
        std::wstring q = st->device.empty() ? std::wstring(L"Allow a device with no name to use AirGlass?")
                                            : L"Allow “" + st->device + L"” to use AirGlass?";
        SetDlgItemTextW(h, kIdQuestion, q.c_str());
        ShowCountdown(h, st);
        SetTimer(h, kTimerPoll, 250, nullptr);
        // Over a full-screen mirroring window too, and noticed even if Windows keeps the focus elsewhere.
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(h);
        FLASHWINFO fi{sizeof(fi), h, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&fi);
        MessageBeep(MB_ICONQUESTION);
        SetFocus(GetDlgItem(h, kIdOnce));
        return FALSE;  // focus set by hand
    }
    case WM_TIMER:
        if (w == kTimerPoll) {
            if (NowSeconds() >= st->deadline || (st->settled && st->settled())) {
                KillTimer(h, kTimerPoll);
                EndDialog(h, kAllowDeny);
            } else {
                ShowCountdown(h, st);
            }
        }
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case kIdOnce: EndDialog(h, kAllowOnce); return TRUE;
        case kIdAlways: EndDialog(h, kAllowAlways); return TRUE;
        case kIdBlock: EndDialog(h, kAllowBlock); return TRUE;
        case IDCANCEL: EndDialog(h, kAllowDeny); return TRUE;
        default: break;  // Enter (IDOK) answers nothing: a stray key press never lets a device in
        }
        break;
    }
    return FALSE;
}

}  // namespace

AllowAnswer ShowAllowDialog(HWND owner, HICON icon, const std::wstring& device, double deadline,
                            const std::function<bool()>& settled) {
    Template t;
    t.Dialog(L"AirGlass", 250, 92, 6);
    t.Item(kStatic, L"", kIdQuestion, 10, 10, 230, 20, SS_LEFT);
    t.Item(kStatic, L"It wants to mirror its screen or play audio on this PC. “Always allow” and “Block” are "
                    L"remembered.",
           -1, 10, 32, 230, 18, SS_LEFT);
    t.Item(kStatic, L"", kIdCountdown, 10, 53, 230, 10, SS_LEFT);
    t.Item(kButton, L"Allow once", kIdOnce, 82, 70, 50, 15, BS_PUSHBUTTON | WS_TABSTOP);
    t.Item(kButton, L"Always allow", kIdAlways, 136, 70, 54, 15, BS_PUSHBUTTON | WS_TABSTOP);
    t.Item(kButton, L"Block", kIdBlock, 194, 70, 46, 15, BS_PUSHBUTTON | WS_TABSTOP);
    AllowState st;
    st.icon = icon;
    st.device = device;
    st.deadline = deadline;
    st.settled = settled;
    INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), owner, AllowProc, LPARAM(&st));
    if (rc < 0) LOGE("devices: prompt failed (error %lu)", GetLastError());
    return rc == kAllowOnce || rc == kAllowAlways || rc == kAllowBlock ? AllowAnswer(rc) : kAllowDeny;
}

}  // namespace ui
