#include "ui/welcome.h"

#include "ui/dialog_template.h"

namespace ui {
namespace {

constexpr int kIdIcon = 301, kIdTitle = 302, kIdAutostart = 303;

struct WelcomeState {
    HICON icon = nullptr;
    HFONT titleFont = nullptr;
    bool autostart = true;
};

INT_PTR CALLBACK WelcomeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<WelcomeState*>(GetWindowLongPtrW(h, DWLP_USER));
    switch (m) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(h, DWLP_USER, l);
        st = reinterpret_cast<WelcomeState*>(l);
        SendMessageW(h, WM_SETICON, ICON_BIG, LPARAM(st->icon));
        SendDlgItemMessageW(h, kIdIcon, STM_SETICON, WPARAM(st->icon), 0);
        // Title in a larger semibold copy of the dialog font.
        LOGFONTW lf{};
        GetObjectW(HFONT(SendMessageW(h, WM_GETFONT, 0, 0)), sizeof(lf), &lf);
        lf.lfHeight = lf.lfHeight * 3 / 2;
        lf.lfWeight = FW_SEMIBOLD;
        st->titleFont = CreateFontIndirectW(&lf);
        SendDlgItemMessageW(h, kIdTitle, WM_SETFONT, WPARAM(st->titleFont), TRUE);
        CheckDlgButton(h, kIdAutostart, st->autostart ? BST_CHECKED : BST_UNCHECKED);
        // Windows may refuse focus to a just-started app; stay on top so the welcome is never buried.
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        SetForegroundWindow(h);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK || LOWORD(w) == IDCANCEL) {
            st->autostart = IsDlgButtonChecked(h, kIdAutostart) == BST_CHECKED;
            EndDialog(h, 1);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (st && st->titleFont) DeleteObject(st->titleFont);
        break;
    }
    return FALSE;
}

}  // namespace

void ShowWelcome(HWND owner, HICON icon, const std::wstring& receiverName, bool* startWithWindows) {
    std::wstring connect = L"This PC now appears as “" + receiverName +
                           L"”. On your iPhone, iPad or Mac, open Control Centre, tap Screen Mirroring and choose it. "
                           L"Music can be sent to it with the AirPlay button too.";
    std::wstring tray = L"AirGlass keeps running quietly in the system tray, next to the clock. Right-click its icon "
                        L"there for options, or to quit.";
    Template t;
    t.Dialog(L"Welcome to AirGlass", 260, 138, 7);
    t.Item(kStatic, L"", kIdIcon, 10, 10, 32, 32, SS_ICON | SS_REALSIZEIMAGE);
    t.Item(kStatic, L"AirGlass is running", kIdTitle, 50, 12, 200, 14, SS_LEFT);
    t.Item(kStatic, L"Your PC is ready to receive AirPlay.", -1, 50, 28, 200, 10, SS_LEFT);
    t.Item(kStatic, connect.c_str(), -1, 10, 48, 240, 28, SS_LEFT);
    t.Item(kStatic, tray.c_str(), -1, 10, 80, 240, 20, SS_LEFT);
    t.Item(kButton, L"Start AirGlass with Windows", kIdAutostart, 10, 115, 150, 12, BS_AUTOCHECKBOX | WS_TABSTOP);
    t.Item(kButton, L"Got it", IDOK, 200, 113, 50, 15, BS_DEFPUSHBUTTON | WS_TABSTOP);
    WelcomeState st;
    st.icon = icon;
    st.autostart = *startWithWindows;
    INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), owner, WelcomeProc, LPARAM(&st));
    if (rc <= 0) LOGE("welcome: dialog failed (%lld, error %lu)", (long long)rc, GetLastError());
    *startWithWindows = st.autostart;
}

}  // namespace ui
