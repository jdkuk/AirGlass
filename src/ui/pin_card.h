// "AirPlay code" card: the code a connecting iPhone, iPad or Mac asks for, large and on top of everything, on the
// screen mirroring would use. It never takes the focus (the user is typing on the device); a click hides it.
#pragma once

#include "common.h"

namespace ui {

class PinCard {
public:
    PinCard() = default;
    PinCard(const PinCard&) = delete;
    PinCard& operator=(const PinCard&) = delete;
    ~PinCard();
    // Shows `code` (or replaces the one showing) on `mon`, and hides it by itself after `seconds`.
    void Show(const std::wstring& code, const std::wstring& receiver, HMONITOR mon, double seconds);
    void Hide();

private:
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    void Paint(HDC dc);

    HWND hwnd_ = nullptr;
    std::wstring code_, receiver_;
    float dp_ = 1.0f;
    double deadline_ = 0;
};

}  // namespace ui
