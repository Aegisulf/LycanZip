#pragma once
// Shared by the source files of the main window (MainWindow.xaml.cpp and MainWindow.<part>.cpp, one per feature): the
// includes, the small helpers every part uses, and the request types of the worker thread.
#include "MainWindow.xaml.h"

#include "EntryRow.h"
#include "TaskRow.h"
#include "ImagePreview.h"
#include "AudioPreview.h"
#include "OpenTempDir.h"
#include "lycan/tempdir.h"
#include "ShellNotify.h"
#include "I18nXaml.h"
#include "lycan/languages.h"
#include <shlobj.h>
#include "AltBeep.h"
#include "DragOut.h"
#include "MenuCursor.h"
#include "PasswordVault.h"
#include "Pickers.h"
#include "ScanText.h"
#include "lycan/junk.h"
#include "lycan/fileicons.h"
#include "Win11Menu.h"
#include "FileIconImages.h"
#include "ShellIcons.h"
#include "lycan/shellitems.h"
#include "SettingsHub.h"
#include "HighContrast.h"
#include "ContrastTheme.h"
#include "Welcome.h"

#include <algorithm>
#include <tlhelp32.h>
#include <chrono>
#include <ctime>
#include <cmath>
#include <coroutine>
#include <fstream>
#include <limits>
#include <atomic>
#include <optional>
#include <set>
#include <thread>

#include <commctrl.h>
#include <mmsystem.h>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

#include "lycan/checksum.h"
#include "lycan/compress.h"
#include "lycan/datalocation.h"
#include "lycan/diag.h"
#include "lycan/encoding.h"
#include "lycan/error.h"
#include "lycan/i18n.h"
#include "lycan/security.h"
#include "lycan/version.h"
#include "lycan/virustotal.h"
#ifdef LYCAN_VELOPACK
#include "installed_update.h"
#endif

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

namespace
{
    // An awaiter that continues on the UI thread (does not depend on WIL)
    struct resume_ui
    {
        winrt::Microsoft::UI::Dispatching::DispatcherQueue q;
        bool await_ready() const { return q.HasThreadAccess(); }
        void await_suspend(std::coroutine_handle<> h) const { q.TryEnqueue([h] { h(); }); }
        void await_resume() const noexcept {}
    };

    inline std::wstring FromUtf8(std::string const& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    inline std::wstring FormatSize(uint64_t b)
    {
        wchar_t buf[64];
        const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
        double v = static_cast<double>(b);
        int u = 0;
        while (v >= 1024 && u < 4) { v /= 1024; ++u; }
        if (u == 0) swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(b));
        else swprintf_s(buf, L"%.1f %s", v, units[u]);
        return buf;
    }

    inline std::wstring FormatTime(uint64_t fileTime)
    {
        if (!fileTime) return lycan::Tr(L"main.unknown");
        FILETIME ft{static_cast<DWORD>(fileTime & 0xFFFFFFFF), static_cast<DWORD>(fileTime >> 32)}, local;
        SYSTEMTIME st;
        if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return lycan::Tr(L"main.unknown");
        wchar_t buf[64];
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        return buf;
    }

    inline bool IsArchivePath(fs::path const& p)
    {
        static const wchar_t* exts[] = {L".zip", L".7z", L".rar", L".tar", L".gz", L".tgz", L".xz", L".txz", L".bz2", L".tbz2",
                                        L".tbz", L".iso", L".cab", L".arj", L".lzh", L".lha", L".z", L".wim", L".zst", L".001", L".jar", L".cbz", L".cbr", L".cb7", L".cbt"};
        std::wstring e = p.extension().wstring();
        std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return false;
        if (std::any_of(std::begin(exts), std::end(exts), [&](const wchar_t* x) { return e == x; })) return true;
        // a later volume of a split archive (.part2.rar, .r00, .z01, .002 ...) counts when the first one is next to it
        const lycan::VolumeInfo v = lycan::DetectVolume(p);
        return v.IsLater() && fs::exists(v.first, ec);
    }

