// "Rename this PC…": the receiver name iPhones, iPads and Macs show in Screen Mirroring.
#pragma once

#include "common.h"

namespace ui {

// Modal. `name` is the current name on entry and the user's new name on return; false when cancelled.
bool ShowRenameDialog(HWND owner, HICON icon, std::wstring* name);

}  // namespace ui
