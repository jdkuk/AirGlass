// First-run welcome window: says AirGlass is running, how to connect, and where the tray icon lives.
#pragma once

#include "common.h"

namespace ui {

// Modal. `startWithWindows` is the checkbox's initial state on entry and the user's choice on return.
void ShowWelcome(HWND owner, HICON icon, const std::wstring& receiverName, bool* startWithWindows);

}  // namespace ui
