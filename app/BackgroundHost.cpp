#include "pch.h"
#include "BackgroundHost.h"

#include "lycan/i18n.h"

namespace lycan_app {

namespace {
constexpr UINT kTrayMsg = WM_APP + 0x41;
constexpr UINT kTrayId = 7;
constexpr UINT_PTR kBalloonTimer = 0x4C5B;  // fallback when the shell never reports the balloon as gone
constexpr UINT kBalloonFallbackMs = 12000;
constexpr wchar_t kClassName[] = L"LycanZipBackgroundHost";
enum { kCmdShow = 1, kCmdCancelAll, kCmdExit };

void CopyTrunc(wchar_t* dst, size_t cap, const std::wstring& s) { wcsncpy_s(dst, cap, s.c_str(), _TRUNCATE); }

HICON AppIcon() {
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                         GetSystemMetrics(SM_CYSMICON), LR_SHARED));
}
}  // namespace

bool BackgroundHost::Start(Callbacks cb) {
    if (active_) return true;
    cb_ = std::move(cb);
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);  // fails harmlessly when already registered
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kClassName, L"LycanZip", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
    if (!hwnd_) return false;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);
    active_ = true;
    AddIcon();
    ShutdownBlockReasonCreate(hwnd_, lycan::Tr(L"app.lycanzip_still_has_tasks_running"));
    return true;
}

void BackgroundHost::Stop() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kBalloonTimer);
    if (iconAdded_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        iconAdded_ = false;
    }
    ShutdownBlockReasonDestroy(hwnd_);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    active_ = false;
    balloonActive_ = false;
    onBalloonGone_ = nullptr;
}

void BackgroundHost::AddIcon() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = AppIcon();
    CopyTrunc(nid.szTip, ARRAYSIZE(nid.szTip), tip_.empty() ? std::wstring(L"LycanZip") : tip_);
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        iconAdded_ = true;
    }
}

void BackgroundHost::SetTip(const std::wstring& text) {
    if (text == tip_) return;
    tip_ = text;
    if (!iconAdded_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    CopyTrunc(nid.szTip, ARRAYSIZE(nid.szTip), tip_);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void BackgroundHost::Notify(const std::wstring& title, const std::wstring& text, bool error, std::function<void(bool)> onGone) {
    if (!iconAdded_) {
        if (onGone) onGone(false);
        return;
    }
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_INFO;
    CopyTrunc(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    CopyTrunc(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    nid.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_LARGE_ICON;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
    balloonActive_ = true;
    onBalloonGone_ = std::move(onGone);
    SetTimer(hwnd_, kBalloonTimer, kBalloonFallbackMs, nullptr);
}

void BackgroundHost::ShowMenu() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kCmdShow, lycan::Tr(L"app.show_window"));
    AppendMenuW(menu, MF_STRING, kCmdCancelAll, lycan::Tr(L"app.cancel_all_tasks"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdExit, lycan::Tr(L"app.exit_lycanzip"));
    SetMenuDefaultItem(menu, kCmdShow, FALSE);
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);  // required so the menu closes when the user clicks elsewhere
    const int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
    // callbacks may call Stop() (destroys the window): run them last
    if (cmd == kCmdShow && cb_.restore) cb_.restore();
    else if (cmd == kCmdCancelAll && cb_.cancelAll) cb_.cancelAll();
    else if (cmd == kCmdExit && cb_.exitApp) cb_.exitApp();
}

LRESULT CALLBACK BackgroundHost::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    auto* self = reinterpret_cast<BackgroundHost*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == kTrayMsg) {
        switch (LOWORD(lp)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            if (self->cb_.restore) self->cb_.restore();
            break;
        case WM_CONTEXTMENU:
            self->ShowMenu();
            break;
        case NIN_BALLOONUSERCLICK:
        case NIN_BALLOONTIMEOUT: {
            KillTimer(hwnd, kBalloonTimer);
            self->balloonActive_ = false;
            auto gone = std::move(self->onBalloonGone_);
            self->onBalloonGone_ = nullptr;
            if (gone) gone(LOWORD(lp) == NIN_BALLOONUSERCLICK);
            break;
        }
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == kBalloonTimer) {
        KillTimer(hwnd, kBalloonTimer);
        self->balloonActive_ = false;
        auto gone = std::move(self->onBalloonGone_);
        self->onBalloonGone_ = nullptr;
        if (gone) gone(false);
        return 0;
    }
    if (msg == self->taskbarCreatedMsg_ && msg != 0) {
        self->iconAdded_ = false;  // Explorer restarted: the icon is gone
        self->AddIcon();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace lycan_app
