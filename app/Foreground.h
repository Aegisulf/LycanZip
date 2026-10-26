#pragma once
// Brings a window of this program to the front even when Windows would only flash its taskbar button: a program that was started by
// Explorer (a double click with Shift held, "Preview" of the context menu) may be too slow for the foreground rule, because its window
// is created after the archive was read. Sharing the input queue of the foreground window for a moment makes SetForegroundWindow work.
#include <windows.h>

namespace lycan_app {

inline void ForceForeground(HWND h) {
    if (!h) return;
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    const HWND fg = GetForegroundWindow();
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const DWORD me = GetCurrentThreadId();
    const bool attach = fgThread && fgThread != me && AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    if (attach) AttachThreadInput(me, fgThread, FALSE);
}

}  // namespace lycan_app
