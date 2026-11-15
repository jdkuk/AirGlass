#include "ui/rename.h"

#include <commctrl.h>

#include "ui/dialog_template.h"

namespace ui {
namespace {

constexpr int kIdName = 401;

struct RenameState {
    HICON icon = nullptr;
    std::wstring name;
};

INT_PTR CALLBACK RenameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<RenameState*>(GetWindowLongPtrW(h, DWLP_USER));
    switch (m) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(h, DWLP_USER, l);
        st = reinterpret_cast<RenameState*>(l);
        SendMessageW(h, WM_SETICON, ICON_BIG, LPARAM(st->icon));
        SendDlgItemMessageW(h, kIdName, EM_LIMITTEXT, 40, 0);
        SendDlgItemMessageW(h, kIdName, EM_SETCUEBANNER, TRUE, LPARAM(L"Living Room"));
        SetDlgItemTextW(h, kIdName, st->name.c_str());
        SendDlgItemMessageW(h, kIdName, EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(h, kIdName));
        SetForegroundWindow(h);
        return FALSE;  // focus set by hand
    case WM_COMMAND:
        if (LOWORD(w) == IDCANCEL) {
            EndDialog(h, 0);
            return TRUE;
        }
        if (LOWORD(w) == IDOK) {
            wchar_t buf[64] = L"";
            GetDlgItemTextW(h, kIdName, buf, 64);
            std::wstring n = buf;
            if (n.find_first_not_of(L" .") == std::wstring::npos) {
                MessageBeep(MB_ICONWARNING);
                SetFocus(GetDlgItem(h, kIdName));
                return TRUE;
            }
            st->name = n;
            EndDialog(h, 1);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

}  // namespace

bool ShowRenameDialog(HWND owner, HICON icon, std::wstring* name) {
    Template t;
    t.Dialog(L"Rename this PC", 230, 92, 5);
    t.Item(kStatic, L"Name shown in Screen Mirroring on your iPhone, iPad and Mac:", -1, 10, 10, 210, 10, SS_LEFT);
    t.Item(kEdit, L"", kIdName, 10, 24, 210, 13, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP);
    t.Item(kStatic, L"Name it after the room, for example Living Room or Office. Devices see the new name "
                    L"straight away.",
           -1, 10, 42, 210, 20, SS_LEFT);
    t.Item(kButton, L"Rename", IDOK, 116, 70, 50, 15, BS_DEFPUSHBUTTON | WS_TABSTOP);
    t.Item(kButton, L"Cancel", IDCANCEL, 170, 70, 50, 15, BS_PUSHBUTTON | WS_TABSTOP);
    RenameState st;
    st.icon = icon;
    st.name = *name;
    INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), owner, RenameProc, LPARAM(&st));
    if (rc < 0) LOGE("rename: dialog failed (error %lu)", GetLastError());
    if (rc != 1) return false;
    *name = st.name;
    return true;
}

}  // namespace ui
