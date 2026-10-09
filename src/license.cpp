#include "license.h"

#include "ui/dialog_template.h"

#include <commctrl.h>
#include <shellapi.h>
#include <winhttp.h>

namespace license {
namespace {

constexpr int kIdReason = 201, kIdKey = 202, kIdStatus = 203, kIdBuy = 204;

std::string UrlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// POSTs a form to api.lemonsqueezy.com and returns the response body ("" on a network failure).
std::string Post(const wchar_t* path, const std::string& form, DWORD* status) {
    std::string body;
    *status = 0;
    HINTERNET s = WinHttpOpen(L"AirGlass/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 5000, 5000, 10000, 10000);
    HINTERNET c = WinHttpConnect(s, L"api.lemonsqueezy.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         WINHTTP_FLAG_SECURE)
                    : nullptr;
    const wchar_t* headers = L"Accept: application/json\r\nContent-Type: application/x-www-form-urlencoded\r\n";
    if (r && WinHttpSendRequest(r, headers, DWORD(-1), (void*)form.data(), DWORD(form.size()), DWORD(form.size()), 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD size = sizeof(*status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            status, &size, WINHTTP_NO_HEADER_INDEX);
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got) || got == 0) break;
            body.append(chunk.data(), got);
            if (body.size() > (1 << 20)) break;
        }
    } else {
        LOGW("license: request to Lemon Squeezy failed (error %lu)", GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return body;
}

// The scalar after "key": at or after `from` in a JSON text: a string's contents, or the literal
// (true / false / null / number). Enough for Lemon Squeezy's fixed response shape.
std::string Field(const std::string& j, size_t from, const char* key) {
    if (from == std::string::npos) return "";
    std::string pat = std::string("\"") + key + "\"";
    size_t p = j.find(pat, from);
    if (p == std::string::npos) return "";
    p += pat.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == ':' || j[p] == '\n' || j[p] == '\r' || j[p] == '\t')) ++p;
    if (p >= j.size()) return "";
    if (j[p] == '"') {
        std::string out;
        for (++p; p < j.size() && j[p] != '"'; ++p) {
            if (j[p] == '\\' && p + 1 < j.size()) ++p;
            out += j[p];
        }
        return out;
    }
    size_t e = j.find_first_of(",}] \r\n", p);
    return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// Checks the store/product in "meta" so keys sold for other products are refused.
bool ForThisProduct(const std::string& j, std::string* error) {
    size_t meta = j.find("\"meta\"");
    if (kStoreId && Field(j, meta, "store_id") != std::to_string(kStoreId)) {
        *error = "This key is for a different product.";
        return false;
    }
    if (kProductId && Field(j, meta, "product_id") != std::to_string(kProductId)) {
        *error = "This key is for a different product.";
        return false;
    }
    return true;
}

Result Finish(const std::string& j, DWORD status, const char* validField) {
    Result res;
    if (j.empty() || j.find('{') == std::string::npos) {
        res.error = "Couldn't reach the license server. Check the internet connection and try again.";
        return res;
    }
    res.ok = true;
    std::string err = Field(j, 0, "error");
    if (Field(j, 0, validField) == "true" && ForThisProduct(j, &res.error)) {
        res.valid = true;
    } else if (res.error.empty()) {
        res.error = !err.empty() && err != "null" ? err : "This license key isn't valid (HTTP " + std::to_string(status) + ").";
    }
    return res;
}

struct DialogState {
    std::wstring reason;
    std::string key, instanceId;
};

std::string Trim(std::string s) {
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string ComputerName() {
    wchar_t buf[256];
    DWORD n = 256;
    return GetComputerNameExW(ComputerNameDnsHostname, buf, &n) ? WideToUtf8(std::wstring(buf, n)) : "Windows PC";
}

INT_PTR CALLBACK DialogProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = reinterpret_cast<DialogState*>(GetWindowLongPtrW(h, DWLP_USER));
    switch (m) {
    case WM_INITDIALOG:
        SetWindowLongPtrW(h, DWLP_USER, l);
        st = reinterpret_cast<DialogState*>(l);
        SetDlgItemTextW(h, kIdReason, st->reason.c_str());
        SendDlgItemMessageW(h, kIdKey, EM_SETCUEBANNER, TRUE, LPARAM(L"XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"));
        SetForegroundWindow(h);
        SetFocus(GetDlgItem(h, kIdKey));
        return FALSE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case kIdBuy: OpenBuyPage(); return TRUE;
        case IDCANCEL: EndDialog(h, 0); return TRUE;
        case IDOK: {
            wchar_t buf[256] = L"";
            GetDlgItemTextW(h, kIdKey, buf, 256);
            std::string key = Trim(WideToUtf8(buf));
            if (key.empty()) {
                SetDlgItemTextW(h, kIdStatus, L"Paste the key from your receipt email first.");
                return TRUE;
            }
            SetDlgItemTextW(h, kIdStatus, L"Activating…");
            EnableWindow(GetDlgItem(h, IDOK), FALSE);
            UpdateWindow(h);
            HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
            Result r = Activate(key, ComputerName());
            SetCursor(old);
            EnableWindow(GetDlgItem(h, IDOK), TRUE);
            if (r.valid) {
                st->key = key;
                st->instanceId = r.instanceId;
                MessageBoxW(h, L"Thank you! AirGlass Pro is unlocked: the mirroring window can now be moved, resized, "
                               L"zoomed and kept on top.", L"AirGlass Pro", MB_ICONINFORMATION);
                EndDialog(h, 1);
            } else {
                SetDlgItemTextW(h, kIdStatus, Utf8ToWide(r.error).c_str());
            }
            return TRUE;
        }
        }
        break;
    }
    return FALSE;
}

}  // namespace

Result Activate(const std::string& key, const std::string& instanceName) {
    DWORD status = 0;
    std::string j = Post(L"/v1/licenses/activate",
                         "license_key=" + UrlEncode(key) + "&instance_name=" + UrlEncode("AirGlass on " + instanceName),
                         &status);
    Result r = Finish(j, status, "activated");
    if (r.valid) r.instanceId = Field(j, j.find("\"instance\""), "id");
    LOGI("license: activate -> %s%s", r.valid ? "ok" : "refused: ", r.valid ? "" : r.error.c_str());
    return r;
}

Result Validate(const std::string& key, const std::string& instanceId) {
    DWORD status = 0;
    std::string j = Post(L"/v1/licenses/validate", "license_key=" + UrlEncode(key) + "&instance_id=" + UrlEncode(instanceId),
                         &status);
    Result r = Finish(j, status, "valid");
    LOGI("license: validate -> %s%s", !r.ok ? "offline (kept)" : r.valid ? "ok" : "invalid: ",
         r.ok && !r.valid ? r.error.c_str() : "");
    return r;
}

void OpenBuyPage() { ShellExecuteW(nullptr, L"open", kBuyUrl, nullptr, nullptr, SW_SHOWNORMAL); }

bool ShowUpgradeDialog(HWND owner, const std::wstring& reason, std::string* key, std::string* instanceId) {
    std::wstring buy = std::wstring(L"Buy AirGlass Pro (") + kPrice + L")…";
    std::wstring pitch = std::wstring(L"AirGlass Pro unlocks the mirroring window: move it, resize it, zoom, keep it "
                                      L"on top or float it in a corner. One-time ") + kPrice + L", no subscription.";
    ui::Template t;
    t.Dialog(L"AirGlass Pro", 260, 134, 8);
    t.Item(ui::kStatic, L"", kIdReason, 10, 8, 240, 18, SS_LEFT);
    t.Item(ui::kStatic, pitch.c_str(), -1, 10, 28, 240, 26, SS_LEFT);
    t.Item(ui::kButton, buy.c_str(), kIdBuy, 10, 58, 130, 15, BS_PUSHBUTTON | WS_TABSTOP);
    t.Item(ui::kStatic, L"Already bought it? Paste the license key from your receipt email:", -1, 10, 82, 240, 10, SS_LEFT);
    t.Item(ui::kEdit, L"", kIdKey, 10, 94, 240, 13, ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP);
    t.Item(ui::kStatic, L"", kIdStatus, 10, 113, 136, 18, SS_LEFT);
    t.Item(ui::kButton, L"Activate", IDOK, 150, 113, 50, 15, BS_DEFPUSHBUTTON | WS_TABSTOP);
    t.Item(ui::kButton, L"Not now", IDCANCEL, 204, 113, 46, 15, BS_PUSHBUTTON | WS_TABSTOP);
    DialogState st;
    st.reason = reason;
    INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), t.Get(), owner, DialogProc, LPARAM(&st));
    if (rc != 1) return false;
    *key = st.key;
    *instanceId = st.instanceId;
    return true;
}

}  // namespace license
