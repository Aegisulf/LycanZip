#pragma once
// Taskbar progress (ITaskbarList3), taskbar button flashing, system notifications (Shell_NotifyIcon balloon).
// A balloon needs no system registration (no registry, no shortcut); the notification icon exists only while it is shown.
// Use on the UI thread only.
#include <string>

#include <windows.h>
#include <shobjidl_core.h>

namespace lycan_app {

enum class ProgressState { None, Indeterminate, Normal, Paused, Error };

class ShellNotifier {
public:
    ShellNotifier() = default;
    ~ShellNotifier() { Detach(); }
    ShellNotifier(const ShellNotifier&) = delete;

    ProgressState State() const { return state_; }
    bool BalloonActive() const { return balloonIcon_; }  // a notification is being shown (the compact window must wait for it before the process may end)
    void Attach(HWND hwnd);  // call Detach before the window is closed later
    void Detach();

    // done / total are used only for Normal / Paused / Error (Error always shows a full red bar).
    // Error stays until the user activates the window (has seen it).
    void SetProgress(ProgressState state, unsigned long long done = 0, unsigned long long total = 0);

    // When the window is not in the foreground, flash the taskbar button until the user switches back.
    void FlashUntilForeground();

    // Balloon notification; error = true uses the error icon. Clicking it brings the window to the front.
    void Notify(const std::wstring& title, const std::wstring& text, bool error);

    // Any window of this process (main window, preview window) is in the foreground and the main window is not minimized
    bool AppIsForeground() const;

private:
    static LRESULT CALLBACK SubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void ApplyProgress();
    void RemoveBalloonIcon();
    void BringToFront();

    HWND hwnd_ = nullptr;
    winrt::com_ptr<ITaskbarList3> taskbar_;
    ProgressState state_ = ProgressState::None;
    unsigned long long done_ = 0, total_ = 0;
    bool balloonIcon_ = false;
    UINT taskbarCreatedMsg_ = 0;
    long lastHr_ = 0;   // the last failure of the taskbar calls (logged when it changes)
};

}  // namespace lycan_app
