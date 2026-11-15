// Allow-list prompt: "Allow <device> to use AirGlass?" when a device that isn't known yet starts mirroring or audio.
#pragma once

#include "common.h"

namespace ui {

enum AllowAnswer { kAllowDeny = 0, kAllowOnce = 1, kAllowAlways = 2, kAllowBlock = 3 };

// Modal. Closing the dialog (Esc, ✕) turns the device away this time. The dialog closes by itself with
// kAllowDeny at `deadline` (NowSeconds) or as soon as `settled` returns true (the sender gave up, or the
// request was answered elsewhere).
AllowAnswer ShowAllowDialog(HWND owner, HICON icon, const std::wstring& device, double deadline,
                            const std::function<bool()>& settled);

}  // namespace ui
