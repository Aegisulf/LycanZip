#pragma once
// Alt+<letter> shortcuts (Alt+P, Alt+Shift+P, Alt+Enter ...) are handled as keyboard accelerators, but Windows still
// delivers the matching WM_SYSCHAR and the default handler answers it with a beep ("no such menu mnemonic"). None of our
// windows has a menu bar, so the character is dropped.
//
// A window procedure is the wrong place: the keyboard focus is in a child window that WinUI creates inside the top-level
// window, and that child gets the message. A GetMessage hook on the UI thread sees every message of every window of the
// thread before it is dispatched, so one hook covers the main window and the preview windows. Alt+Space (the system menu)
// is left alone.
#include <windows.h>

namespace lycan_app {

inline LRESULT CALLBACK AltBeepGetMsg(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && wp == PM_REMOVE) {
        auto* m = reinterpret_cast<MSG*>(lp);
        if (m->message == WM_SYSCHAR && m->wParam != L' ') m->message = WM_NULL;
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

// Call on the UI thread (once is enough; later calls do nothing). The hwnd is not needed any more, it stays so that the
// windows keep saying where they want the beep gone.
inline void SuppressAltBeep(HWND) {
    static const HHOOK hook = SetWindowsHookExW(WH_GETMESSAGE, AltBeepGetMsg, nullptr, GetCurrentThreadId());
    (void)hook;
}

}  // namespace lycan_app
