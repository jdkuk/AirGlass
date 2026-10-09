// AirGlass Pro: free builds mirror full screen only; a Lemon Squeezy license key unlocks the
// window (move, resize, zoom, keep on top, corner float).
//
// The key is activated once online (Lemon Squeezy License API, no account or server of ours) and
// stored in config.ini; startup re-validates in the background and only a definite "invalid"
// answer (refund, disabled key) locks it again, so Pro keeps working offline.
#pragma once

#include "common.h"

namespace license {

// Store settings, filled in once the Lemon Squeezy product exists (package.ps1 refuses to build a
// release while kStoreId is 0, because then a key from any Lemon Squeezy store would be accepted).
constexpr long long kStoreId = 492472;
constexpr long long kProductId = 1425951;
constexpr const wchar_t* kBuyUrl = L"https://airglasspc.lemonsqueezy.com/checkout/buy/0e38c6a7-1639-4409-ad59-49f0d62c9bfb";
constexpr const wchar_t* kPrice = L"$2.99";
// Refund policy (Steam-style): within kRefundDays of unlocking and fewer than kRefundSessions
// mirroring sessions with Pro. AirGlass only counts and shows this; refunds are granted by hand.
constexpr int kRefundDays = 14;
constexpr int kRefundSessions = 10;

struct Result {
    bool ok = false;           // the request reached Lemon Squeezy and was understood
    bool valid = false;        // the key is good for this product
    std::string instanceId;    // from activate
    std::string error;         // human-readable reason when !valid
};

// Blocking HTTPS calls (up to ~15 s); run them off the UI thread or behind a wait cursor.
Result Activate(const std::string& key, const std::string& instanceName);
Result Validate(const std::string& key, const std::string& instanceId);

// Opens the purchase page.
void OpenBuyPage();

// Modal "AirGlass Pro" dialog: buy link + key entry. Calls Activate itself and returns the
// activated key/instance through the out-params; returns true when Pro was just unlocked.
// `reason` is a short line shown at the top (e.g. why the dialog appeared).
bool ShowUpgradeDialog(HWND owner, const std::wstring& reason, std::string* key, std::string* instanceId);

}  // namespace license
