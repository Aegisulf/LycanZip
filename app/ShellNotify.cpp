#include "pch.h"
#include "ShellNotify.h"
#include "lycan/diag.h"

#include <commctrl.h>
#include <algorithm>

namespace lycan_app {

namespace {
constexpr UINT kTrayMsg = WM_APP + 0x31;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kBalloonTimer = 0x4C5A;  // safety timer for when the balloon gets no timeout event (Focus Assist etc.)
constexpr UINT kBalloonFallbackMs = 12000;
constexpr UINT_PTR kSubclassId = 2;

void CopyTrunc(wchar_t* dst, size_t cap, const std::wstring& s) {
    wcsncpy_s(dst, cap, s.c_str(), _TRUNCATE);
}
}  // namespace

void ShellNotifier::Attach(HWND hwnd) {
    hwnd_ = hwnd;
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarButtonCreated");
    // also received by low-privilege processes / when the taskbar is rebuilt
    ChangeWindowMessageFilterEx(hwnd_, taskbarCreatedMsg_, MSGFLT_ALLOW, nullptr);
    SetWindowSubclass(hwnd_, SubclassProc, kSubclassId, reinterpret_cast<DWORD_PTR>(this));
    if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITaskbarList3), taskbar_.put_void()))) {
        if (FAILED(taskbar_->HrInit())) taskbar_ = nullptr;
    }
}

void ShellNotifier::Detach() {
    if (!hwnd_) return;
    RemoveBalloonIcon();
    if (taskbar_) taskbar_->SetProgressState(hwnd_, TBPF_NOPROGRESS);
    taskbar_ = nullptr;
    RemoveWindowSubclass(hwnd_, SubclassProc, kSubclassId);
    hwnd_ = nullptr;
}

void ShellNotifier::SetProgress(ProgressState state, unsigned long long done, unsigned long long total) {
    state_ = state;
    done_ = done;
    total_ = total;
    ApplyProgress();
}

void ShellNotifier::ApplyProgress() {
    if (!taskbar_ || !hwnd_) { if (lastHr_ != 1) { lastHr_ = 1; lycan::diag::Info(L"taskbar progress: no taskbar object or window yet"); } return; }
    HRESULT hr = S_OK;
    switch (state_) {
    case ProgressState::None:
        hr = taskbar_->SetProgressState(hwnd_, TBPF_NOPROGRESS);
        break;
    case ProgressState::Indeterminate:
        hr = taskbar_->SetProgressState(hwnd_, TBPF_INDETERMINATE);
        break;
    case ProgressState::Normal:
    case ProgressState::Paused:
        hr = taskbar_->SetProgressState(hwnd_, state_ == ProgressState::Paused ? TBPF_PAUSED : TBPF_NORMAL);
        if (SUCCEEDED(hr)) hr = taskbar_->SetProgressValue(hwnd_, std::min(done_, total_), std::max<unsigned long long>(total_, 1));
        break;
    case ProgressState::Error:
        hr = taskbar_->SetProgressState(hwnd_, TBPF_ERROR);
        if (SUCCEEDED(hr)) hr = taskbar_->SetProgressValue(hwnd_, 1, 1);
        break;
    }
    if (static_cast<long>(hr) != lastHr_ && (FAILED(hr) || lastHr_ != 0)) lycan::diag::Info(L"taskbar progress: the call returned {:#x}", static_cast<unsigned long>(hr));
    lastHr_ = FAILED(hr) ? static_cast<long>(hr) : 0;
}

bool ShellNotifier::AppIsForeground() const {
    if (!hwnd_ || IsIconic(hwnd_)) return false;
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

void ShellNotifier::FlashUntilForeground() {
    if (!hwnd_ || AppIsForeground()) return;
    FLASHWINFO fi{sizeof(fi), hwnd_, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fi);
}

void ShellNotifier::Notify(const std::wstring& title, const std::wstring& text, bool error) {
    if (!hwnd_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP | NIF_INFO;
    nid.uCallbackMessage = kTrayMsg;
    nid.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON,
                                              GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    CopyTrunc(nid.szTip, ARRAYSIZE(nid.szTip), L"LycanZip");
    CopyTrunc(nid.szInfoTitle, ARRAYSIZE(nid.szInfoTitle), title);
    CopyTrunc(nid.szInfo, ARRAYSIZE(nid.szInfo), text);
    nid.dwInfoFlags = (error ? NIIF_ERROR : NIIF_INFO) | NIIF_LARGE_ICON;
    nid.uVersion = NOTIFYICON_VERSION_4;
    if (!balloonIcon_) {
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) return;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        balloonIcon_ = true;
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    SetTimer(hwnd_, kBalloonTimer, kBalloonFallbackMs, nullptr);
}

void ShellNotifier::RemoveBalloonIcon() {
    if (!hwnd_) return;
    KillTimer(hwnd_, kBalloonTimer);
    if (!balloonIcon_) return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd_;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    balloonIcon_ = false;
}

void ShellNotifier::BringToFront() {
    if (!hwnd_) return;
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

LRESULT CALLBACK ShellNotifier::SubclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<ShellNotifier*>(ref);
    if (msg == kTrayMsg) {
        switch (LOWORD(lp)) {
        case NIN_BALLOONUSERCLICK:
            self->BringToFront();
            self->RemoveBalloonIcon();
            break;
        case NIN_BALLOONTIMEOUT:
            self->RemoveBalloonIcon();
            break;
        }
        return 0;
    }
    if (msg == WM_TIMER && wp == kBalloonTimer) {
        self->RemoveBalloonIcon();
        return 0;
    }
    if (msg == self->taskbarCreatedMsg_ && msg != 0) {
        // the taskbar (re)created the button: apply the current progress state again
        self->taskbar_ = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITaskbarList3), self->taskbar_.put_void()))) {
            if (FAILED(self->taskbar_->HrInit())) self->taskbar_ = nullptr;
        }
        self->ApplyProgress();
    } else if (msg == WM_ACTIVATE && LOWORD(wp) != WA_INACTIVE) {
        // the user came back to the window = has seen it: clear the red error state
        if (self->state_ == ProgressState::Error) self->SetProgress(ProgressState::None);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

}  // namespace lycan_app