    // .001 .002 ... (split volumes): not handled when the original is deleted, so that only one of the volumes is not deleted
    inline bool IsVolumePath(fs::path const& p)
    {
        std::wstring e = p.extension().wstring();
        return e.size() == 4 && iswdigit(e[1]) && iswdigit(e[2]) && iswdigit(e[3]);
    }

    inline fs::path UniquePath(fs::path p)
    {
        std::error_code ec;
        if (!fs::exists(p, ec)) return p;
        std::wstring stem = p.stem().wstring(), ext = p.extension().wstring();
        // double extensions like foo.tar.gz: ".tar" counts as part of the extension
        if (fs::path(stem).extension() == L".tar") { ext = L".tar" + ext; stem = fs::path(stem).stem().wstring(); }
        for (int i = 2; i < 1000; ++i)
        {
            fs::path c = p.parent_path() / (stem + L" (" + std::to_wstring(i) + L")" + ext);
            if (!fs::exists(c, ec)) return c;
        }
        return p;
    }

    inline bool IsExePath(fs::path const& p)
    {
        std::wstring e = p.extension().wstring();
        std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
        std::error_code ec;
        return e == L".exe" && fs::is_regular_file(p, ec);
    }

    // Deletes with the shell's own file operation (IFileOperation): Windows shows its progress and its own prompts
    // (file in use: retry / skip, permissions: elevation) and reports every item. recycle = move to the Recycle Bin
    // (undo-able); otherwise the items are deleted permanently. Returns the number of items that could not be deleted.
    struct DeleteSink : winrt::implements<DeleteSink, IFileOperationProgressSink>
    {
        int failed = 0, deleted = 0;
        HRESULT __stdcall StartOperations() noexcept override { return S_OK; }
        HRESULT __stdcall FinishOperations(HRESULT) noexcept override { return S_OK; }
        HRESULT __stdcall PreRenameItem(DWORD, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
        HRESULT __stdcall PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT, IShellItem*) noexcept override { return S_OK; }
        HRESULT __stdcall PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
        HRESULT __stdcall PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) noexcept override { return S_OK; }
        HRESULT __stdcall PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
        HRESULT __stdcall PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) noexcept override { return S_OK; }
        HRESULT __stdcall PreDeleteItem(DWORD, IShellItem*) noexcept override { return S_OK; }
        HRESULT __stdcall PostDeleteItem(DWORD, IShellItem*, HRESULT hr, IShellItem*) noexcept override
        {
            if (SUCCEEDED(hr)) ++deleted; else ++failed;
            return S_OK;
        }
        HRESULT __stdcall PreNewItem(DWORD, IShellItem*, LPCWSTR) noexcept override { return S_OK; }
        HRESULT __stdcall PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) noexcept override { return S_OK; }
        HRESULT __stdcall UpdateProgress(UINT, UINT) noexcept override { return S_OK; }
        HRESULT __stdcall ResetTimer() noexcept override { return S_OK; }
        HRESULT __stdcall PauseTimer() noexcept override { return S_OK; }
        HRESULT __stdcall ResumeTimer() noexcept override { return S_OK; }
    };

    inline int ShellDelete(HWND owner, std::vector<fs::path> const& items, bool recycle, int& deleted)
    {
        deleted = 0;
        winrt::com_ptr<IFileOperation> op;
        if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(op.put())))) return static_cast<int>(items.size());
        DWORD flags = recycle ? (FOF_ALLOWUNDO | FOF_WANTNUKEWARNING | FOFX_ADDUNDORECORD | FOFX_RECYCLEONDELETE) : FOF_NOCONFIRMATION;
        op->SetOperationFlags(flags | FOFX_SHOWELEVATIONPROMPT);
        if (owner) op->SetOwnerWindow(owner);
        auto sink = winrt::make_self<DeleteSink>();
        DWORD cookie = 0;
        op->Advise(sink.get(), &cookie);
        int queued = 0;
        for (auto const& p : items)
        {
            winrt::com_ptr<IShellItem> si;
            if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(si.put()))) && SUCCEEDED(op->DeleteItem(si.get(), nullptr))) ++queued;
        }
        if (queued) op->PerformOperations();
        op->Unadvise(cookie);
        deleted = sink->deleted;
        return static_cast<int>(items.size()) - sink->deleted;
    }

    // can the Recycle Bin take items on this path? (fixed local drives only; network and removable drives cannot)
    inline bool CanRecycle(fs::path const& p)
    {
        wchar_t root[MAX_PATH] = {};
        if (!GetVolumePathNameW(p.c_str(), root, MAX_PATH)) return false;
        return GetDriveTypeW(root) == DRIVE_FIXED;
    }

    inline std::wstring FileVersionOf(fs::path const& file)
    {
        DWORD h = 0, size = GetFileVersionInfoSizeW(file.c_str(), &h);
        if (!size) return L"";
        std::vector<char> buf(size);
        if (!GetFileVersionInfoW(file.c_str(), 0, size, buf.data())) return L"";
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) || !fi) return L"";
        wchar_t out[64];
        swprintf_s(out, L"%u.%02u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS));
        return out;
    }

    constexpr uint64_t kExecFolderWarnBytes = 1ull << 30;  // an executable's folder bigger than this is confirmed before extracting it

    // Order of the drop-down lists of the settings page (the same as in XAML)
    constexpr lycan::ArchiveType kFormatOrder[] = {lycan::ArchiveType::Zip,    lycan::ArchiveType::SevenZip, lycan::ArchiveType::TarGz,
                                                   lycan::ArchiveType::TarXz, lycan::ArchiveType::TarBz2,   lycan::ArchiveType::Tar,
                                                   lycan::ArchiveType::Iso};
    constexpr const wchar_t* kFormatExt[] = {L".zip", L".7z", L".tar.gz", L".tar.xz", L".tar.bz2", L".tar", L".iso"};
    constexpr lycan::Profile kProfileOrder[] = {lycan::Profile::Balanced, lycan::Profile::Compatible, lycan::Profile::Smallest, lycan::Profile::Fastest};

    template <typename T, size_t N>
    inline int IndexOf(const T (&arr)[N], T v)
    {
        for (size_t i = 0; i < N; ++i) if (arr[i] == v) return static_cast<int>(i);
        return 0;
    }
}

