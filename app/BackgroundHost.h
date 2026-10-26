#pragma once
// Background mode: the windows are hidden while tasks keep running (in worker processes). A notification-area icon
// exists only while this mode is active; its tooltip shows the progress, a click brings the windows back, and the
// context menu offers show / cancel all / exit. It also blocks logoff and shutdown while tasks run.
// Owns a hidden top-level window (not message-only: ShutdownBlockReasonCreate needs a top-level window).
// UI thread only.
#include <functional>
#include <string>

#include <windows.h>

namespace lycan_app {

class BackgroundHost {
public:
    struct Callbacks {
        std::function<void()> restore;    // bring the windows back (tray click / menu)
        std::function<void()> cancelAll;  // cancel every task (menu)
        std::function<void()> exitApp;    // cancel everything and quit (menu)
    };

    BackgroundHost() = default;
    ~BackgroundHost() { Stop(); }
    BackgroundHost(const BackgroundHost&) = delete;

    bool Active() const { return active_; }
    bool Start(Callbacks cb);  // creates the icon and the shutdown block
    void Stop();               // removes everything

    // tooltip text (the shell truncates it to 127 characters)
    void SetTip(const std::wstring& text);
    // balloon from the tray icon (completion / needs attention); onGone(clicked) runs when it was clicked or timed out
    void Notify(const std::wstring& title, const std::wstring& text, bool error, std::function<void(bool clicked)> onGone);
    bool BalloonActive() const { return balloonActive_; }

private:
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    void AddIcon();
    void ModifyIcon(UINT flags, const std::wstring& title, const std::wstring& text, bool error);
    void ShowMenu();

    HWND hwnd_ = nullptr;
    bool active_ = false;
    bool iconAdded_ = false;
    bool balloonActive_ = false;
    UINT taskbarCreatedMsg_ = 0;
    std::wstring tip_;
    Callbacks cb_;
    std::function<void(bool)> onBalloonGone_;
};

}  // namespace lycan_app
