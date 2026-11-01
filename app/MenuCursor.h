#pragma once
// A context menu opens under a mouse that does not move: Windows does not ask the new popup window for its cursor, so the cursor keeps
// the shape it had while the popup was being made (busy, a sizing arrow...) until the mouse moves. Moving the cursor to where it is
// makes Windows send a mouse move, and the menu sets the normal arrow. "Opened" comes before the popup window is on the screen, so
// it is done a little later (twice: the window of a big menu takes longer). Thread timers of the UI thread: nothing to keep alive.
#include <windows.h>

namespace lycan_app {

inline void RefreshCursorWhenOpened(winrt::Microsoft::UI::Xaml::Controls::Primitives::FlyoutBase const& fly) {
    fly.Opened([](auto&&, auto&&) {
        for (UINT ms : {60u, 250u})
            SetTimer(nullptr, 0, ms, [](HWND, UINT, UINT_PTR id, DWORD) {
                KillTimer(nullptr, id);
                POINT pt{};
                if (GetCursorPos(&pt)) SetCursorPos(pt.x, pt.y);
            });
    });
}

}  // namespace lycan_app