namespace winrt::LycanZip::implementation
{
    // What a menu item does, for the colour of its icon: delete = red, malware / VirusTotal = amber, test / verify = green, add / change =
    // the accent colour of the theme, everything else = the colour of the text. The colours are those of the Windows system brushes
    // SystemFillColorCritical / Caution / Success (the XAML menus use the brushes themselves).
    enum class IconTone { None, Danger, Security, Verify, Accent };

    // Small building blocks of dialogs and menus (defined in MainWindow.xaml.cpp)
    Windows::UI::Color AccentColorNow();
    FontIcon MenuIcon(const wchar_t* glyph, IconTone tone = IconTone::None, bool dark = true);
    FontIcon MakeGlyph(const wchar_t* glyph, double size, Microsoft::UI::Xaml::Media::Brush const& brush = nullptr);
    Border MakeCardBox(UIElement const& child, double padH = 12, double padV = 10);
    TextBlock Caption(std::wstring const& text, double opacity = 0.7);
    StackPanel TitledOption(const wchar_t* title, const wchar_t* desc);
    fire_and_forget ScanSources(std::vector<fs::path> inputs, TextBlock target, Microsoft::UI::Dispatching::DispatcherQueue dq);
    uint64_t TagId(Windows::Foundation::IInspectable const& sender);   // (MainWindow.Extract.cpp)

    // Conflict question: the worker thread makes the request and waits, the UI thread shows the dialog and fills in the answer
    struct MainWindow::ConflictRequest
    {
        lycan::ConflictInfo info;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        lycan::ConflictAnswer answer{lycan::ConflictAction::Cancel, false};
        std::shared_ptr<std::atomic<int>> rule;
        void Finish(lycan::ConflictAnswer a)
        {
            { std::lock_guard lk(m); answer = a; done = true; }
            cv.notify_all();
        }
    };

    // A question of a task about an archive that would unpack to far more than it is
    struct MainWindow::LargeRequest
    {
        lycan::LargeExtractInfo info;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        bool answer = false;
        void Finish(bool a)
        {
            { std::lock_guard lk(m); answer = a; done = true; }
            cv.notify_all();
        }
    };
}
