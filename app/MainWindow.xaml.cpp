#include "pch.h"
#include "MainWindow.xaml.h"
#if __has_include("MainWindow.g.cpp")
#include "MainWindow.g.cpp"
#endif
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

    std::wstring FromUtf8(std::string const& s)
    {
        if (s.empty()) return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    }

    std::wstring FormatSize(uint64_t b)
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

    std::wstring FormatTime(uint64_t fileTime)
    {
        if (!fileTime) return lycan::Tr(L"main.unknown");
        FILETIME ft{static_cast<DWORD>(fileTime & 0xFFFFFFFF), static_cast<DWORD>(fileTime >> 32)}, local;
        SYSTEMTIME st;
        if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return lycan::Tr(L"main.unknown");
        wchar_t buf[64];
        swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
        return buf;
    }

    bool IsArchivePath(fs::path const& p)
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
    bool IsVolumePath(fs::path const& p)
    {
        std::wstring e = p.extension().wstring();
        return e.size() == 4 && iswdigit(e[1]) && iswdigit(e[2]) && iswdigit(e[3]);
    }

    fs::path UniquePath(fs::path p)
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

    bool IsExePath(fs::path const& p)
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

    int ShellDelete(HWND owner, std::vector<fs::path> const& items, bool recycle, int& deleted)
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
    bool CanRecycle(fs::path const& p)
    {
        wchar_t root[MAX_PATH] = {};
        if (!GetVolumePathNameW(p.c_str(), root, MAX_PATH)) return false;
        return GetDriveTypeW(root) == DRIVE_FIXED;
    }

    std::wstring FileVersionOf(fs::path const& file)
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
    int IndexOf(const T (&arr)[N], T v)
    {
        for (size_t i = 0; i < N; ++i) if (arr[i] == v) return static_cast<int>(i);
        return 0;
    }
}

namespace winrt::LycanZip::implementation
{
    static LRESULT CALLBACK MinSizeProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

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

    MainWindow::MainWindow()
    {
        lycan::diag::Info(L"window: created");
        InitializeComponent();
        lycan_app::TranslateTree(Content());
        dispatcher_ = DispatcherQueue();
        taskRows_ = single_threaded_observable_vector<Windows::Foundation::IInspectable>();  // must be an "observable" collection, so that added / removed rows are shown
        TaskList().ItemsSource(taskRows_);

        LoadBrandImages();
        ExtendsContentIntoTitleBar(true);
        // No SetTitleBar: the whole strip would be the caption, and the settings button inside it sometimes ignored a click
        // (a second quick click counted as a double-click on the caption and maximized the window). Only the empty parts
        // of the strip are drag rectangles; the button is plain client area.
        AppTitleBar().SizeChanged([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->UpdateTitleDragRects(); });
        ResizeToDefault();
        UpdateTitleDragRects();

        // File name encoding menu
        struct Enc { const wchar_t* label; unsigned cp; };
        for (auto const& e : {Enc{lycan::Tr(L"main.auto_detect"), 0}, Enc{L"UTF-8", 65001}, Enc{lycan::Tr(L"main.big5_traditional_chinese"), 950}, Enc{lycan::Tr(L"main.gbk_simplified_chinese"), 936},
                              Enc{lycan::Tr(L"main.shift_jis_japanese"), 932}, Enc{lycan::Tr(L"main.euc_kr_korean"), 949}})
        {
            ComboBoxItem item;
            item.Content(box_value(e.label));
            item.Tag(box_value(static_cast<int32_t>(e.cp)));
            EncodingCombo().Items().Append(item);
        }
        EncodingCombo().SelectedIndex(0);

        settingsPath_ = lycan::DefaultSettingsPath();
        settings_ = lycan::LoadSettings(settingsPath_);
        // the names of the file types follow the language of the program (only the names are rewritten, not where the registration points to)
        if (GetEnvironmentVariableW(L"LYCAN_NO_STALE_CHECK", nullptr, 0) == 0 && !StepAsideForStore()) lycan::UpdateAssociationNames(AssocOptions());
        StampSettings();
        RootGrid().ActualThemeChanged([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->UpdateBackdrop(); });
        // the twinkling stars of the home page rest while the window is not the active one (minimized included)
        Activated([weak = get_weak()](auto&&, WindowActivatedEventArgs const& e) {
            if (auto self = weak.get()) { self->windowActive_ = e.WindowActivationState() != WindowActivationState::Deactivated; self->UpdateTwinkle(); }
        });
        // a contrast theme switched on or off while the program runs: the background goes away / comes back, and the rows get their colours again
        hcWatch_ = lycan_app::WatchHighContrast([weak = get_weak()] {
            if (auto self = weak.get())
                self->dispatcher_.TryEnqueue([weak] {
                    if (auto s = weak.get()) { s->UpdateBackdrop(); if (s->view_ == View::Archive) s->RefreshList(); }
                });
        });
        BuildAccentPicker();
        SettingsTabs().SelectedItem(TabItemGeneral());
        InitCompressPage();
        InitShortcuts();
        BuildShellItemsUi();
        this->Closed([weak = get_weak(), id = static_cast<const void*>(this)](auto&&, auto&&) {
            lycan_app::CloseImagePreview(id);  // close only the previews that belong to this window
            lycan_app::CloseAudioPreview(id);
            lycan_app::SettingsHub::Get().Unsubscribe(id);
            if (auto self = weak.get()) self->notifier_.Detach();
        });
        EntryList().SizeChanged([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) s->UpdateListColumns(); });
        InitTreeSplitter();
        InitListHeader();
        SetWindowSubclass(Hwnd(), MinSizeProc, 1, reinterpret_cast<DWORD_PTR>(&compactFlag_));
        lycan_app::SuppressAltBeep(Hwnd());
        notifier_.Attach(Hwnd());
        // Several windows: when another window saved the settings, apply them at once (see SettingsHub.h)
        lycan_app::SettingsHub::Get().Subscribe(this, [weak = get_weak()](lycan::Settings const& s) {
            if (auto self = weak.get()) self->ApplyExternalSettings(s);
        });
        // Closing the window while tasks are running needs a confirmation first; with no tasks it closes at once
        AppWindow().Closing([weak = get_weak()](Microsoft::UI::Windowing::AppWindow const&, Microsoft::UI::Windowing::AppWindowClosingEventArgs const& e) {
            auto self = weak.get();
            if (!self || self->closeConfirmed_) return;
            const int n = self->UnfinishedTaskCount();
            if (n == 0)
            {
                if (!self->HasUnsavedEdits()) return;
                e.Cancel(true);
                self->AskCloseWithEditsAsync();
                return;
            }
            e.Cancel(true);
            self->AskCloseWithTasksAsync(n);
        });
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();

        queue_.SetListener([weak = get_weak()](lycan::TaskInfo const& info) {
            if (auto self = weak.get()) self->Post([self, info] { self->OnTaskUpdate(info); });
        });
        // The list items handle the click first (toggle the check) and mark the event as handled, so a double click is received only with handledEventsToo
        for (UIElement list : {EntryList().as<UIElement>(), EntryGrid().as<UIElement>()})
        {
            list.AddHandler(UIElement::DoubleTappedEvent(), box_value(Input::DoubleTappedEventHandler{this, &MainWindow::OnEntryDoubleTapped}), true);
            list.AddHandler(UIElement::PointerPressedEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandPressed}), true);
            list.AddHandler(UIElement::PointerMovedEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandMoved}), true);
            list.AddHandler(UIElement::PointerReleasedEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandReleased}), true);
            list.AddHandler(UIElement::PointerCanceledEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandReleased}), true);
            list.AddHandler(UIElement::PointerCaptureLostEvent(), box_value(Input::PointerEventHandler{this, &MainWindow::OnBandCaptureLost}), true);
        }
        CleanupStaleTemp();
    }

    // While a window shows an archive, the file is held open for reading (others may read it, nobody may write to it or delete it): a
    // download or a sync program cannot swap the content under a list that was read earlier. Not in the compact window of the context
    // menu (it is gone in a moment). Released before this program changes or deletes the file itself, and when the archive is closed.
    void MainWindow::LockArchiveFile(fs::path const& path)
    {
        UnlockArchiveFile();
        if (!settings_.lockOpenArchive || compact_ || path.empty()) return;
        archiveLock_ = CreateFileW(lycan::ToLongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (archiveLock_ == INVALID_HANDLE_VALUE) lycan::diag::Debug(L"archive lock: not taken ({})", GetLastError());
    }

    void MainWindow::UnlockArchiveFile()
    {
        if (archiveLock_ != INVALID_HANDLE_VALUE) { CloseHandle(archiveLock_); archiveLock_ = INVALID_HANDLE_VALUE; }
    }

    MainWindow::~MainWindow()
    {
        UnlockArchiveFile();
        lycan::diag::Info(L"window: destroyed");
        lycan_app::SettingsHub::Get().Unsubscribe(this);
        closing_ = true;  // lets a worker thread that waits for the user to answer a conflict / dialog give up the wait, so that queue_ can end
        if (aggTimer_) aggTimer_.Stop();
        if (closeTimer_) closeTimer_.Stop();
    }

    void MainWindow::Post(std::function<void()> fn)
    {
        dispatcher_.TryEnqueue([fn = std::move(fn)] { fn(); });
    }

    HWND MainWindow::Hwnd()
    {
        HWND h{};
        check_hresult(this->try_as<::IWindowNative>()->get_WindowHandle(&h));
        return h;
    }

    // The window size is in DIP, converted to pixels by the screen scale; not more than 95% of the work area
    void MainWindow::ResizeDip(int widthDip, int heightDip)
    {
        HWND h = Hwnd();
        double scale = GetDpiForWindow(h) / 96.0;
        MONITORINFO mi{sizeof(mi)};
        int w = static_cast<int>(widthDip * scale), ht = static_cast<int>(heightDip * scale);
        if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi))
        {
            w = std::min(w, static_cast<int>((mi.rcWork.right - mi.rcWork.left) * 0.95));
            ht = std::min(ht, static_cast<int>((mi.rcWork.bottom - mi.rcWork.top) * 0.95));
        }
        AppWindow().Resize({w, ht});
    }

    void MainWindow::ResizeToDefault() { ResizeDip(1000, 700); }

    // The caption (drag) area is the title strip without the settings button and without the system buttons; the button
    // keeps clear of the system buttons by the inset the window reports instead of a fixed margin.
    void MainWindow::UpdateTitleDragRects()
    {
        try {
            const double scale = GetDpiForWindow(Hwnd()) / 96.0;
            auto tb = AppWindow().TitleBar();
            const double right = tb.RightInset() / scale, left = tb.LeftInset() / scale;
            const double width = AppTitleBar().ActualWidth(), height = AppTitleBar().ActualHeight();
            if (width <= 0 || height <= 0) return;
            const bool button = SettingsButton().Visibility() == Visibility::Visible;
            const double bw = button ? 40.0 : 0.0, margin = right + 8;
            if (button) SettingsButton().Margin(ThicknessHelper::FromLengths(0, 0, margin, 0));
            const double bl = width - margin - bw;  // left edge of the button
            std::vector<Windows::Graphics::RectInt32> rects;
            auto add = [&](double x0, double x1) {
                if (x1 - x0 < 1) return;
                rects.push_back({static_cast<int32_t>(x0 * scale), 0, static_cast<int32_t>((x1 - x0) * scale), static_cast<int32_t>(height * scale)});
            };
            if (button) { add(left, bl); add(bl + bw, width - right); }
            else add(left, width - right);
            tb.SetDragRectangles(array_view<const Windows::Graphics::RectInt32>(rects.data(), rects.data() + rects.size()));
        } catch (...) {}
    }

    // Brand pictures are in Assets\ next to the exe (see CopyBrandAssets in the vcxproj); loaded by file path; if not found they stay blank (no effect on functions)
    void MainWindow::LoadBrandImages()
    {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        fs::path dir = fs::path(std::wstring(exe, n)).parent_path() / L"Assets";
        auto load = [&](Image const& img, const wchar_t* file) {
            std::error_code ec;
            fs::path p = dir / file;
            if (!fs::exists(p, ec)) return;
            Microsoft::UI::Xaml::Media::Imaging::BitmapImage bmp{Windows::Foundation::Uri{hstring{p.wstring()}}};
            img.Source(bmp);
        };
        load(TitleLogo(), L"logo-small-32.png");
        load(PreviewWolf(), L"logo-small-32.png");  // the wolf marks the features that are LycanZip's own
        load(HomeLogo(), L"logo-icon-256.png");   // the same icon as the program itself
        load(AboutLogo(), L"logo-icon-128.png");
    }

    // The file-type icon of an archive (the family of its extension, lycan/fileicons.h): a 32 px PNG of Assets\FileIcons, null when it is missing
    static Microsoft::UI::Xaml::Media::Imaging::BitmapImage FileIconImage(fs::path const& archive)
    {
        return lycan_app::FileIconImage(lycan::IconFamilyForPath(archive.wstring()), 32);
    }

    // The icon in front of the breadcrumb: the archive's file-type icon (the generic glyph while there is none)
    void MainWindow::UpdateArchiveIcon()
    {
        auto bmp = FileIconImage(currentPath_);
        PathIcon().Source(bmp);
        PathIcon().Visibility(bmp ? Visibility::Visible : Visibility::Collapsed);
        PathGlyph().Visibility(bmp ? Visibility::Collapsed : Visibility::Visible);
    }

    void MainWindow::SetBusy(bool busy)
    {
        BusyRing().IsActive(busy);
        BusyRing().Visibility(busy ? Visibility::Visible : Visibility::Collapsed);
    }

    static Windows::UI::Color AccentColorNow();

    // ---------------------------------------------------------------- Minimum window size

    // A normal window must not get so small that the layout breaks (only the title bar left); the compact window (context menu action) is small anyway, so its lower limit is relaxed.
    // ref is the compactFlag_ of this window (several windows: one for each window)
    static LRESULT CALLBACK MinSizeProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref)
    {
        if (msg == WM_GETMINMAXINFO)
        {
            const double scale = GetDpiForWindow(hwnd) / 96.0;
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            const bool compact = reinterpret_cast<std::atomic<bool>*>(ref)->load();
            mmi->ptMinTrackSize.x = static_cast<LONG>((compact ? 420 : 640) * scale);
            mmi->ptMinTrackSize.y = static_cast<LONG>((compact ? 220 : 480) * scale);
            return 0;
        }
        return DefSubclassProc(hwnd, msg, wp, lp);
    }

    // Upper limit of the height of the task panel: about 22% of the window height (at least 90, at most 240 DIP); above that a scroll bar is used
    void MainWindow::UpdateTaskPanelMax()
    {
        if (compact_) return;
        TaskScroll().MaxHeight(std::clamp(RootGrid().ActualHeight() * 0.22, 90.0, 240.0));
    }

    // ---------------------------------------------------------------- Motion and sound

    // Played only when the setting is on and the Windows "show animations" is on too
    bool MainWindow::AnimOn() const
    {
        if (!settings_.animations) return false;
        try { return Windows::UI::ViewManagement::UISettings{}.AnimationsEnabled(); } catch (...) { return true; }
    }

    // Fade in and slide in from below (dy is the starting offset, DIP)
    void MainWindow::FadeIn(UIElement const& el, double dy, int ms, int delayMs)
    {
        if (!AnimOn() || !el) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        auto ct = el.RenderTransform().try_as<CompositeTransform>();
        if (!ct) { ct = CompositeTransform{}; el.RenderTransform(ct); }
        el.Opacity(0);
        ct.TranslateY(dy);
        Storyboard sb;
        auto make = [&](DependencyObject const& target, const wchar_t* prop, double from, double to) {
            DoubleAnimation a;
            a.From(from);
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(ms)));
            a.BeginTime(Windows::Foundation::TimeSpan{std::chrono::milliseconds(delayMs)});
            CubicEase ease;
            ease.EasingMode(EasingMode::EaseOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, target);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        };
        make(el, L"Opacity", 0.0, 1.0);
        if (dy != 0) make(ct, L"TranslateY", dy, 0.0);
        sb.Completed([el, ct, sb](auto&&, auto&&) {
            sb.Stop();  // the held value of an animation would override later settings: release it at the end and write back the final value
            el.Opacity(1);
            ct.TranslateY(0);
        });
        sb.Begin();
    }

    // Sound: Assets\Sounds\<name>.wav (placeholders for now, replace them with files of the same name). Off by default, only one plays at a time
    void MainWindow::PlaySfx(const wchar_t* name)
    {
        if (!settings_.soundEffects) { lycan::diag::Debug(L"sound {}: skipped, sound effects are off", name); return; }
        wchar_t exe[MAX_PATH];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        if (n == 0 || n >= std::size(exe)) return;
        auto path = fs::path(exe).parent_path() / L"Assets" / L"Sounds" / (settings_.soundSet == lycan::SoundSet::Wave ? L"Wave" : L"Synth") / (std::wstring(name) + L".wav");
        const bool muted = GetEnvironmentVariableW(L"LYCAN_TEST_MUTE", nullptr, 0) > 0;  // tests must not make noise
        const BOOL queued = muted ? TRUE : PlaySoundW(path.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
        lycan::diag::Info(L"sound {} ({}): {}{}", name, settings_.soundSet == lycan::SoundSet::Wave ? L"Wave" : L"Synth", queued ? L"queued" : L"NOT played", muted ? L" (muted by the test)" : L"");
    }

    // When files are dragged into the window, the halo of the start page grows and glows
    void MainWindow::SetDragGlow(bool on)
    {
        if (dragGlow_ == on) return;
        dragGlow_ = on;
        if (!AnimOn() || view_ != View::Home) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        auto halo = MoonHalo();
        auto ct = halo.RenderTransform().try_as<CompositeTransform>();
        if (!ct) { ct = CompositeTransform{}; halo.RenderTransform(ct); halo.RenderTransformOrigin({0.5, 0.5}); }
        Storyboard sb;
        auto make = [&](DependencyObject const& target, const wchar_t* prop, double to) {
            DoubleAnimation a;
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(260)));
            CubicEase ease;
            ease.EasingMode(EasingMode::EaseOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, target);
            Storyboard::SetTargetProperty(a, prop);
            sb.Children().Append(a);
        };
        make(ct, L"ScaleX", on ? 1.35 : 1.0);
        make(ct, L"ScaleY", on ? 1.35 : 1.0);
        sb.Begin();
    }

    // When a task is done, the frame of the task panel flashes in the accent colour
    void MainWindow::PulseTaskPanel()
    {
        if (!AnimOn() || compact_) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        SolidColorBrush brush{AccentColorNow()};
        TaskPanel().BorderBrush(brush);
        ColorAnimation a;
        a.From(AccentColorNow());
        a.To(Windows::UI::Color{0x30, 0xFF, 0xFF, 0xFF});
        a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(900)));
        Storyboard::SetTarget(a, brush);
        Storyboard::SetTargetProperty(a, L"Color");
        Storyboard sb;
        sb.Children().Append(a);
        sb.Completed([weak = get_weak()](auto&&, auto&&) {
            if (auto self = weak.get()) self->TaskPanel().ClearValue(Border::BorderBrushProperty());
        });
        sb.Begin();
    }

    // The moon of a task card glows once when the task ends: moonlight when it is done, red (twice) when it failed. Also in the compact window (the same list,
    // another template). The card is the data of a template: its element is found through the container of the list, after the layout (a task that ends at
    // once may have no element yet). A card that is not on screen does not glow.
    void MainWindow::GlowMoon(LycanZip::TaskRow const& row, bool failed)
    {
        if (!AnimOn()) return;
        dispatcher_.TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak(), row, failed] {
            auto self = weak.get();
            if (!self) return;
            using namespace Microsoft::UI::Xaml::Media;
            using namespace Microsoft::UI::Xaml::Media::Animation;
            auto container = self->TaskList().ContainerFromItem(row).try_as<DependencyObject>();
            if (!container || VisualTreeHelper::GetChildrenCount(container) == 0) return;
            auto root = VisualTreeHelper::GetChild(container, 0).try_as<FrameworkElement>();
            auto glow = root ? root.FindName(L"MoonGlow").try_as<Microsoft::UI::Xaml::Shapes::Ellipse>() : nullptr;
            lycan::diag::Debug(L"moon glow ({}): {}", failed ? L"failed" : L"done", glow ? L"played" : L"no element");
            if (!glow) return;
            const Windows::UI::Color c = failed ? Windows::UI::Color{255, 0xFF, 0x6B, 0x6B} : Windows::UI::Color{255, 0xF4, 0xF8, 0xFF};
            RadialGradientBrush brush;
            GradientStop inner, outer;
            inner.Color(c);
            inner.Offset(0.4);
            outer.Color(Windows::UI::Color{0, c.R, c.G, c.B});
            outer.Offset(1);
            brush.GradientStops().Append(inner);
            brush.GradientStops().Append(outer);
            glow.Fill(brush);
            auto ct = glow.RenderTransform().try_as<CompositeTransform>();
            if (!ct) { ct = CompositeTransform{}; glow.RenderTransform(ct); }
            Storyboard sb;
            DoubleAnimationUsingKeyFrames op;
            auto key = [&](DoubleAnimationUsingKeyFrames& k, int ms, double v) {
                EasingDoubleKeyFrame f;
                f.KeyTime(KeyTimeHelper::FromTimeSpan(std::chrono::milliseconds(ms)));
                f.Value(v);
                k.KeyFrames().Append(f);
            };
            key(op, 0, 0.0);
            if (failed) { key(op, 140, 0.9); key(op, 300, 0.15); key(op, 440, 0.9); key(op, 760, 0.0); }   // red, twice
            else { key(op, 180, 0.95); key(op, 760, 0.0); }                                             // moonlight, once
            Storyboard::SetTarget(op, glow);
            Storyboard::SetTargetProperty(op, L"Opacity");
            sb.Children().Append(op);
            for (const wchar_t* prop : {L"ScaleX", L"ScaleY"})
            {
                DoubleAnimation sc;
                sc.From(1.0);
                sc.To(failed ? 1.3 : 1.4);   // stays inside the room the templates leave around the moon (Padding): the list clips what spills over
                sc.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(760)));
                CubicEase ease;
                ease.EasingMode(EasingMode::EaseOut);
                sc.EasingFunction(ease);
                Storyboard::SetTarget(sc, ct);
                Storyboard::SetTargetProperty(sc, prop);
                sb.Children().Append(sc);
            }
            sb.Completed([glow, ct, sb](auto&&, auto&&) { sb.Stop(); glow.Opacity(0); ct.ScaleX(1); ct.ScaleY(1); });
            sb.Begin();
        });
    }

    void MainWindow::ShowView(View v)
    {
        const View old = view_;
        view_ = v;
        HomePanel().Visibility(v == View::Home && !loading_ ? Visibility::Visible : Visibility::Collapsed);
        ArchivePanel().Visibility(v == View::Archive ? Visibility::Visible : Visibility::Collapsed);
        SettingsPanel().Visibility(v == View::Settings ? Visibility::Visible : Visibility::Collapsed);
        CompressPanel().Visibility(v == View::Compress ? Visibility::Visible : Visibility::Collapsed);
        ApplyLoadingVisibility();
        UpdateBackdrop();
        if (v != old && !(v == View::Home && loading_))  // page switch: the new page fades in and slides up a little
            FadeIn(v == View::Home ? HomePanel().as<UIElement>() : v == View::Archive ? ArchivePanel().as<UIElement>() : v == View::Compress ? CompressPanel().as<UIElement>() : SettingsPanel().as<UIElement>(), 12, 220);
        UpdateTwinkle();
    }

    // The only animation that keeps running: five of the big stars of the home page slowly dim and come back, each at its own pace (opacity and scale, on the
    // compositor). It stops when the home page is left, the window loses the focus or is minimized, or animations are off.
    void MainWindow::UpdateTwinkle()
    {
        const bool want = AnimOn() && !compact_ && view_ == View::Home && windowActive_ && Stars().Visibility() == Visibility::Visible && HomeScene().Visibility() == Visibility::Visible;
        if (!want)
        {
            for (auto& sb : twinkle_) sb.Stop();   // Stop puts the stars back to their own opacity
            twinkle_.clear();
            return;
        }
        if (!twinkle_.empty()) return;
        using namespace Microsoft::UI::Xaml::Media;
        using namespace Microsoft::UI::Xaml::Media::Animation;
        // the big four-pointed stars of Canis Major (they follow the small ones in Stars()); Sirius, the biggest, also swells a little
        const uint32_t first = static_cast<uint32_t>(starFrac_.size());
        static constexpr struct { uint32_t index; int ms; int delay; bool swell; } kTwinkle[] = {
            {2, 1700, 0, true},       // Sirius
            {0, 2300, 900, false},    // head
            {7, 2000, 400, false},    // hip
            {10, 2600, 1500, false},  // hind leg
            {5, 1900, 2100, false},   // paw
        };
        auto anim = [](DependencyObject const& target, const wchar_t* prop, double to, int ms, int delay) {
            DoubleAnimation a;
            a.To(to);
            a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(ms)));
            a.AutoReverse(true);
            a.RepeatBehavior(RepeatBehaviorHelper::Forever());
            a.BeginTime(Windows::Foundation::TimeSpan{std::chrono::milliseconds(delay)});
            SineEase ease;
            ease.EasingMode(EasingMode::EaseInOut);
            a.EasingFunction(ease);
            Storyboard::SetTarget(a, target);
            Storyboard::SetTargetProperty(a, prop);
            return a;
        };
        for (auto const& t : kTwinkle)
        {
            if (t.index >= conStars_.size() || first + t.index >= Stars().Children().Size()) continue;
            auto star = Stars().Children().GetAt(first + t.index).as<UIElement>();
            Storyboard sb;
            sb.Children().Append(anim(star, L"Opacity", 0.4, t.ms, t.delay));
            if (t.swell)
            {
                auto ct = star.RenderTransform().try_as<CompositeTransform>();
                if (!ct) { ct = CompositeTransform{}; star.RenderTransform(ct); }
                star.RenderTransformOrigin({0.5, 0.5});
                sb.Children().Append(anim(ct, L"ScaleX", 1.22, t.ms, t.delay));
                sb.Children().Append(anim(ct, L"ScaleY", 1.22, t.ms, t.delay));
            }
            sb.Begin();
            twinkle_.push_back(sb);
        }
    }

    // Window title (title bar text and taskbar): LycanZip or LycanZip - file name
    void MainWindow::SetWindowTitle(std::wstring const& archiveName)
    {
        titleName_ = archiveName;
        std::wstring t = archiveName.empty() ? L"LycanZip" : L"LycanZip - " + archiveName;
        if (titlePct_ >= 0) t = std::to_wstring(titlePct_) + L"% - " + t;
        AppTitleText().Text(hstring{t});
        Title(hstring{t});
    }

    void MainWindow::SetTitleProgress(int percent)
    {
        percent = std::clamp(percent, -1, 100);
        if (percent == titlePct_) return;
        titlePct_ = percent;
        SetWindowTitle(titleName_);
    }

    void MainWindow::ApplyLoadingVisibility()
    {
        LoadingPanel().Visibility(loading_ && loadingShown_ && view_ == View::Home ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::ShowLoading(std::wstring const& name)
    {
        RevealWindow();   // the loading view is the first thing a window that opens an archive shows
        LoadingText().Text(hstring{lycan::Tf(L"main.opening", name)});
        loadingShown_ = false;  // the spinner only appears when it takes a moment: a quick open must not flash it
        const bool first = !loading_;
        loading_ = true;
        if (!loadingTimer_)
        {
            loadingTimer_ = dispatcher_.CreateTimer();
            loadingTimer_.IsRepeating(false);
            loadingTimer_.Interval(std::chrono::milliseconds(250));
            loadingTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
                auto self = weak.get();
                if (!self || !self->loading_) return;
                self->loadingTimer_.Stop();
                self->loadingShown_ = true;
                self->ApplyLoadingVisibility();
                if (self->LoadingPanel().Visibility() == Visibility::Visible) self->FadeIn(self->LoadingPanel(), 8, 180);
            });
        }
        loadingTimer_.Stop();
        loadingTimer_.Start();
        if (first) ShowView(view_);  // hides the home page
    }

    void MainWindow::EndLoading(bool goHome)
    {
        if (!loading_) return;
        loading_ = false;
        loadingShown_ = false;
        if (loadingTimer_) loadingTimer_.Stop();
        ApplyLoadingVisibility();
        if (goHome) ShowView(view_);  // the home page comes back (a failed open); a successful one switches the view itself
    }

    void MainWindow::ShowHome()
    {
        lycan_app::CloseImagePreview(this);  // the previews belong to the archive that is going away
        lycan_app::CloseAudioPreview(this);
        SetWindowTitle(L"");
        archiveInfo_.clear();
        UnlockArchiveFile();
        current_.reset();
        NotifyStateChanged(true);
        tree_.reset();
        entries_ = nullptr;
        EntryList().ItemsSource(nullptr);
        EntryGrid().ItemsSource(nullptr);
        FolderTree().RootNodes().Clear();
        treeNodes_.clear();
        password_.clear();
        passwordVerified_ = false;
        ShowView(View::Home);
    }

    // ---------------------------------------------------------------- Dialogs

    // ---------------------------------------------------------------- Shared parts of dialogs
    static Windows::UI::Color AccentColorNow()
    {
        try
        {
            if (auto b = Application::Current().Resources().Lookup(box_value(L"AccentTextFillColorPrimaryBrush")).try_as<Microsoft::UI::Xaml::Media::SolidColorBrush>())
                return b.Color();
        }
        catch (...) {}
        return Windows::UI::Color{255, 0x6E, 0xA8, 0xFF};
    }

    // What a menu item does, for the colour of its icon: delete = red, malware / VirusTotal = amber, test / verify = green, add / change =
    // the accent colour of the theme, everything else = the colour of the text. The colours are those of the Windows system brushes
    // SystemFillColorCritical / Caution / Success (the XAML menus use the brushes themselves).
    enum class IconTone { None, Danger, Security, Verify, Accent };

    // The icon of a menu item (Segoe Fluent Icons, in the colour of the text unless the tone says otherwise). dark: the theme of the window
    static FontIcon MenuIcon(const wchar_t* glyph, IconTone tone = IconTone::None, bool dark = true)
    {
        FontIcon g;
        g.Glyph(glyph);
        Windows::UI::Color c{};
        switch (tone)
        {
        case IconTone::Danger:   c = dark ? Windows::UI::Color{255, 0xFF, 0x99, 0xA4} : Windows::UI::Color{255, 0xC4, 0x2B, 0x1C}; break;
        case IconTone::Security: c = dark ? Windows::UI::Color{255, 0xFC, 0xE1, 0x00} : Windows::UI::Color{255, 0x9D, 0x5D, 0x00}; break;
        case IconTone::Verify:   c = dark ? Windows::UI::Color{255, 0x6C, 0xCB, 0x5F} : Windows::UI::Color{255, 0x0F, 0x7B, 0x0F}; break;
        case IconTone::Accent:   c = AccentColorNow(); break;
        default: return g;
        }
        g.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(c)});
        return g;
    }

    static FontIcon MakeGlyph(const wchar_t* glyph, double size, Microsoft::UI::Xaml::Media::Brush const& brush = nullptr)
    {
        FontIcon icon;
        icon.Glyph(glyph);
        icon.FontSize(size);
        icon.Foreground(brush ? brush : Microsoft::UI::Xaml::Media::SolidColorBrush{AccentColorNow()});
        icon.VerticalAlignment(VerticalAlignment::Center);
        return icon;
    }

    static StackPanel SectionHeader(const wchar_t* glyph, const wchar_t* text)
    {
        StackPanel p;
        p.Orientation(Orientation::Horizontal);
        p.Spacing(8);
        p.Children().Append(MakeGlyph(glyph, 14));
        TextBlock t;
        t.Text(text);
        t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        p.Children().Append(t);
        return p;
    }

    // A faint card background (a translucent grey that does not depend on the theme, works for dark and light)
    static Border MakeCardBox(UIElement const& child, double padH = 12, double padV = 10)
    {
        Border b;
        b.CornerRadius(CornerRadiusHelper::FromUniformRadius(8));
        b.BorderThickness(ThicknessHelper::FromUniformLength(1));
        b.Padding(ThicknessHelper::FromLengths(padH, padV, padH, padV));
        b.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x40, 0x80, 0x80, 0x80}});
        b.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x14, 0x80, 0x80, 0x80}});
        b.Child(child);
        return b;
    }

    static TextBlock Caption(std::wstring const& text, double opacity = 0.7)
    {
        TextBlock t;
        t.Text(hstring{text});
        t.Opacity(lycan_app::HighContrast() ? (std::max)(opacity, 0.95) : opacity);   // dimmed text is hard to read on a contrast theme
        t.FontSize(12);
        t.TextWrapping(TextWrapping::Wrap);
        return t;
    }

    // The two lines of text in an option card: title + description
    static StackPanel TitledOption(const wchar_t* title, const wchar_t* desc)
    {
        StackPanel p;
        p.Spacing(1);
        TextBlock t;
        t.Text(title);
        p.Children().Append(t);
        p.Children().Append(Caption(desc, 0.65));
        return p;
    }

    // The overview of the sources (number of files and total size) is computed in the background and the text is updated on the UI thread when done; with too many items it counts only up to a limit
    static fire_and_forget ScanSources(std::vector<fs::path> inputs, TextBlock target, Microsoft::UI::Dispatching::DispatcherQueue dq)
    {
        co_await resume_background();
        uint64_t files = 0, bytes = 0, dirs = 0;
        bool truncated = false;
        auto t0 = std::chrono::steady_clock::now();
        for (auto const& in : inputs)
        {
            std::error_code ec;
            if (fs::is_directory(in, ec))
            {
                ++dirs;
                for (fs::recursive_directory_iterator it(in, fs::directory_options::skip_permission_denied, ec), end; it != end && !ec; it.increment(ec))
                {
                    std::error_code e2;
                    if (it->is_regular_file(e2)) { ++files; bytes += it->file_size(e2); }
                    else if (it->is_directory(e2)) ++dirs;
                    if (files + dirs > 100000 || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(3)) { truncated = true; break; }
                }
            }
            else
            {
                ++files;
                bytes += fs::file_size(in, ec);
            }
            if (truncated) break;
        }
        std::wstring text = lycan::Tf(L"fmt.n_files", files);
        if (dirs) text += lycan::Tf(L"fmt.and_n_folders", dirs);
        text += lycan::Tf(L"main.total", FormatSize(bytes)) + (truncated ? lycan::Tr(L"main.many_items_only_part_were_counted") : L"");
        dq.TryEnqueue([target, text] { target.Text(hstring{text}); });
    }

    ContentDialog MainWindow::MakeDialog(hstring const& title, const wchar_t* glyph)
    {
        ContentDialog dlg;
        dlg.XamlRoot(RootGrid().XamlRoot());
        dlg.RequestedTheme(RootGrid().ActualTheme());  // a dialog does not inherit RequestedTheme by itself
        if (glyph)
        {
            StackPanel head;
            head.Orientation(Orientation::Horizontal);
            head.Spacing(10);
            head.Children().Append(MakeGlyph(glyph, 22));
            TextBlock t;
            t.Text(title);
            t.FontSize(20);
            t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            t.VerticalAlignment(VerticalAlignment::Center);
            head.Children().Append(t);
            dlg.Title(head);
        }
        else dlg.Title(box_value(title));
        return dlg;
    }

    // WinUI allows only one ContentDialog at a time for one XamlRoot: later ones wait in a queue
    Windows::Foundation::IAsyncOperation<ContentDialogResult> MainWindow::ShowDialogAsync(ContentDialog dlg)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        if (sLeaveBackground) sLeaveBackground();  // a prompt needs the user: bring the hidden windows back first
        // Right after the window starts (for example an encrypted archive opened from the command line) XamlRoot may not be ready yet; dialogs must not overlap either
        while ((dialogOpen_ || !RootGrid().XamlRoot()) && !closing_)
        {
            co_await resume_after(50ms);
            co_await resume_ui{dispatcher_};
        }
        if (closing_) co_return ContentDialogResult::None;
        RevealWindow();
        dlg.XamlRoot(RootGrid().XamlRoot());
        dlg.RequestedTheme(RootGrid().ActualTheme());
        dialogOpen_ = true;
        if (compact_)
        {
            // The compact window is only there for the question: the dialog is drawn straight on it (no dimming layer, no frame, no
            // rounded corners, as wide and as high as the window) instead of floating in a window that is bigger than it needs to be.
            auto res = dlg.Resources();
            const Media::SolidColorBrush none{Windows::UI::Color{0, 0, 0, 0}};
            res.Insert(box_value(L"ContentDialogSmokeFill"), none);
            res.Insert(box_value(L"ContentDialogBorderBrush"), none);
            res.Insert(box_value(L"ContentDialogBorderWidth"), box_value(ThicknessHelper::FromUniformLength(0)));
            res.Insert(box_value(L"OverlayCornerRadius"), box_value(CornerRadiusHelper::FromUniformRadius(0)));
            res.Insert(box_value(L"ContentDialogMinWidth"), box_value(static_cast<double>(dialogWidthDip_)));
            res.Insert(box_value(L"ContentDialogMaxWidth"), box_value(static_cast<double>(dialogWidthDip_)));
            // the height follows the content (a short question does not need a tall window); long content scrolls inside the dialog
            double height = 780;
            if (auto el = dlg.Content().try_as<UIElement>())
            {
                try
                {
                    el.Measure(Windows::Foundation::Size{static_cast<float>(dialogWidthDip_ - 48), 4000.0f});
                    height = std::clamp(static_cast<double>(el.DesiredSize().Height) + 210.0, 300.0, 780.0);
                }
                catch (...) {}
            }
            res.Insert(box_value(L"ContentDialogMinHeight"), box_value(height - 38));
            res.Insert(box_value(L"ContentDialogMaxHeight"), box_value(2000.0));
            compactDialogHeight_ = height;
            // content that grows while the dialog is open (the results of the checksums, a list that gets longer) makes the window grow too
            if (auto fe = dlg.Content().try_as<FrameworkElement>())
                fe.SizeChanged([weak = get_weak()](auto&&, SizeChangedEventArgs const& e) {
                    auto s = weak.get();
                    if (!s || !s->compact_ || !s->dialogOpen_) return;
                    const double want = std::clamp(static_cast<double>(e.NewSize().Height) + 210.0, 300.0, 780.0);
                    if (want > s->compactDialogHeight_ + 1) { s->compactDialogHeight_ = want; s->ResizeDip(s->dialogWidthDip_, static_cast<int>(want)); }
                });
            ResizeDip(dialogWidthDip_, static_cast<int>(height));  // the compact window is small, and dialogs (new archive, conflict ...) need more room
        }
        ContentDialogResult r = ContentDialogResult::None;
        try { r = co_await dlg.ShowAsync(); }
        catch (...) {}
        dialogOpen_ = false;
        if (compact_) ShrinkCompactSoon();
        co_return r;
    }

    // After a dialog closes, shrink the compact window back; wait a moment, and if another queued dialog follows at once, do not shrink
    fire_and_forget MainWindow::ShrinkCompactSoon()
    {
        auto strong = get_strong();
        co_await resume_after(250ms);
        co_await resume_ui{dispatcher_};
        if (compact_ && !dialogOpen_ && !closing_) ResizeDip(620, 300);
    }

    fire_and_forget MainWindow::ShowMessage(hstring title, hstring body)
    {
        auto strong = get_strong();
        lycan::diag::Warn(L"message shown to the user: {}", std::wstring(title));  // (the text may carry paths: only in the detailed log)
        lycan::diag::Info(L"message text: {}", std::wstring(body));
        OpGuard guard{this};  // until the user has seen the message (including waiting in the queue) the compact window must not close by itself; check again after it closes
        co_await resume_ui{dispatcher_};
        auto dlg = MakeDialog(title, L"\uE946");
        TextBlock tb;
        tb.Text(body);
        tb.TextWrapping(TextWrapping::Wrap);
        dlg.Content(tb);
        dlg.CloseButtonText(lycan::Tr(L"main.ok"));
        co_await ShowDialogAsync(dlg);
    }

    // ---------------------------------------------------------------- Start requests (command line / context menu / handed over by other instances)

    void MainWindow::ReportBadArguments(std::wstring const& error)
    {
        ShowMessage(lycan::Tr(L"main.invalid_command_line_arguments"), hstring{error});
    }

    void MainWindow::DeferShow()
    {
        deferredShow_ = true;
        revealTimer_ = dispatcher_.CreateTimer();
        revealTimer_.IsRepeating(false);
        revealTimer_.Interval(1000ms);
        revealTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->RevealWindow(); });
        revealTimer_.Start();
    }

    void MainWindow::RevealWindow()
    {
        if (!deferredShow_) return;
        deferredShow_ = false;
        if (revealTimer_) revealTimer_.Stop();
        if (previewOnly_) return;   // the preview window is the whole UI
        lycan::diag::Info(L"window: shown");
        AppWindow().Show();
        Activate();
    }

    void MainWindow::ActivateWindow()
    {
        RevealWindow();
        HWND h = Hwnd();
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        Activate();
        SetForegroundWindow(h);
    }

    // Compact window: an action of the context menu needs only the progress, not the main window
    void MainWindow::EnterCompact()
    {
        compact_ = true;
        compactFlag_ = true;
        ContentRow().Height(GridLength{0, GridUnitType::Pixel});
        HomePanel().Visibility(Visibility::Collapsed);
        ArchivePanel().Visibility(Visibility::Collapsed);
        SettingsPanel().Visibility(Visibility::Collapsed);
        CompressPanel().Visibility(Visibility::Collapsed);
        SettingsButton().Visibility(Visibility::Collapsed);
        UpdateTitleDragRects();
        // Compact window: only the task card with the big moon (no title bar and frame), the size follows the screen scale
        TaskHeader().Visibility(Visibility::Collapsed);
        TaskPanel().Background(nullptr);
        TaskPanel().BorderThickness(ThicknessHelper::FromUniformLength(0));
        TaskPanel().Margin(ThicknessHelper::FromLengths(28, 0, 28, 20));
        TaskScroll().MaxHeight(900);
        TaskList().ItemTemplate(RootGrid().Resources().Lookup(box_value(L"TaskHeroTemplate")).as<DataTemplate>());
        ResizeDip(620, 300);
        SetCompactResizable(false);
        UpdateBackdrop();
        UpdateCompactOptions();
    }

    // The compact window has a fixed size (it grows by itself for a dialog and shrinks back): no dragging the edges, no maximizing; it can still be minimized
    void MainWindow::SetCompactResizable(bool resizable)
    {
        try
        {
            if (auto p = AppWindow().Presenter().try_as<Microsoft::UI::Windowing::OverlappedPresenter>())
            {
                p.IsResizable(resizable);
                p.IsMaximizable(resizable);
            }
        }
        catch (...) {}
    }

    void MainWindow::ExitCompact()
    {
        if (!compact_) return;
        compactHost_ = false;
        compact_ = false;
        compactFlag_ = false;
        SetCompactResizable(true);
        CompactDoneButton().Visibility(Visibility::Collapsed);
        if (closeTimer_) closeTimer_.Stop();
        ContentRow().Height(GridLength{1, GridUnitType::Star});
        SettingsButton().Visibility(Visibility::Visible);
        UpdateTitleDragRects();
        TaskHeader().Visibility(Visibility::Visible);
        TaskPanel().Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});  // restore: back to the card background of the theme
        TaskPanel().ClearValue(Border::BackgroundProperty());
        TaskPanel().ClearValue(Border::BorderThicknessProperty());
        CompactOptions().Visibility(Visibility::Collapsed);
        TaskPanel().Margin(ThicknessHelper::FromLengths(20, 0, 20, 16));
        UpdateTaskPanelMax();
        TaskList().ItemTemplate(RootGrid().Resources().Lookup(box_value(L"TaskRowTemplate")).as<DataTemplate>());
        ResizeToDefault();
        ShowView(current_ ? View::Archive : View::Home);
    }

    // The compact window closes by itself when "no flow is running and all tasks succeeded"; with a failure it stays for the user to see
    void MainWindow::CheckCompactAutoClose()
    {
        UpdateCompactDone();
        if (!compact_ || closing_) return;
        if (busyOps_ > 0 || dialogOpen_ || quickRunning_ || !pendingPaths_.empty()) return;

        bool any = false;
        for (auto const& t : queue_.Snapshot())
        {
            if (!t.Finished()) return;
            any = true;
            bool ok = t.state == lycan::TaskState::Completed &&
                      (t.kind != lycan::TaskKind::Extract || t.extract.ok()) &&
                      (t.kind != lycan::TaskKind::Compress || t.compress.ok()) && (t.kind != lycan::TaskKind::Update || t.update.ok());
            // a cancelled integrity test goes back to the finished screen: it follows the "close the window" option
            if (t.kind == lycan::TaskKind::Test && t.state == lycan::TaskState::Cancelled) ok = true;
            if (!ok) return;
        }

        if (!closeTimer_)
        {
            closeTimer_ = dispatcher_.CreateTimer();
            closeTimer_.IsRepeating(false);
            closeTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
                if (auto self = weak.get())
                {
                    self->closeTimer_.Stop();
                    // if a new task / dialog comes while the timer waits, do not close
                    if (self->compact_ && !self->closing_ && self->busyOps_ == 0 && !self->dialogOpen_ && !self->quickRunning_ &&
                        self->pendingPaths_.empty())
                    {
                        bool allDone = true;
                        for (auto const& t : self->queue_.Snapshot()) if (!t.Finished()) allDone = false;
                        if (!allDone) {}
                        else if (self->notifier_.BalloonActive()) { self->closeTimer_.Interval(1000ms); self->closeTimer_.Start(); }  // a notification is still shown; it disappears when the process ends
                        else self->FadeOutAndClose();
                    }
                }
            });
        }
        if (any && !settings_.closeCompactWhenDone) return;  // the user wants to read the result: stay open
        closeTimer_.Interval(any ? 1200ms : 50ms);  // after success wait a moment so that "Done" can be seen; if nothing was done (a dialog was cancelled) close at once
        closeTimer_.Stop();
        closeTimer_.Start();
    }

    // After the compact window is done it closes at once: no fade (nothing that delays or decorates closing a window)
    void MainWindow::FadeOutAndClose()
    {
        if (closing_) return;
        Close();
    }

    void MainWindow::HandleRequest(lycan::LaunchRequest const& req, bool fromLaunch)
    {
        using lycan::LaunchAction;
        lycan::diag::Info(L"request: action {}, {} path(s){}", static_cast<int>(req.action), req.paths.size(), fromLaunch ? L", from the launch" : L"");
        if (req.action == LaunchAction::None)
        {
            if (!fromLaunch) ActivateWindow();
            else if (!MaybeShowWelcome()) { CheckInstalledCopiesAsync(); CheckForUpdateAsync(false); }  // the program was started on its own: the welcome window the first time, and a good moment to say that the menus still point to the old folder (or that LycanZip is installed twice)
            return;
        }
        if (req.IsPreviewOnly())
        {
            if (previewOnly_) PreviewOnlyAsync(req.paths[0], req.action == LaunchAction::PreviewAudio);
            else { ActivateWindow(); OpenArchiveAsync(req.paths[0], 0, L""); }  // a normal window got the request: just open the archive
            return;
        }
        if (previewOnly_) { previewOnly_ = false; AppWindow().Show(); }  // another kind of request reached the hidden host: it becomes a normal window
        if (fromLaunch && (req.action == LaunchAction::Add || req.action == LaunchAction::AddEncrypted)) cFromCompact_ = true;  // creating an archive needs the full page; shrink back to the compact window after the compression starts
        else if (fromLaunch && req.IsQuickAction())
        {
            compactExtract_ = req.IsArchiveQuickAction();
            compactTest_ = req.action == LaunchAction::Test;
            EnterCompact();
        }
        else if (!fromLaunch) ActivateWindow();

        switch (req.action)
        {
        case LaunchAction::Open:
            ExitCompact();
            if (IsArchivePath(req.paths[0])) { OpenArchiveChecked(req.paths[0]); break; }
            if (req.paths.size() == 1 && IsExePath(req.paths[0])) { HandleDroppedPaths(req.paths); break; }
            [[fallthrough]];  // not an archive: becomes "create an archive"
        case LaunchAction::Add:
        case LaunchAction::AddEncrypted:
        case LaunchAction::Compress:
        {
            // On a multi-select Explorer starts a process for each file: collect the paths of the same batch of requests for a short time, then handle them at once
            LaunchAction kind = req.action == LaunchAction::Open ? LaunchAction::Add : req.action;
            if (pendingKind_ != LaunchAction::None && (pendingKind_ != kind || pendingFormat_ != req.format || pendingPreset_ != req.preset || pendingDest_ != req.dest)) FlushPending();
            pendingKind_ = kind;
            pendingFormat_ = req.format;
            pendingPreset_ = req.preset;
            pendingDest_ = req.dest;
            for (auto const& p : req.paths)
                if (std::find(pendingPaths_.begin(), pendingPaths_.end(), p) == pendingPaths_.end()) pendingPaths_.push_back(p);
            if (!aggTimer_)
            {
                aggTimer_ = dispatcher_.CreateTimer();
                aggTimer_.IsRepeating(false);
                aggTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->FlushPending(); });
            }
            aggTimer_.Interval(500ms);
            aggTimer_.Stop();
            aggTimer_.Start();
            break;
        }
        case LaunchAction::CompressEach:
            for (auto const& p : req.paths) AutoCompress({p}, req.format, req.preset, false, req.dest);   // one archive each: a name that is taken gets (2)
            CheckCompactAutoClose();
            break;
        case LaunchAction::ExtractHere:
        case LaunchAction::ExtractFolder:
        case LaunchAction::Test:
            for (auto const& p : req.paths)
            {
                QuickJob job;
                job.archive = p;
                job.dest = req.dest;   // the right-drag menu: into the folder the files were dropped on (empty = next to the archive)
                job.folderMode = req.action == LaunchAction::ExtractFolder;
                job.test = req.action == LaunchAction::Test;
                quickQueue_.push_back(std::move(job));
            }
            RunQuickQueue();
            break;
        case LaunchAction::ExtractTo:
        case LaunchAction::ExtractMerge:
            ExtractToRequestAsync(req);
            break;
        case LaunchAction::HashSave:
            if (fromLaunch)
            {
                // no window yet: it is shown only when the calculation takes long, or when it has something to ask
                compactHost_ = true;
                compactExtract_ = false;
                compactTest_ = false;
                EnterCompact();
                if (revealTimer_) revealTimer_.Stop();
            }
            HashSaveAsync(req.paths, fromLaunch);
            break;
        case LaunchAction::Hash:
        case LaunchAction::Verify:
            if (fromLaunch)
            {
                // the window only hosts the dialog: the small window (the dialog makes it as big as it needs), not the home page behind it
                compactHost_ = true;
                compactExtract_ = false;
                compactTest_ = false;
                EnterCompact();
            }
            if (req.action == LaunchAction::Hash) HashRequestAsync(req.paths, fromLaunch);
            else QueueVerify(req.paths, fromLaunch);
            break;
        case LaunchAction::OpenContent:
            ExitCompact();
            OpenArchiveAsync(req.paths[0], 0, L"");  // whatever the extension is: treat the file as an archive
            break;
        default: break;
        }
        if ((pendingKind_ == LaunchAction::None || compact_) && req.action != LaunchAction::HashSave) RevealWindow();   // the screen that fits the request is set (a request that ends on the compress page shows the window when the page is up)
    }

    // Ask for the destination once (unless the menu gave one), then extract every archive there (merge = contents straight into it)
    fire_and_forget MainWindow::ExtractToRequestAsync(lycan::LaunchRequest req)
    {
        auto strong = get_strong();
        OpGuard guard{this};  // the compact window must not close while the question is open
        fs::path dest = req.dest;
        if (dest.empty())
            dest = fs::path(std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, req.paths[0].parent_path(),
                                                                             lycan::Tr(L"main.choose_folder_extract"))));
        if (dest.empty()) co_return;  // ~OpGuard checks whether the compact window can close
        RememberRecentExtract(dest);
        for (auto const& p : req.paths)
        {
            QuickJob job;
            job.archive = p;
            job.dest = dest;
            job.noFolder = req.action == lycan::LaunchAction::ExtractMerge;
            quickQueue_.push_back(std::move(job));
        }
        RunQuickQueue();
    }

    // The folders chosen with "Extract to...": the context menu offers them under "Extract to a recent folder"
    void MainWindow::RememberRecentExtract(fs::path const& dir)
    {
        lycan::RememberExtractDir(settings_.recentExtract, dir.wstring());
        SaveSettings();
    }

    void MainWindow::FlushPending()
    {
        if (aggTimer_) aggTimer_.Stop();
        if (pendingPaths_.empty()) { CheckCompactAutoClose(); return; }
        auto paths = std::move(pendingPaths_);
        pendingPaths_.clear();
        auto kind = pendingKind_;
        const auto format = pendingFormat_;
        const auto preset = pendingPreset_;
        const auto dest = pendingDest_;
        pendingKind_ = lycan::LaunchAction::None;
        std::sort(paths.begin(), paths.end());  // the processes arrive in no fixed order: sorting keeps the resulting names stable
        switch (kind)
        {
        case lycan::LaunchAction::Compress: AutoCompress(std::move(paths), format, preset, true, dest); break;
        case lycan::LaunchAction::AddEncrypted:
            StartCompress(std::move(paths));
            // the password is the point of this entry; focus after the page has been laid out
            {
                auto t = dispatcher_.CreateTimer();
                t.IsRepeating(false);
                t.Interval(400ms);
                t.Tick([weak = get_weak(), t](auto&&, auto&&) { if (auto self = weak.get()) { t.Stop(); self->CPwText().Focus(FocusState::Programmatic); } });
                t.Start();
            }
            break;
        default: StartCompress(std::move(paths)); break;
        }
        CheckCompactAutoClose();
        RevealWindow();
    }

    fs::path MainWindow::DefaultArchiveOutput(std::vector<fs::path> const& inputs, const wchar_t* ext, bool unique, fs::path const& outDir)
    {
        std::wstring baseName = inputs.size() == 1 ? inputs[0].stem().wstring() : inputs[0].parent_path().filename().wstring();
        if (inputs.size() == 1 && fs::is_directory(inputs[0])) baseName = inputs[0].filename().wstring();
        if (baseName.empty()) baseName = L"Archive";
        const fs::path dir = outDir.empty() ? inputs[0].parent_path() : outDir;
        return unique ? UniquePath(dir / (baseName + ext)) : dir / (baseName + ext);
    }

    fire_and_forget MainWindow::AutoCompress(std::vector<fs::path> paths, lycan::ArchiveType type, std::wstring preset, bool askExisting, fs::path outDir)
    {
        auto strong = get_strong();
        OpGuard guard{this};   // the compact window stays while the question is open
        lycan::CompressOptions co;
        co.tempDir = lycan_app::ScratchOverride();
        co.skipThumbsDb = settings_.skipThumbsDb;
        co.type = type;
        co.profile = settings_.defaultProfile;
        bool sumsA = settings_.sumsArchive, sumsF = settings_.sumsFiles;   // a template has its own; without one the settings say
        if (!preset.empty())
        {
            // a preset from the built-in list or presets.ini; it decides the format too
            std::vector<lycan::CompressPreset> all = lycan::BuiltinPresets();
            for (auto& u : lycan::LoadPresets(lycan::DefaultPresetsPath())) all.push_back(std::move(u));
            for (auto const& p : all) if (p.name == preset) { lycan::ApplyPreset(p, co); sumsA = p.sumsArchive; sumsF = p.sumsFiles; break; }
        }
        // Note: compute the output name first, then move. The order in which arguments are evaluated in one call is not fixed; paths could be moved away before DefaultArchiveOutput reads it
        const std::wstring ext = std::wstring(L".") + lycan::FormatName(co.type);
        fs::path output = DefaultArchiveOutput(paths, ext.c_str(), !askExisting, outDir);
        std::vector<fs::path> sources = paths;
        if (askExisting)
        {
            std::error_code ec;
            if (fs::exists(output, ec))
            {
                // the name is taken: make a new archive under another name, replace the old one, or add to it (the update modes)
                const bool canUpdate = co.type == lycan::ArchiveType::Zip || co.type == lycan::ArchiveType::SevenZip || co.type == lycan::ArchiveType::Tar;
                const int32_t choice = co_await ChooseExistingOutputAsync(output, canUpdate);
                if (choice == 0) co_return;
                if (choice == 1) output = UniquePath(output);
                else if (choice == 2) co.overwrite = true;
                else
                {
                    lycan::UpdateOptions uo;
                    uo.compress = co;
                    uo.mode = static_cast<lycan::UpdateMode>(choice - 3);
                    uo.keepBackup = settings_.keepBackupOnModify;
                    std::vector<lycan::UpdateOp> ops = {lycan::UpdateOp::Add(L"", paths)};
                    if (uo.mode == lycan::UpdateMode::Synchronize && !co_await ConfirmSyncAsync(output, ops, uo)) co_return;
                    std::lock_guard lk(metaM_);
                    queue_.EnqueueUpdate(output, std::move(ops), uo);
                    co_return;
                }
            }
        }
        {
            std::lock_guard lk(metaM_);
            uint64_t id = queue_.EnqueueCompress(std::move(output), std::move(paths), co);
            CompressMeta m;
            m.sources = std::move(sources);
            m.testAfter = settings_.testAfterCompress;
            m.sumsArchive = sumsA;
            m.sumsFiles = sumsF;
            compressMeta_[id] = std::move(m);
        }
    }

    fire_and_forget MainWindow::RunQuickQueue()
    {
        auto strong = get_strong();
        if (quickRunning_) co_return;
        quickRunning_ = true;
        while (!quickQueue_.empty() && !closing_)
        {
            QuickJob job = std::move(quickQueue_.front());
            quickQueue_.pop_front();
            try { co_await QuickExtractOne(std::move(job)); }
            catch (...) {}
        }
        quickRunning_ = false;
        CheckCompactAutoClose();
    }

    // Right click "Extract here / to a folder of the same name": no file list view; open -> ask for the password if needed -> queue the extraction task directly
    Windows::Foundation::IAsyncAction MainWindow::QuickExtractOne(QuickJob job)
    {
        auto strong = get_strong();
        const fs::path path = job.archive;
        co_await resume_ui{dispatcher_};
        hstring name{path.filename().wstring()};

        std::wstring password;
        bool wrong = false;
        std::shared_ptr<lycan::ArchiveSource> src;
        for (;;)
        {
            std::wstring errorText;
            bool needPassword = false;
            co_await resume_background();
            try
            {
                lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
                if (!password.empty()) oo.password = [password](std::wstring& out) { out = password; return true; };
                src = lycan::ArchiveSource::Open(path, oo);
            }
            catch (lycan::Error const& e)
            {
                if (e.wrongPassword()) { needPassword = true; wrong = true; }
                else if (e.cancelled()) needPassword = true;
                else errorText = e.message().Text();
            }
            catch (std::exception const& e) { errorText = FromUtf8(e.what()); }
            co_await resume_ui{dispatcher_};

            if (src) break;
            if (needPassword)
            {
                auto pw = co_await AskPasswordAsync(name, wrong);
                if (pw.empty()) co_return;
                password = std::wstring(pw);
                continue;
            }
            ShowMessage(lycan::Tr(L"main.cannot_open_archive"), hstring{errorText.empty() ? lycan::Tf(L"main.file_format_unrecognized_file", path.wstring()) : errorText});
            co_return;
        }

        // Encrypted content (zip AES, 7z data encryption): verify the password first and ask again if wrong; with encrypted headers it was verified while opening
        if (src->Caps().encrypted && password.empty())
        {
            bool wrongPw = false;
            for (;;)
            {
                auto pw = co_await AskPasswordAsync(name, wrongPw);
                if (pw.empty()) co_return;
                co_await resume_background();
                bool ok = src->CheckPassword(std::wstring(pw));
                co_await resume_ui{dispatcher_};
                if (ok) { password = std::wstring(pw); break; }
                wrongPw = true;
            }
        }

        if (job.test)
        {
            lycan::OpenOptions to;
            to.tempDir = lycan_app::ScratchOverride();
            if (!password.empty()) to.password = [password](std::wstring& out) { out = password; return true; };
            queue_.EnqueueTest(path, to);
            co_return;
        }

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        const lycan::ExtractMode mode = job.noFolder ? lycan::ExtractMode::NoFolder : job.folderMode ? lycan::ExtractMode::Folder : settings_.extractMode;
        eo.smart = mode == lycan::ExtractMode::Auto;
        eo.overwrite = lycan::ToOverwriteMode(settings_.conflict);
        if (eo.overwrite == lycan::OverwriteMode::Ask) { auto rule = std::make_shared<std::atomic<int>>(0); eo.onConflict = [this, rule](lycan::ConflictInfo const& ci) { return AskConflict(ci, rule); }; }
        ApplyLargeGuard(eo);
        lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
        if (!password.empty())
        {
            eo.password = [password](std::wstring& out) { out = password; return true; };
            oo.password = eo.password;
        }
        const fs::path base = job.dest.empty() ? path.parent_path() : job.dest;
        fs::path dest = mode == lycan::ExtractMode::Folder ? base / lycan::ArchiveBaseName(path) : base;

        TaskMeta meta;
        meta.archive = path;
        meta.wholeArchive = true;
        std::lock_guard lk(metaM_);
        uint64_t id = queue_.EnqueueExtract(path, dest, eo, oo, {});
        taskMeta_[id] = std::move(meta);
    }

    // ---------------------------------------------------------------- Settings

    // After a DataTemplate (list row, task row) is instantiated, translate the button text written into it
    void MainWindow::OnTemplateLoaded(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        if (auto d = sender.try_as<DependencyObject>()) lycan_app::TranslateTemplateRoot(d);
    }


    // Shows a notice when the chosen language differs from the one loaded now (it takes effect after a restart)
    void MainWindow::UpdateLangNote()
    {
        const bool pending = lycan::i18n::Resolve(settings_.language, lycan::i18n::DefaultLangDir()) != lycan::i18n::Current();
        LangRestartNote().Visibility(pending ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::LoadSettingsToUi()
    {
        loadingSettings_ = true;
        UpdateShellItemsUi();
        ThemeCombo().SelectedIndex(static_cast<int>(settings_.theme));
        {
            // the menu: follow the system, then the languages of lycan::kLanguages (the first item is in the XAML)
            if (LangCombo().Items().Size() == 1)
                for (const auto& l : lycan::kLanguages) {
                    ComboBoxItem item;
                    item.Content(box_value(hstring{lycan::Tr(l.nameId)}));
                    LangCombo().Items().Append(item);
                }
            const int found = lycan::LanguageIndex(settings_.language);
            LangCombo().SelectedIndex(found < 0 ? 0 : found + 1);
            UpdateLangNote();
        }
        AccentGrid().SelectedIndex(static_cast<int>(settings_.accent));
        BackgroundCombo().SelectedIndex(static_cast<int>(settings_.background));
        ConflictCombo().SelectedIndex(static_cast<int>(settings_.conflict));
        ExtractModeCombo().SelectedIndex(static_cast<int>(settings_.extractMode));
        CloseCompactToggle().IsOn(settings_.closeCompactWhenDone);
        RevealToggle().IsOn(settings_.revealAfterCompress);
        OpenFolderToggle().IsOn(settings_.openFolderAfterExtract);
        BombCheckToggle().IsOn(settings_.bombCheck);
        SkipMacMetaToggle().IsOn(settings_.skipMacMetadata);
        ShellIconsToggle().IsOn(settings_.shellIcons);
        KeepDamagedToggle().IsOn(settings_.keepDamaged);
        ConfirmDeleteToggle().IsOn(settings_.confirmDeleteArchive);
        ConfirmDeleteEntriesToggle().IsOn(settings_.confirmDeleteEntries);
        ConfirmWriteBackToggle().IsOn(settings_.confirmWriteBack);
        EditorPathBox().Text(hstring{settings_.editorPath});
        KeepBackupToggle().IsOn(settings_.keepBackupOnModify);
        DragVirtualToggle().IsOn(settings_.dragOutVirtual);
        SkipThumbsToggle().IsOn(settings_.skipThumbsDb);
        HideMacMetaToggle().IsOn(settings_.hideMacMetadata);
        WarnExecToggle().IsOn(settings_.warnExecutable);
        LockOpenToggle().IsOn(settings_.lockOpenArchive);
        UpdateCheckToggle().IsOn(settings_.checkUpdates);
        ExecScopeCombo().SelectedIndex(static_cast<int>(settings_.execScope));
        TestAfterToggle().IsOn(settings_.testAfterCompress);
        SumsArchiveToggle().IsOn(settings_.sumsArchive);
        SumsFilesToggle().IsOn(settings_.sumsFiles);
        DefaultFormatCombo().SelectedIndex(IndexOf(kFormatOrder, settings_.defaultFormat));
        DefaultProfileCombo().SelectedIndex(IndexOf(kProfileOrder, settings_.defaultProfile));
        // details, list, then the icon view in its four sizes (one choice, like the View menu)
        ViewStyleCombo().SelectedIndex(settings_.viewMode == lycan::ViewMode::Icons ? 2 + static_cast<int>(std::min(settings_.thumbSize, 3u)) : static_cast<int>(settings_.viewMode));
        ViewStyleCombo().IsEnabled(!settings_.rememberViewMode);  // when "remember the last choice" is on, the default view is the last choice
        RememberViewToggle().IsOn(settings_.rememberViewMode);
        ThumbsToggle().IsOn(settings_.iconThumbnails);
        ViewModeCombo().SelectedIndex(settings_.browseFlat ? 1 : 0);
        OpenPolicyCombo().SelectedIndex(static_cast<int>(settings_.openIn));
        DensityCombo().SelectedIndex(settings_.compactRows ? 0 : 1);
        FolderTreeToggle().IsOn(settings_.showFolderTree);
        DblPreviewCombo().SelectedIndex(settings_.doubleClickPreview ? 0 : 1);
        OpenPreviewKeyCombo().SelectedIndex(static_cast<int>(settings_.openPreviewKey));
        DblAudioCombo().SelectedIndex(settings_.doubleClickAudioPreview ? 0 : 1);
        {
            constexpr unsigned kPrefetchSteps[] = {0, 128, 256, 512, 1024, 2048};
            int sel = 2;
            for (int i = 0; i < 6; ++i) if (settings_.prefetchMB <= kPrefetchSteps[i]) { sel = i; break; }
            if (settings_.prefetchMB > 2048) sel = 5;
            PrefetchCombo().SelectedIndex(sel);
        }
        EnlargeToggle().IsOn(settings_.previewEnlargeSmall);
        AnimToggle().IsOn(settings_.animations);
        SoundToggle().IsOn(settings_.soundEffects);
        SoundSetCombo().SelectedIndex(static_cast<int>(settings_.soundSet));
        NotifyToggle().IsOn(settings_.notifications);
        DebugLogToggle().IsOn(settings_.debugLog);
        LogPathsToggle().IsOn(settings_.logFullPaths);
        ParallelBox().Value(settings_.parallelTasks);
        UpdateTempCard();
        TaskPriorityCombo().SelectedIndex(static_cast<int>(settings_.taskPriority));
        loadingSettings_ = false;
    }

    void MainWindow::ReadSettingsFromUi()
    {
        if (loadingSettings_) return;
        auto idx = [](ComboBox const& c, int max) { return std::clamp(c.SelectedIndex(), 0, max); };
        settings_.theme = static_cast<lycan::ThemeMode>(idx(ThemeCombo(), 3));
        {
            const int sel = idx(LangCombo(), lycan::kLanguageCount);   // 0 = follow the system
            settings_.language = sel == 0 ? L"auto" : lycan::kLanguages[sel - 1].code;
        }
        UpdateLangNote();
        settings_.conflict = static_cast<lycan::ConflictPolicy>(idx(ConflictCombo(), 3));
        settings_.extractMode = static_cast<lycan::ExtractMode>(idx(ExtractModeCombo(), 2));
        settings_.closeCompactWhenDone = CloseCompactToggle().IsOn();
        settings_.revealAfterCompress = RevealToggle().IsOn();
        settings_.openFolderAfterExtract = OpenFolderToggle().IsOn();
        settings_.bombCheck = BombCheckToggle().IsOn();
        settings_.skipMacMetadata = SkipMacMetaToggle().IsOn();
        const bool shellIconsChanged = settings_.shellIcons != ShellIconsToggle().IsOn();
        settings_.shellIcons = ShellIconsToggle().IsOn();
        if (shellIconsChanged)
        {
            if (entries_) for (auto const& it : entries_) ApplyShellIcon(it.as<LycanZip::EntryRow>());
            for (auto const& [id, tn] : treeNodes_) if (tn.Content()) ApplyShellIcon(tn.Content().as<LycanZip::EntryRow>());
            RefreshShellIcons();
        }
        settings_.keepDamaged = KeepDamagedToggle().IsOn();
        settings_.confirmDeleteArchive = ConfirmDeleteToggle().IsOn();
        settings_.confirmDeleteEntries = ConfirmDeleteEntriesToggle().IsOn();
        settings_.confirmWriteBack = ConfirmWriteBackToggle().IsOn();
        settings_.keepBackupOnModify = KeepBackupToggle().IsOn();
        settings_.dragOutVirtual = DragVirtualToggle().IsOn();
        settings_.skipThumbsDb = SkipThumbsToggle().IsOn();
        const bool hideMetaChanged = settings_.hideMacMetadata != HideMacMetaToggle().IsOn();
        settings_.hideMacMetadata = HideMacMetaToggle().IsOn();
        settings_.warnExecutable = WarnExecToggle().IsOn();
        settings_.checkUpdates = UpdateCheckToggle().IsOn();
        {
            const bool was = settings_.lockOpenArchive;
            settings_.lockOpenArchive = LockOpenToggle().IsOn();
            if (was != settings_.lockOpenArchive)
            {
                if (settings_.lockOpenArchive && current_ && updateTask_ == 0) LockArchiveFile(currentPath_);
                else if (!settings_.lockOpenArchive) UnlockArchiveFile();
            }
        }
        settings_.execScope = static_cast<lycan::OpenScope>(idx(ExecScopeCombo(), 1));
        settings_.testAfterCompress = TestAfterToggle().IsOn();
        settings_.sumsArchive = SumsArchiveToggle().IsOn();
        settings_.sumsFiles = SumsFilesToggle().IsOn();
        settings_.defaultFormat = kFormatOrder[idx(DefaultFormatCombo(), 5)];   // (no ISO among the defaults)
        settings_.defaultProfile = kProfileOrder[idx(DefaultProfileCombo(), 3)];
        const int viewChoice = idx(ViewStyleCombo(), 5);   // 0 details, 1 list, 2..5 icons small .. huge
        const lycan::ViewMode chosenView = viewChoice < 2 ? static_cast<lycan::ViewMode>(viewChoice) : lycan::ViewMode::Icons;
        const unsigned chosenSize = viewChoice >= 2 ? static_cast<unsigned>(viewChoice - 2) : settings_.thumbSize;
        const bool thumbChanged = settings_.iconThumbnails != ThumbsToggle().IsOn() || settings_.thumbSize != chosenSize;
        const bool rememberChanged = settings_.rememberViewMode != RememberViewToggle().IsOn();
        settings_.rememberViewMode = RememberViewToggle().IsOn();
        settings_.iconThumbnails = ThumbsToggle().IsOn();
        settings_.thumbSize = chosenSize;
        bool viewChanged = settings_.viewMode != chosenView || settings_.browseFlat != (idx(ViewModeCombo(), 1) == 1) || settings_.showFolderTree != FolderTreeToggle().IsOn();
        settings_.viewMode = chosenView;
        settings_.background = static_cast<lycan::BackgroundStyle>(idx(BackgroundCombo(), 2));
        settings_.browseFlat = idx(ViewModeCombo(), 1) == 1;
        settings_.openIn = static_cast<lycan::OpenPolicy>(idx(OpenPolicyCombo(), 1));
        settings_.compactRows = idx(DensityCombo(), 1) == 0;
        settings_.showFolderTree = FolderTreeToggle().IsOn();
        settings_.doubleClickPreview = idx(DblPreviewCombo(), 1) == 0;
        settings_.openPreviewKey = static_cast<lycan::PreviewKey>(idx(OpenPreviewKeyCombo(), 2));
        settings_.doubleClickAudioPreview = idx(DblAudioCombo(), 1) == 0;
        {
            constexpr unsigned kPrefetchSteps[] = {0, 128, 256, 512, 1024, 2048};
            settings_.prefetchMB = kPrefetchSteps[idx(PrefetchCombo(), 5)];
        }
        settings_.previewEnlargeSmall = EnlargeToggle().IsOn();
        settings_.animations = AnimToggle().IsOn();
        const lycan::SoundSet oldSoundSet = settings_.soundSet;
        settings_.soundEffects = SoundToggle().IsOn();
        settings_.soundSet = static_cast<lycan::SoundSet>(idx(SoundSetCombo(), 1));
        if (settings_.soundSet != oldSoundSet && settings_.soundEffects) PlaySfx(L"done");  // hear the other set
        settings_.notifications = NotifyToggle().IsOn();
        {
            const bool dbgChanged = settings_.debugLog != DebugLogToggle().IsOn();
            settings_.debugLog = DebugLogToggle().IsOn();
            settings_.logFullPaths = LogPathsToggle().IsOn();
            if (dbgChanged) lycan::diag::Configure(settings_.debugLog, settings_.logFullPaths);  // (written even when it is switched off: the end of the detailed part)
            if (dbgChanged) lycan::diag::Write(lycan::diag::Level::Warn, settings_.debugLog ? L"the detailed log was switched on" : L"the detailed log was switched off");
        }
        settings_.taskPriority = static_cast<lycan::TaskPriority>(idx(TaskPriorityCombo(), 2));
        double n = ParallelBox().Value();
        settings_.parallelTasks = std::isnan(n) ? 1u : static_cast<unsigned>(std::clamp(n, 1.0, 4.0));
        SaveSettings();
        ApplySettings();
        if (rememberChanged) ViewStyleCombo().IsEnabled(!settings_.rememberViewMode);
        if (thumbChanged && tree_ && !viewChanged) { UpdateViewMenuChecks(); RefreshList(true); }
        if (viewChanged) ApplyViewSettings();  // only when the default view itself changed is the view of the archive open now overridden (not reset because another setting changed)
        if (hideMetaChanged) RebuildTree();
    }

    void MainWindow::SaveSettings()
    {
        if (!lycan::SaveSettings(settingsPath_, settings_))
            AboutSettingsPath().Text(hstring{lycan::Tf(L"main.cannot_write_settings_file", settingsPath_.wstring())});
        StampSettings();
        lycan_app::SettingsHub::Get().Publish(this, settings_);  // the other windows get the newest settings at once
    }

    // Another window saved the settings: replace the working copy of this window and apply (only the look and global behaviour, not the view state of the archive that is open)
    void MainWindow::ApplyExternalSettings(lycan::Settings const& s)
    {
        if (s == settings_) return;
        settings_ = s;
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
        LoadColWidths();
        UpdateListColumns();
        StampSettings();  // the file was just written by the other side: no need to reload it again on activation
    }

    int MainWindow::UnfinishedTaskCount()
    {
        int n = 0;
        for (auto const& t : queue_.Snapshot())
            if (!t.Finished()) ++n;
        return n;
    }

    MainWindow::TaskStats MainWindow::CollectTaskStats()
    {
        TaskStats s;
        for (auto const& t : queue_.Snapshot())
        {
            if (!t.Finished())
            {
                ++s.running;
                if (t.total > 0) { s.done += t.done; s.total += t.total; }
            }
            else
            {
                const bool ok = t.state != lycan::TaskState::Failed &&
                                (t.state != lycan::TaskState::Completed ||
                                 ((t.kind != lycan::TaskKind::Extract || t.extract.ok()) && (t.kind != lycan::TaskKind::Compress || t.compress.ok()) && (t.kind != lycan::TaskKind::Update || t.update.ok())));
                if (!ok) ++s.failed;
            }
        }
        if (busyOps_ > 0 || dialogOpen_ || quickRunning_ || !pendingPaths_.empty()) s.needsUi = 1;
        return s;
    }

    // Background work: the window disappears, the tasks (in worker processes) keep running
    void MainWindow::HideForBackground()
    {
        lycan_app::CloseImagePreview(this);
        lycan_app::CloseAudioPreview(this);
        AppWindow().Hide();
    }

    // --preview-images / --preview-audio: the window never shows; the preview window is the whole UI
    void MainWindow::SetPreviewOnly()
    {
        previewOnly_ = true;
        AppWindow().Hide();
    }

    // The preview could not be shown straight away (password, damaged file, nothing to preview): show this window and
    // open the archive the normal way so the user sees what is going on.
    void MainWindow::LeavePreviewOnly(fs::path const& path, std::wstring const& note)
    {
        previewOnly_ = false;
        openNote_ = note;
        AppWindow().Show();
        ActivateWindow();
        OpenArchiveAsync(path, 0, L"");
    }

    fire_and_forget MainWindow::PreviewOnlyAsync(fs::path path, bool audio)
    {
        auto strong = get_strong();
        std::shared_ptr<lycan::ArchiveSource> src;
        std::vector<lycan_app::PreviewItem> images;
        std::vector<lycan_app::AudioItem> tracks;
        bool encrypted = false;
        co_await resume_background();
        try
        {
            lycan::OpenOptions oo;
            oo.tempDir = lycan_app::ScratchOverride();
            src = lycan::ArchiveSource::Open(path, oo);
            encrypted = src->Caps().encrypted;
            // every level of the archive, in path order (the same as the list's "show everything" view)
            lycan::ArchiveTree tree(src->Entries(), true);  // the junk macOS adds is never previewed
            for (uint32_t id : tree.Descendants(tree.Root(), lycan::SortKey::Path, true, false))
            {
                auto const& n = tree.Node(id);
                if (n.isDir || n.entryIndex == lycan::kNoEntry) continue;
                if (audio) { if (lycan_app::IsPreviewableAudioName(n.name)) tracks.push_back({n.entryIndex, n.name, n.path, n.size}); }
                else if (lycan_app::CanPreviewImage(n.name)) images.push_back({n.entryIndex, n.name, n.path, n.size, 0, n.packedSize, n.mtimeFileTime});
            }
        }
        catch (...) { src.reset(); }  // password, damaged, no room...: the normal window explains it
        co_await resume_ui{dispatcher_};
        if (closing_) co_return;

        if (!src || encrypted) { LeavePreviewOnly(path, L""); co_return; }
        if (images.empty() && tracks.empty())
        {
            LeavePreviewOnly(path, audio ? lycan::Tr(L"main.there_no_audio_preview_archive") : lycan::Tr(L"main.there_no_images_preview_archive"));
            co_return;
        }

        auto onClosed = [weak = get_weak()] {
            if (auto self = weak.get())
                self->dispatcher_.TryEnqueue([weak] { if (auto s = weak.get()) if (s->previewOnly_ && !s->closing_) s->Close(); });  // the last window: the process ends
        };
        if (audio)
        {
            lycan_app::AudioRequest req;
            req.owner = this;
            req.archive = path;
            req.open.tempDir = lycan_app::ScratchOverride();
            req.items = std::move(tracks);
            req.onClosed = onClosed;
            req.loopMode = static_cast<int>(settings_.audioLoop);
            req.onLoopMode = [weak = get_weak()](int m) { if (auto s = weak.get()) { s->settings_.audioLoop = static_cast<lycan::AudioLoop>(m); s->SaveSettings(); } };
            lycan_app::ShowAudioPreview(std::move(req));
        }
        else
        {
            lycan_app::PreviewRequest req;
            req.owner = this;
            req.archive = path;
            req.open.tempDir = lycan_app::ScratchOverride();
            req.prefetchBytes = static_cast<uint64_t>(settings_.prefetchMB) << 20;
            req.enlargeSmall = settings_.previewEnlargeSmall;
            req.animations = AnimOn();
            req.thumbReads = src->Caps().randomAccess && !src->Caps().solid;
            req.items = std::move(images);
            req.onClosed = onClosed;  // no main window to hand files to: "open with the associated program" (Enter) is unavailable
            lycan_app::ShowImagePreview(std::move(req));
        }
    }

    void MainWindow::ShowFromBackground()
    {
        AppWindow().Show();
        ActivateWindow();
    }

    void MainWindow::OnBackgroundClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (sEnterBackground) sEnterBackground();
    }

    // ---------------------------------------------------------------- temporary files

    void MainWindow::UpdateTempCard()
    {
        const bool custom = !settings_.tempDir.empty();
        TempDirText().Text(hstring{custom ? settings_.tempDir : std::wstring{lycan::Tr(L"main.system_temporary_folder")}});
        TempResetButton().IsEnabled(custom);
        RefreshTempUsage();
    }

    fire_and_forget MainWindow::RefreshTempUsage()
    {
        auto strong = get_strong();
        co_await resume_background();
        const uint64_t used = lycan_app::OpenTempUsage();
        co_await resume_ui{dispatcher_};
        TempUsageText().Text(hstring{lycan::Tf(L"main.use_opened_dragged_files", FormatSize(used))});
    }

    fire_and_forget MainWindow::OnTempChooseClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        const fs::path picked{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeTempDir,
                                                                               settings_.tempDir.empty() ? fs::path{} : fs::path{settings_.tempDir}, lycan::Tr(L"main.choose_temporary_folder")))};
        if (picked.empty()) co_return;
        if (lycan::IsSlowLocation(picked))
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.location_slow"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.network_removable_drive_temporary", picked.wstring())});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(420);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"main.use_anyway"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Close);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        }
        settings_.tempDir = picked.wstring();
        SaveSettings();
        ApplySettings();
        UpdateTempCard();
    }

    void MainWindow::OnTempResetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        settings_.tempDir.clear();
        SaveSettings();
        ApplySettings();
        UpdateTempCard();
    }

    fire_and_forget MainWindow::OnTempClearClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        co_await resume_background();
        const uint64_t freed = lycan_app::ClearOpenTemp();
        co_await resume_ui{dispatcher_};
        RefreshTempUsage();
        ShowMessage(lycan::Tr(L"main.temporary_files_cleared"), hstring{lycan::Tf(L"main.freed_files_use_not_deleted", FormatSize(freed))});
    }

    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskTempLocationAsync(uint64_t need, uint64_t have, std::wstring dir)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        // local disks with room, other than the one that is full
        const std::wstring fullRoot = fs::path(dir).root_path().wstring();
        const uint64_t want = need ? need + need / 20 : (2ull << 30);
        std::vector<fs::path> bases;
        auto dlg = MakeDialog(lycan::Tr(L"main.not_enough_room_temporary_files"), L"");
        StackPanel sp;
        sp.Spacing(12);
        TextBlock info;
        info.Text(hstring{need ? lycan::Tf(L"main.about_intermediate_data_has_be_put", FormatSize(need), dir, FormatSize(have))
                               : lycan::Tf(L"main.temporary_location_full_left", dir, FormatSize(have))});
        info.TextWrapping(TextWrapping::Wrap);
        info.MaxWidth(460);
        sp.Children().Append(info);
        RadioButtons options;
        options.Header(box_value(lycan::Tr(L"main.where_use_instead_local_disks_only")));
        for (auto const& v : lycan::LocalFixedVolumes())
        {
            if (v.root.wstring() == fullRoot || v.freeBytes < want) continue;
            bases.push_back(v.root);
            TextBlock t;
            t.Text(hstring{lycan::Tf(L"main.free", v.root.wstring(), FormatSize(v.freeBytes))});
            options.Items().Append(t);
        }
        TextBlock none;
        none.Text(lycan::Tr(L"main.no_other_local_disk_has_enough"));
        none.TextWrapping(TextWrapping::Wrap);
        none.MaxWidth(460);
        none.Opacity(0.8);
        none.Visibility(bases.empty() ? Visibility::Visible : Visibility::Collapsed);
        sp.Children().Append(none);
        if (!bases.empty()) options.SelectedIndex(0);
        options.Visibility(bases.empty() ? Visibility::Collapsed : Visibility::Visible);
        sp.Children().Append(options);
        TextBlock slow;
        slow.Text(lycan::Tr(L"main.network_removable_drive_temporary_files_will_be_slow"));
        slow.Opacity(0.8);
        slow.Visibility(Visibility::Collapsed);
        HyperlinkButton other;
        other.Content(box_value(lycan::Tr(L"main.choose_another_folder")));
        other.Padding(ThicknessHelper::FromUniformLength(0));
        CheckBox always;
        always.Content(box_value(lycan::Tr(L"main.always_use_temporary_location")));
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.use_location_try_again"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        dlg.IsPrimaryButtonEnabled(!bases.empty());
        const auto owner = AppWindow().Id();
        other.Click([&, owner](auto&&, auto&&) -> fire_and_forget {
            const fs::path p{std::wstring(co_await lycan_app::PickFolderAsync(owner, lycan_app::kPurposeTempDir, {}, lycan::Tr(L"main.choose_temporary_folder")))};
            if (p.empty()) co_return;
            bases.push_back(p);
            TextBlock t;
            t.Text(hstring{lycan::Tf(L"main.chosen", p.wstring())});
            options.Visibility(Visibility::Visible);
            options.Items().Append(t);
            options.SelectedIndex(static_cast<int>(options.Items().Size()) - 1);
            none.Visibility(Visibility::Collapsed);
            slow.Visibility(lycan::IsSlowLocation(p) ? Visibility::Visible : Visibility::Collapsed);
            dlg.IsPrimaryButtonEnabled(true);
        });
        sp.Children().Append(other);
        sp.Children().Append(slow);
        sp.Children().Append(always);
        const auto r = co_await ShowDialogAsync(dlg);
        if (r != ContentDialogResult::Primary) co_return hstring{};
        const int i = options.SelectedIndex();
        if (i < 0 || static_cast<size_t>(i) >= bases.size()) co_return hstring{};
        const fs::path base = bases[static_cast<size_t>(i)];
        if (always.IsChecked().Value())
        {
            settings_.tempDir = base.wstring();
            SaveSettings();
            ApplySettings();
            UpdateTempCard();
        }
        else lycan_app::SetSessionTempBase(base);  // for the rest of this run
        co_return hstring{base.wstring()};
    }

    // a task failed because the scratch folder was too small: ask where else, then run it again there
    fire_and_forget MainWindow::HandleTempSpaceTask(lycan::TaskInfo info)
    {
        auto strong = get_strong();
        if (!tempAsked_.insert(info.id).second) co_return;
        const hstring base = co_await AskTempLocationAsync(info.tempNeed, info.tempHave, info.tempDir);
        if (base.empty()) co_return;
        const uint64_t nid = queue_.Retry(info.id, fs::path{std::wstring{base}} / L"LycanZip");
        if (!nid) co_return;
        {
            std::lock_guard lk(metaM_);  // what happens after the task (test the new archive, open the folder...) follows to the new one
            if (auto c = compressMeta_.find(info.id); c != compressMeta_.end()) compressMeta_[nid] = c->second;
            if (auto t = taskMeta_.find(info.id); t != taskMeta_.end()) { auto m = t->second; m.handled = false; taskMeta_[nid] = std::move(m); }
        }
        RemoveTaskRow(info.id);
    }

    void MainWindow::RemoveTaskRow(uint64_t id)
    {
        for (uint32_t i = 0; i < taskRows_.Size(); ++i)
            if (taskRows_.GetAt(i).as<LycanZip::TaskRow>().Id() == id)
            {
                taskRows_.RemoveAt(i);
                break;
            }
        taskById_.erase(id);
        {
            std::lock_guard lk(metaM_);
            taskMeta_.erase(id);
            compressMeta_.erase(id);
        }
        if (taskRows_.Size() == 0) TaskPanel().Visibility(Visibility::Collapsed);
        UpdateTaskSummary();
    }

    fire_and_forget MainWindow::AskCloseWithTasksAsync(int count)
    {
        auto strong = get_strong();
        if (askingClose_ || dialogOpen_) co_return;  // already asking, or another dialog is not answered yet: handle that first
        askingClose_ = true;
        auto dlg = MakeDialog(lycan::Tr(L"main.tasks_still_running"), L"\uE7BA");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.still_unfinished_closing_now", count)});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.cancel_tasks_close"));
        dlg.CloseButtonText(lycan::Tr(L"main.keep_waiting"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const auto r = co_await ShowDialogAsync(dlg);
        askingClose_ = false;
        if (r == ContentDialogResult::Primary)
        {
            closeConfirmed_ = true;
            queue_.CancelAll();
            Close();
        }
    }

    void MainWindow::NotifyStateChanged(bool force)
    {
        if (!sStateChanged) return;
        const uint64_t now = GetTickCount64();
        if (!force && now - stateTick_ < 1000) return;
        stateTick_ = now;
        sStateChanged();
    }

    void MainWindow::AppendCrashState(std::vector<fs::path>& archives, std::vector<lycan::CrashTask>& tasks) const
    {
        if (current_) archives.push_back(currentPath_);
        else if (openPending_ > 0 && !openingPath_.empty()) archives.push_back(openingPath_);
        for (auto const& t : queue_.Snapshot())
        {
            if (t.Finished()) continue;
            lycan::CrashTask ct;
            ct.kind = t.kind == lycan::TaskKind::Compress ? L"Compress" : t.kind == lycan::TaskKind::Test ? L"Test" : t.kind == lycan::TaskKind::Update ? L"Update" : t.kind == lycan::TaskKind::Scan ? L"Scan" : L"Extract";
            ct.state = t.state == lycan::TaskState::Running ? L"Running" : t.state == lycan::TaskState::Paused ? L"Paused" : L"Queued";
            ct.title = t.title;
            ct.target = t.target;
            tasks.push_back(std::move(ct));
        }
    }

    // The last crash: first remove the compression temporary files that are clearly ours and mark the report as seen (however the user answers, do not ask at every start), then explain
    void MainWindow::ShowCrashRecovery(std::vector<lycan::CrashReport> reports)
    {
        int cleaned = 0;
        for (auto& r : reports)
        {
            cleaned += lycan::CleanupCrashLeftovers(r);
            lycan::MarkCrashReportSeen(r);
        }
        ShowCrashRecoveryAsync(std::move(reports), cleaned);
    }

    fire_and_forget MainWindow::ShowCrashRecoveryAsync(std::vector<lycan::CrashReport> reports, int cleaned)
    {
        auto strong = get_strong();
        if (reports.empty() || dialogOpen_) co_return;
        auto const& r = reports.front();  // the newest one
        auto kindName = [](std::wstring const& k) -> std::wstring {
            return k == L"Compress" ? lycan::Tr(L"main.compression") : k == L"Test" ? lycan::Tr(L"main.integrity_test") : lycan::Tr(L"main.extraction");
        };
        constexpr size_t kShow = 5;
        std::wstring body = lycan::Tf(L"main.lycanzip_closed_unexpectedly_at", r.time);
        if (!r.archives.empty())
        {
            body += L"\n\n" + lycan::Tf(L"main.open_at_time", r.archives.size());
            for (size_t i = 0; i < r.archives.size() && i < kShow; ++i) body += L"\n• " + r.archives[i].filename().wstring();
            if (r.archives.size() > kShow) body += L"\n…";
        }
        if (!r.tasks.empty())
        {
            body += L"\n\n" + lycan::Tf(L"main.not_finished", r.tasks.size());
            bool anyExtract = false;
            for (size_t i = 0; i < r.tasks.size(); ++i)
            {
                if (r.tasks[i].kind == L"Extract") anyExtract = true;
                if (i < kShow) body += L"\n• " + kindName(r.tasks[i].kind) + L" " + r.tasks[i].title;
            }
            if (r.tasks.size() > kShow) body += L"\n…";
            if (anyExtract) body += L"\n" + std::wstring(lycan::Tr(L"main.destination_extraction_may_contain"));
        }
        if (cleaned > 0) body += L"\n\n" + lycan::Tf(L"main.removed_unfinished_compression", cleaned);
        const fs::path crashDir = r.file.parent_path();
        body += L"\n\n" + lycan::Tf(L"main.crash_report_including_memory_dump", crashDir.wstring());

        auto dlg = MakeDialog(lycan::Tr(L"main.lycanzip_closed_unexpectedly_last"), L"\uE7BA");
        TextBlock tb;
        tb.Text(hstring{body});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(520);
        dlg.Content(tb);
        std::vector<fs::path> reopen;
        for (auto const& a : r.archives)
        {
            std::error_code ec;
            if (reopen.size() < kShow && fs::is_regular_file(a, ec)) reopen.push_back(a);
        }
        if (!reopen.empty()) dlg.PrimaryButtonText(hstring{lycan::Tf(L"main.reopen", reopen.size())});
        dlg.SecondaryButtonText(lycan::Tr(L"main.open_report_folder"));
        dlg.CloseButtonText(lycan::Tr(L"main.skip"));
        dlg.DefaultButton(reopen.empty() ? ContentDialogButton::Close : ContentDialogButton::Primary);
        const auto res = co_await ShowDialogAsync(dlg);
        if (res == ContentDialogResult::Primary)
        {
            if (sOpenInNewWindow)
                for (auto const& a : reopen) sOpenInNewWindow(a);
        }
        else if (res == ContentDialogResult::Secondary)
            ShellExecuteW(nullptr, L"open", crashDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::StampSettings()
    {
        std::error_code ec;
        auto t = fs::last_write_time(settingsPath_, ec);
        settingsStamp_ = ec ? fs::file_time_type{} : t;
    }

    // Several windows: when another window saved the settings file (theme, animations, notifications ...), reload and apply it when this window is activated.
    // The state of the archive view (view mode, sort, thumbnails) is not touched, only the global look / behaviour settings are updated.
    void MainWindow::ReloadSettingsIfChanged()
    {
        std::error_code ec;
        auto t = fs::last_write_time(settingsPath_, ec);
        if (ec || t == settingsStamp_) return;
        settingsStamp_ = t;
        auto fresh = lycan::LoadSettings(settingsPath_);
        if (fresh == settings_) return;
        settings_ = fresh;
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
    }

    bool MainWindow::IsOpenableArchive(fs::path const& p) { return IsArchivePath(p); }

    // The same archive (case-insensitive / any way of writing the path): this window shows it or is opening it
    bool MainWindow::ShowsArchive(fs::path const& p) const
    {
        auto same = [&](fs::path const& a) {
            if (a.empty()) return false;
            std::error_code ec;
            return fs::equivalent(a, p, ec) && !ec;
        };
        return (current_ && same(currentPath_)) || (openPending_ > 0 && same(openingPath_));
    }

    bool MainWindow::IsPristine() const
    {
        return view_ == View::Home && !current_ && !compact_ && openPending_ == 0 && busyOps_ == 0 && !dialogOpen_ && !cPageActive_ &&
               taskById_.empty() && pendingPaths_.empty() && !quickRunning_ && !closing_;
    }

    bool MainWindow::ReplaceOnOpen() const
    {
        return settings_.openIn == lycan::OpenPolicy::Replace && taskById_.empty() && pendingPaths_.empty() && !dialogOpen_ && !cPageActive_ &&
               !quickRunning_ && !closing_ && openPending_ == 0 && busyOps_ == 0;
    }

    Windows::Foundation::IAsyncOperation<int> MainWindow::AskOpenBatchAsync(int total, int safeCount, int maxCount)
    {
        auto strong = get_strong();
        if (dialogOpen_) co_return safeCount;  // another dialog is not answered yet: open only the safe number, do not stack dialogs
        const int all = (std::min)(total, maxCount);
        auto dlg = MakeDialog(lycan::Tr(L"main.opening_several_archives"), L"\uE8A7");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.received_archives_at_once_each", total)});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(hstring{all < total ? lycan::Tf(L"main.open_first", all) : lycan::Tf(L"main.open_all", all)});
        dlg.SecondaryButtonText(hstring{lycan::Tf(L"main.open_only_first", safeCount)});
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Secondary);
        const auto r = co_await ShowDialogAsync(dlg);
        co_return r == ContentDialogResult::Primary ? all : r == ContentDialogResult::Secondary ? safeCount : 0;
    }

    void MainWindow::OpenFromUi(fs::path p)
    {
        if (!ReplaceOnOpen() && !IsPristine() && sOpenInNewWindow) { sOpenInNewWindow(std::move(p)); return; }
        OpenArchiveChecked(std::move(p));
    }

    // A later volume of a split archive (name.part2.rar, name.002 ...) shows only part of the archive: every time, the user is
    // asked whether to open the first volume instead (never automatic). Without the first volume next to it there is nothing to
    // offer: the volume opens, with a note.
    fire_and_forget MainWindow::OpenArchiveChecked(fs::path path)
    {
        auto strong = get_strong();
        const lycan::VolumeInfo vol = lycan::DetectVolume(path);
        if (vol.IsLater())
        {
            std::error_code ec;
            if (fs::exists(vol.first, ec))
            {
                OpGuard guard{this};
                auto dlg = MakeDialog(lycan::Tr(L"main.later_volume_split_archive"), L"\uE8B7");
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.volume_split_archive_one_volume", path.filename().wstring(), vol.index, vol.first.filename().wstring())});
                tb.TextWrapping(TextWrapping::Wrap);
                tb.MaxWidth(440);
                dlg.Content(tb);
                dlg.PrimaryButtonText(lycan::Tr(L"main.open_first_volume"));
                dlg.SecondaryButtonText(lycan::Tr(L"main.open_volume_anyway"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Primary);
                const auto r = co_await ShowDialogAsync(dlg);
                if (r == ContentDialogResult::Primary) path = vol.first;
                else if (r != ContentDialogResult::Secondary) { CheckCompactAutoClose(); co_return; }
            }
            else openNote_ = lycan::Tf(L"main.first_volume_was_not_found_only", vol.first.filename().wstring());
        }
        OpenArchiveAsync(std::move(path), 0, L"");
    }

    void MainWindow::ApplySettings()
    {
        const bool contrast = settings_.theme == lycan::ThemeMode::ContrastBlack;
        RootGrid().RequestedTheme(settings_.theme == lycan::ThemeMode::Light  ? ElementTheme::Light
                                  : settings_.theme == lycan::ThemeMode::Dark || contrast ? ElementTheme::Dark
                                                                              : ElementTheme::Default);
        if (contrast != lycan_app::ContrastThemeOn() || contrast)
        {
            const bool changed = contrast != lycan_app::ContrastThemeOn();
            lycan_app::SetContrastTheme(contrast);
            if (!contrast) ApplyAccent();
            if (changed)
            {   // the controls ask for their brushes again when the theme changes
                RootGrid().RequestedTheme(ElementTheme::Light);
                RootGrid().RequestedTheme(contrast ? ElementTheme::Dark : (settings_.theme == lycan::ThemeMode::Light ? ElementTheme::Light : settings_.theme == lycan::ThemeMode::Dark ? ElementTheme::Dark : ElementTheme::Default));
            }
        }
        queue_.SetMaxParallel(settings_.parallelTasks);
        queue_.SetDefaultPriority(settings_.taskPriority);
        lycan::diag::Configure(settings_.debugLog, settings_.logFullPaths);
        lycan_app::SetCustomTempBase(settings_.tempDir);
        ApplyDensity();
        UpdateBackdrop();
        UpdateExtractButton();
        UpdateCompactOptions();
    }

    // ---- Accent colour: overrides the SystemAccentColor resources at Application level, so buttons / selection / progress etc. change together
    namespace
    {
        struct AccentDef { const wchar_t* name; uint8_t r, g, b; };
        constexpr AccentDef kAccentDefs[] = {{L"main.moonlight", 0x4C, 0x8D, 0xF6}, {L"main.blood_moon", 0xD6, 0x45, 0x50}, {L"main.amber_eyes", 0xE0, 0x93, 0x1F},
                                             {L"main.aurora", 0x1F, 0xB5, 0xA0}, {L"main.forest", 0x3E, 0x9B, 0x5A},
                                             {L"main.night", 0xA9, 0xB6, 0xCC}};   // Night: a silver accent (the background is near black, see UpdateBackdrop)
        constexpr int kAccentCount = 7;  // including "follow the system"

        Windows::UI::Color Mix(AccentDef const& a, double toward, double t)  // toward: 255 = white, 0 = black
        {
            auto m = [&](uint8_t c) { return static_cast<uint8_t>(c + (toward - c) * t + 0.5); };
            return Windows::UI::Color{255, m(a.r), m(a.g), m(a.b)};
        }
    }

    namespace
    {
        struct AccentPalette { Windows::UI::Color fill, text; };

        // Change the colour of the brush itself: the controls share one brush instance, so after Color changes all places that use it follow at once (no reload needed)
        void SetBrushColor(ResourceDictionary const& dict, const wchar_t* key, Windows::UI::Color c)
        {
            auto k = box_value(hstring{key});
            if (!dict.HasKey(k)) return;
            if (auto b = dict.Lookup(k).try_as<Microsoft::UI::Xaml::Media::SolidColorBrush>()) b.Color(c);
        }

        void ApplyPalette(ResourceDictionary const& dict, AccentPalette const& dark, AccentPalette const& light)
        {
            for (auto const& kv : dict.ThemeDictionaries())
            {
                auto name = unbox_value_or<hstring>(kv.Key(), L"");
                auto sub = kv.Value().try_as<ResourceDictionary>();
                if (!sub) continue;
                const AccentPalette* pal = nullptr;
                if (name == L"Light") pal = &light;
                else if (name == L"Default" || name == L"Dark") pal = &dark;  // high contrast (HighContrast) keeps the system setting
                if (!pal) continue;
                auto alpha = [](Windows::UI::Color c, uint8_t a) { c.A = a; return c; };
                SetBrushColor(sub, L"AccentFillColorDefaultBrush", pal->fill);
                SetBrushColor(sub, L"AccentFillColorSecondaryBrush", alpha(pal->fill, 0xE6));
                SetBrushColor(sub, L"AccentFillColorTertiaryBrush", alpha(pal->fill, 0xCC));
                SetBrushColor(sub, L"AccentFillColorSelectedTextBackgroundBrush", pal->fill);
                SetBrushColor(sub, L"AccentTextFillColorPrimaryBrush", pal->text);
                SetBrushColor(sub, L"AccentTextFillColorSecondaryBrush", pal->text);
                SetBrushColor(sub, L"AccentTextFillColorTertiaryBrush", alpha(pal->text, 0xCC));
                SetBrushColor(sub, L"SystemControlHighlightAccentBrush", pal->fill);
                SetBrushColor(sub, L"SystemControlForegroundAccentBrush", pal->text);
            }
            for (auto const& m : dict.MergedDictionaries()) ApplyPalette(m, dark, light);
        }
    }

    void MainWindow::ApplyAccent()
    {
        AccentPalette dark, light;
        int idx = static_cast<int>(settings_.accent);
        if (idx >= 0 && idx < kAccentCount - 1)
        {
            auto const& a = kAccentDefs[idx];
            // As in Windows: the fill of the dark theme uses Light2, the light theme Dark1; accent text uses Light3 / Dark2
            dark = {Mix(a, 255, 0.22), Mix(a, 255, 0.50)};
            light = {Mix(a, 0, 0.20), Mix(a, 0, 0.40)};
        }
        else  // follow the system: use the accent colour family of Windows
        {
            Windows::UI::ViewManagement::UISettings ui;
            using Windows::UI::ViewManagement::UIColorType;
            dark = {ui.GetColorValue(UIColorType::AccentLight2), ui.GetColorValue(UIColorType::AccentLight3)};
            light = {ui.GetColorValue(UIColorType::AccentDark1), ui.GetColorValue(UIColorType::AccentDark2)};
        }
        ApplyPalette(Application::Current().Resources(), dark, light);
        if (lycan_app::ContrastThemeOn()) lycan_app::SetContrastTheme(true);   // the accent does not replace the contrast colours
        UpdateBackdrop();
    }

    // ---- Background: the gradient changes with the accent theme and dark / light; the start page also has the halo, stars and wolf shadow (side silhouette)
    namespace
    {
        Windows::UI::Color MixColor(Windows::UI::Color c, double toward, double t, uint8_t alpha = 255)
        {
            auto m = [&](uint8_t v) { return static_cast<uint8_t>(v + (toward - v) * t + 0.5); };
            return Windows::UI::Color{alpha, m(c.R), m(c.G), m(c.B)};
        }
        Windows::UI::Color WithAlpha(Windows::UI::Color c, uint8_t a) { c.A = a; return c; }
    }

    static Windows::UI::Color AccentBaseColor(lycan::AccentTheme t)
    {
        int idx = static_cast<int>(t);
        if (idx >= 0 && idx < kAccentCount - 1) return Windows::UI::Color{255, kAccentDefs[idx].r, kAccentDefs[idx].g, kAccentDefs[idx].b};
        return Windows::UI::ViewManagement::UISettings{}.GetColorValue(Windows::UI::ViewManagement::UIColorType::Accent);
    }

    void MainWindow::UpdateBackdrop()
    {
        using namespace Microsoft::UI::Xaml::Media;
        const bool highContrast = lycan_app::HighContrast();
        if (highContrast != contrastBackdrop_)
        {   // a contrast look is solid: no Mica behind the window, plain black under the page
            contrastBackdrop_ = highContrast;
            if (highContrast) SystemBackdrop(nullptr); else SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
            RootGrid().Background(highContrast && lycan_app::ContrastThemeOn() ? Brush{SolidColorBrush{Windows::UI::Color{255, 0, 0, 0}}} : Brush{nullptr});
        }
        const bool off = settings_.background == lycan::BackgroundStyle::Off || highContrast;
        BackdropLayer().Visibility(off ? Visibility::Collapsed : Visibility::Visible);
        if (off) return;

        const bool dark = RootGrid().ActualTheme() != ElementTheme::Light;
        const auto base = AccentBaseColor(settings_.accent);
        Windows::UI::Color top, bottom, glow;
        if (dark && settings_.accent == lycan::AccentTheme::Night)
        {
            top = Windows::UI::Color{0xF4, 0x0B, 0x0D, 0x12};      // black night: almost fully black, with only a trace of cold colour
            bottom = Windows::UI::Color{0xF4, 0x03, 0x04, 0x06};
            glow = WithAlpha(MixColor(base, 255, 0.10), 0x26);
        }
        else if (dark)
        {
            top = MixColor(base, 0, 0.74, 0xF2);       // deep night: the accent colour darkened
            bottom = MixColor(base, 0, 0.94, 0xF2);
            glow = WithAlpha(MixColor(base, 255, 0.10), 0x58);
        }
        else
        {
            top = MixColor(base, 255, 0.93, 0xEE);     // morning light: the accent colour so pale it is almost white
            bottom = MixColor(base, 255, 0.80, 0xEE);
            glow = WithAlpha(MixColor(base, 255, 0.35), 0x50);
        }
        LinearGradientBrush fill;
        fill.StartPoint({0.2, 0.0});
        fill.EndPoint({0.8, 1.0});
        GradientStop s0, s1;
        s0.Color(top);
        s0.Offset(0.0);
        s1.Color(bottom);
        s1.Offset(1.0);
        fill.GradientStops().Append(s0);
        fill.GradientStops().Append(s1);
        BgFill().Fill(fill);

        // Glow of the accent colour at the top right
        RadialGradientBrush glowBrush;
        glowBrush.Center({0.85, 0.05});
        glowBrush.GradientOrigin({0.85, 0.05});
        glowBrush.RadiusX(0.75);
        glowBrush.RadiusY(0.75);
        GradientStop g0, g1;
        g0.Color(glow);
        g0.Offset(0.0);
        g1.Color(WithAlpha(glow, 0));
        g1.Offset(1.0);
        glowBrush.GradientStops().Append(g0);
        glowBrush.GradientStops().Append(g1);
        BgGlow().Fill(glowBrush);

        // Start page scene
        const bool scene = settings_.background == lycan::BackgroundStyle::Full && view_ == View::Home && !compact_ && !loading_;
        HomeScene().Visibility(scene ? Visibility::Visible : Visibility::Collapsed);
        const bool strip = settings_.background == lycan::BackgroundStyle::Full && view_ == View::Archive && !compact_ && !loading_;
        ArchiveScene().Visibility(strip ? Visibility::Visible : Visibility::Collapsed);
        if (strip)
        {
            try { UpdateArchiveStrip(dark, base); }
            catch (winrt::hresult_error const& e) { lycan::diag::Warn(L"archive strip: {} (0x{:08x})", std::wstring(e.message()), static_cast<uint32_t>(e.code().value)); }
        }
        if (!scene) return;

        const Windows::UI::Color moon = dark ? Windows::UI::Color{255, 214, 228, 255} : WithAlpha(MixColor(base, 255, 0.55), 255);
        RadialGradientBrush halo;
        halo.Center({0.5, 0.5});
        halo.GradientOrigin({0.5, 0.5});
        halo.RadiusX(0.5);
        halo.RadiusY(0.5);
        GradientStop h0, h1, h2;
        h0.Color(WithAlpha(moon, dark ? 0x46 : 0x55));
        h0.Offset(0.0);
        h1.Color(WithAlpha(moon, dark ? 0x1C : 0x26));
        h1.Offset(0.55);
        h2.Color(WithAlpha(moon, 0));
        h2.Offset(1.0);
        halo.GradientStops().Append(h0);
        halo.GradientStops().Append(h1);
        halo.GradientStops().Append(h2);
        MoonHalo().Fill(halo);

        // Milky way band: a very flat ellipse + radial gradient, with fully transparent edges (the end points of a rectangle would show hard edges)
        RadialGradientBrush band;
        band.Center({0.5, 0.5});
        band.GradientOrigin({0.5, 0.5});
        band.RadiusX(0.5);
        band.RadiusY(0.5);
        GradientStop b0, b1, b2;
        b0.Color(WithAlpha(MixColor(base, 255, 0.7), dark ? 0x26 : 0));
        b0.Offset(0.0);
        b1.Color(WithAlpha(MixColor(base, 255, 0.7), dark ? 0x10 : 0));
        b1.Offset(0.55);
        b2.Color(WithAlpha(MixColor(base, 255, 0.7), 0));
        b2.Offset(1.0);
        band.GradientStops().Append(b0);
        band.GradientStops().Append(b1);
        band.GradientStops().Append(b2);
        MilkyBand().Fill(band);
        MilkyBand().Visibility(dark ? Visibility::Visible : Visibility::Collapsed);

        // Wolf shadow: in the dark theme a darker silhouette with a rim light in the accent colour; in the light theme a faint blue-grey
        WolfShade().Fill(SolidColorBrush{dark ? WithAlpha(MixColor(base, 0, 0.96), 0xE0) : WithAlpha(MixColor(base, 0, 0.55), 0x22)});
        WolfShade().Stroke(SolidColorBrush{dark ? WithAlpha(MixColor(base, 255, 0.15), 0x55) : WithAlpha(base, 0x30)});
        WolfShade().StrokeThickness(dark ? 3.5 : 2.5);
        WolfShade().Opacity(dark ? 0.95 : 1.0);

        // Starry sky: few stars, in different sizes, with a soft glow (many small dots of the same size look like a dirty screen).
        // Positions are sampled to keep a distance from each other so they do not clump together; a few are four-pointed flashes, the rest are soft round dots.
        if (!starsBuilt_)
        {
            starsBuilt_ = true;
            uint32_t seed = 7721005;
            auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
            std::vector<std::pair<double, double>> pts;
            for (int tries = 0; tries < 600 && pts.size() < 24; ++tries)
            {
                double x = rnd(), y = rnd() * 0.74;
                bool ok = true;
                for (auto const& q : pts) if (std::hypot(x - q.first, (y - q.second) * 1.4) < 0.11) { ok = false; break; }
                if (ok) pts.push_back({x, y});
            }
            auto addStar = [&](double size, bool sparkle, double opacity) -> double
            {
                Grid g;
                g.Width(size * (sparkle ? 2.0 : 1.0));
                g.Height(size * (sparkle ? 2.0 : 1.0));
                // soft glow
                RadialGradientBrush glow;
                glow.Center({0.5, 0.5});
                glow.GradientOrigin({0.5, 0.5});
                glow.RadiusX(0.5);
                glow.RadiusY(0.5);
                GradientStop g0, g1;
                g0.Color(Windows::UI::Color{static_cast<uint8_t>(sparkle ? 0x70 : 0xB0), 214, 228, 255});
                g0.Offset(0.0);
                g1.Color(Windows::UI::Color{0, 214, 228, 255});
                g1.Offset(1.0);
                glow.GradientStops().Append(g0);
                glow.GradientStops().Append(g1);
                Microsoft::UI::Xaml::Shapes::Ellipse halo;
                halo.Fill(glow);
                g.Children().Append(halo);
                if (sparkle)
                {
                    // four-pointed star
                    Microsoft::UI::Xaml::Shapes::Path star;
                    const double r = size, k = size * 0.14;
                    // The coordinates of a Path start at (0,0) and negative coordinates are cut off: shift the whole shape by +r and set the element size to 2r, then it is centred
                    auto n = [](double v) { return std::to_wstring(v); };
                    std::wstring d = L"M" + n(r) + L",0 L" + n(r + k) + L"," + n(r - k) + L" L" + n(2 * r) + L"," + n(r) + L" L" + n(r + k) + L"," + n(r + k) +
                                     L" L" + n(r) + L"," + n(2 * r) + L" L" + n(r - k) + L"," + n(r + k) + L" L0," + n(r) + L" L" + n(r - k) + L"," + n(r - k) + L" Z";
                    star.Width(2 * r);
                    star.Height(2 * r);
                    star.Data(Microsoft::UI::Xaml::Markup::XamlBindingHelper::ConvertValue(xaml_typename<Media::Geometry>(), box_value(hstring{d})).as<Media::Geometry>());
                    star.Fill(SolidColorBrush{Windows::UI::Color{0xF2, 245, 249, 255}});
                    star.HorizontalAlignment(HorizontalAlignment::Center);
                    star.VerticalAlignment(VerticalAlignment::Center);
                    g.Children().Append(star);
                }
                g.Opacity(opacity);
                Stars().Children().Append(g);
                return g.Width();
            };
            // the small soft stars (random places)
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const double size = 5 + rnd() * 5;
                starFrac_.push_back(pts[i]);
                starSize_.push_back(addStar(size, false, 0.35 + rnd() * 0.45));
            }
            // The big four-pointed stars draw Canis Major (the Greater Dog; Sirius is its brightest star), as in the usual
            // picture: the dog stands on its hind legs, the head at the upper right, the tail at the lower left. The
            // places are the stars of that picture (x 55..381, y 100..441), scaled to 0..1; the size follows the brightness.
            struct Pt { double x, y, size; };
            static const Pt kCanisMajor[] = {
                {320, 100, 14},   // head
                {250, 133, 20},   // the star behind the head
                {318, 185, 34},   // Sirius
                {282, 207, 11},
                {316, 247, 15},   // the raised foreleg
                {381, 260, 14},   // its paw
                {183, 262, 13},   // back
                {113, 284, 20},   // hip
                {55, 308, 14},    // tail
                {285, 303, 11},
                {150, 345, 15},   // hind leg
                {148, 394, 11},
                {264, 441, 14},   // hind foot
            };
            for (auto const& c : kCanisMajor)
                conStars_.push_back({(c.x - 55) / (381.0 - 55), (c.y - 100) / (441.0 - 100), addStar(c.size, true, 0.95)});
        }
        Stars().Visibility(dark ? Visibility::Visible : Visibility::Collapsed);  // the light theme has no stars
        LayoutHomeScene();
        LayoutArchiveStrip();
        UpdateToolbarText();
    }

    // The strip at the top of the archive view: some soft stars and the wolf, both fade out downwards (a few dozen pixels, so cheap)
    void MainWindow::UpdateArchiveStrip(bool dark, Windows::UI::Color base)
    {
        using namespace Microsoft::UI::Xaml::Media;
        if (!archiveStarsBuilt_)
        {
            archiveStarsBuilt_ = true;
            uint32_t seed = 4410217;
            auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
            for (int i = 0; i < 12; ++i)
            {
                // the same four-pointed stars as the home page, small, with a soft glow
                const double r = 4 + rnd() * 4;
                Grid g;
                g.Width(2 * r);
                g.Height(2 * r);
                RadialGradientBrush glow;
                glow.Center({0.5, 0.5});
                glow.GradientOrigin({0.5, 0.5});
                glow.RadiusX(0.5);
                glow.RadiusY(0.5);
                GradientStop g0, g1;
                g0.Color(Windows::UI::Color{0x70, 214, 228, 255});
                g0.Offset(0.0);
                g1.Color(Windows::UI::Color{0, 214, 228, 255});
                g1.Offset(1.0);
                glow.GradientStops().Append(g0);
                glow.GradientStops().Append(g1);
                Microsoft::UI::Xaml::Shapes::Ellipse halo;
                halo.Fill(glow);
                g.Children().Append(halo);
                Microsoft::UI::Xaml::Shapes::Path star;
                const double k = r * 0.14;
                auto n = [](double v) { return std::to_wstring(v); };
                const std::wstring d = L"M" + n(r) + L",0 L" + n(r + k) + L"," + n(r - k) + L" L" + n(2 * r) + L"," + n(r) + L" L" + n(r + k) + L"," + n(r + k) +
                                       L" L" + n(r) + L"," + n(2 * r) + L" L" + n(r - k) + L"," + n(r + k) + L" L0," + n(r) + L" L" + n(r - k) + L"," + n(r - k) + L" Z";
                star.Width(2 * r);
                star.Height(2 * r);
                star.Data(Microsoft::UI::Xaml::Markup::XamlBindingHelper::ConvertValue(xaml_typename<Media::Geometry>(), box_value(hstring{d})).as<Media::Geometry>());
                star.Fill(SolidColorBrush{Windows::UI::Color{0xF2, 245, 249, 255}});
                star.HorizontalAlignment(HorizontalAlignment::Center);
                star.VerticalAlignment(VerticalAlignment::Center);
                g.Children().Append(star);
                ArchiveStars().Children().Append(g);
                archiveStarFrac_.push_back({(i + 0.15 + rnd() * 0.7) / 12.0, rnd()});  // x spread over the width, y 0..1 of the strip
                archiveStarSize_.push_back(2 * r);
            }
        }
        ArchiveStars().Visibility(dark ? Visibility::Visible : Visibility::Collapsed);  // no stars in the light theme

        // the wolf: the fill fades out from the top to the bottom
        LinearGradientBrush fill;
        fill.StartPoint({0.5, 0.0});
        fill.EndPoint({0.5, 1.0});
        GradientStop a, b;
        a.Color(dark ? WithAlpha(MixColor(base, 0, 0.9), 0x70) : WithAlpha(MixColor(base, 0, 0.55), 0x30));
        a.Offset(0.0);
        b.Color(dark ? WithAlpha(MixColor(base, 0, 0.9), 0) : WithAlpha(MixColor(base, 0, 0.55), 0));
        b.Offset(1.0);
        fill.GradientStops().Append(a);
        fill.GradientStops().Append(b);
        WolfStrip().Fill(fill);
        WolfStrip().Stroke(nullptr);
        LayoutArchiveStrip();
    }

    // The buttons of the first row of the archive view: when the window is too narrow the texts of the less important ones go first
    // (icons stay; the names for the screen reader and the tips stay), then the text of "Extract here" as well.
    void MainWindow::UpdateToolbarText()
    {
        if (view_ != View::Archive) return;
        const double available = ArchivePanel().ActualWidth();
        if (available < 1) return;
        auto texts = [&](bool secondary, bool primary) {
            const Visibility v2 = secondary ? Visibility::Visible : Visibility::Collapsed;
            ExtractToText().Visibility(v2);
            AddText().Visibility(v2);
            PreviewText().Visibility(v2);
            ViewText().Visibility(v2);
            ExtractHereText().Visibility(primary ? Visibility::Visible : Visibility::Collapsed);
        };
        auto fits = [&] {
            ArchiveToolRow().Measure(Windows::Foundation::Size{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()});
            return ArchiveToolRow().DesiredSize().Width <= available;
        };
        texts(true, true);
        bool iconsOnly = false;
        if (!fits()) { texts(false, true); iconsOnly = true; }
        if (iconsOnly && !fits()) texts(false, false);
        if (iconsOnly != toolbarIconsOnly_ || iconsOnly)
        {
            toolbarIconsOnly_ = iconsOnly;
            // the tips say what the icons mean
            auto tip = [&](Control const& b, wchar_t const* id) { ToolTipService::SetToolTip(b, iconsOnly ? box_value(hstring{lycan::Tr(id)}) : nullptr); };
            tip(ExtractToButton(), L"main.extract");
            tip(PreviewButton(), L"main.preview");
            tip(ViewButton(), L"main.view");
            tip(ExtractHereButton(), L"main.extract_here");
        }
    }

    void MainWindow::LayoutArchiveStrip()
    {
        if (ArchiveScene().Visibility() != Visibility::Visible) return;
        const double w = RootGrid().ActualWidth();
        if (w < 1) return;
        const double wolfLeft = w - 28 - 96 * 1.073;   // the wolf is 1.073 times as wide as high
        for (uint32_t i = 0; i < ArchiveStars().Children().Size() && i < archiveStarFrac_.size(); ++i)
        {
            auto star = ArchiveStars().Children().GetAt(i).as<UIElement>();
            const double x = archiveStarFrac_[i].first * w, y = 4 + archiveStarFrac_[i].second * 100;
            Canvas::SetLeft(star, x - archiveStarSize_[i] / 2);
            Canvas::SetTop(star, y - archiveStarSize_[i] / 2);
            star.Opacity((1.0 - y / 120.0) * 0.9);                       // fades out downwards
            star.Visibility(x > wolfLeft - 10 ? Visibility::Collapsed : Visibility::Visible);  // none over the wolf
        }
    }

    // Lays out the halo, stars and wolf shadow by the window size
    void MainWindow::LayoutHomeScene()
    {
        if (HomeScene().Visibility() != Visibility::Visible) return;
        const double w = RootGrid().ActualWidth(), h = RootGrid().ActualHeight();
        if (w < 1 || h < 1) return;
        const double wolfH = h * 0.62;
        WolfShade().Height(wolfH);
        WolfShade().Margin(ThicknessHelper::FromLengths(0, 0, w * 0.02, h * 0.04));
        const double halo = std::max(w, h) * 0.78;
        MoonHalo().Width(halo);
        MoonHalo().Height(halo);
        // the center of the halo points at the wolf's head (right side, a little up)
        MoonHalo().Margin(ThicknessHelper::FromLengths(w * 0.74 - halo / 2, h * 0.52 - halo / 2, 0, 0));
        // Where the wolf is (its shape is about 1.065 times as wide as high, see WolfShade): no star is drawn over it
        const double wolfRight = w * 0.98, wolfBottom = h * 0.96;
        const double wolfLeft = wolfRight - wolfH * 1.065, wolfTop = wolfBottom - wolfH;
        const double pad = std::max(w, h) * 0.015;
        auto onWolf = [&](double x, double y, double r) {
            return x + r > wolfLeft - pad && x - r < wolfRight && y + r > wolfTop - pad && y - r < wolfBottom;
        };
        for (uint32_t i = 0; i < Stars().Children().Size() && i < starFrac_.size() && i < starSize_.size(); ++i)
        {
            auto star = Stars().Children().GetAt(i).as<UIElement>();
            const double x = starFrac_[i].first * w, y = starFrac_[i].second * h;
            Canvas::SetLeft(star, x - starSize_[i] / 2);
            Canvas::SetTop(star, y - starSize_[i] / 2);
            star.Visibility(onWolf(x, y, starSize_[i] / 2) ? Visibility::Collapsed : Visibility::Visible);
        }
        // Canis Major: in the upper left, away from the wolf (it shrinks when the window is narrow)
        {
            double boxH = h * 0.40;
            const double ratio = (381.0 - 55) / (441.0 - 100);
            const double left = w * 0.05, top = h * 0.13;
            if (left + boxH * ratio > wolfLeft - pad) boxH = std::max(h * 0.18, std::min(boxH, wolfTop - pad - top));  // beside the wolf's head: stay above it
            boxH = std::min(boxH, (w * 0.5 - left) / ratio);                                                          // and never wider than half of the window
            const double boxW = boxH * ratio;
            const size_t first = starFrac_.size();
            for (size_t k = 0; k < conStars_.size() && first + k < Stars().Children().Size(); ++k)
            {
                auto star = Stars().Children().GetAt(static_cast<uint32_t>(first + k)).as<UIElement>();
                Canvas::SetLeft(star, left + conStars_[k].rx * boxW - conStars_[k].size / 2);
                Canvas::SetTop(star, top + conStars_[k].ry * boxH - conStars_[k].size / 2);
            }
        }
        // A faint milky way band: one soft bright band at an angle (dark theme only)
        // The element size must not exceed the layout cell: if it does, XAML clips to the cell and after the rotation it becomes a hard edge
        MilkyBand().Width(w * 0.98);
        MilkyBand().Height(h * 0.40);
        MilkyBand().Margin(ThicknessHelper::FromLengths(0, h * 0.12, 0, 0));
    }

    void MainWindow::BuildAccentPicker()
    {
        for (int i = 0; i < kAccentCount; ++i)
        {
            StackPanel panel;
            panel.Orientation(Orientation::Horizontal);
            panel.Spacing(8);
            panel.Padding(ThicknessHelper::FromLengths(6, 4, 16, 4));
            panel.MinWidth(lycan::i18n::Current() == L"zh-TW" ? 104 : 168);  // the cells have the same width; "Follow system" is longer than the Chinese text
            Microsoft::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(22);
            dot.Height(22);
            if (i < kAccentCount - 1)
            {
                auto const& a = kAccentDefs[i];
                dot.Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, a.r, a.g, a.b}});
            }
            else
            {
                dot.Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, 120, 120, 128}});
                dot.StrokeThickness(2);
                dot.Stroke(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, 220, 220, 230}});
            }
            TextBlock label;
            label.Text(i < kAccentCount - 1 ? hstring{lycan::Tr(kAccentDefs[i].name)} : hstring{lycan::Tr(L"main.follow_system")});
            label.VerticalAlignment(VerticalAlignment::Center);
            panel.Children().Append(dot);
            panel.Children().Append(label);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(panel, label.Text());
            AccentGrid().Items().Append(panel);
        }
    }

    void MainWindow::OnAccentChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (loadingSettings_) return;
        int i = AccentGrid().SelectedIndex();
        if (i < 0) return;
        settings_.accent = static_cast<lycan::AccentTheme>(i);
        SaveSettings();
        ApplyAccent();
    }

    // When the default view setting changes, apply it to the archive that is open now too
    void MainWindow::ApplyViewSettings()
    {
        if (!tree_ || loadingSettings_) return;
        bool changed = flat_ != settings_.browseFlat || viewMode_ != settings_.viewMode;
        flat_ = settings_.browseFlat;
        viewMode_ = settings_.viewMode;
        treeWanted_ = settings_.showFolderTree;
        syncingUi_ = true;
        FlatToggle().IsChecked(flat_);
        TreeToggle().IsChecked(treeWanted_);
        syncingUi_ = false;
        UpdateTreeVisibility();
        if (changed) RefreshList();
    }

    void MainWindow::OnSettingComboChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) { ReadSettingsFromUi(); }
    void MainWindow::OnSettingToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { ReadSettingsFromUi(); }
    void MainWindow::OnSettingNumberChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) { ReadSettingsFromUi(); }

    void MainWindow::OnSettingsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (view_ == View::Settings) { OnSettingsBackClick(nullptr, nullptr); return; }
        viewBeforeSettings_ = view_;
        std::wstring engine = lycan::Tr(L"main.compression_engine_7_zip");
        try
        {
            auto dll = lycan::SevenZipDllPath();
            auto v = FileVersionOf(dll);
            engine += (v.empty() ? L"" : L" " + v) + lycan::Tf(L"fmt.paren", dll.wstring());
        }
        catch (...) { engine += lycan::Tr(L"main.7z_dll_not_found"); }
        AboutVersion().Text(L"LycanZip " LYCAN_VERSION_WSTRING);
        AboutEngine().Text(hstring{engine});
        AboutSettingsPath().Text(hstring{lycan::Tf(L"main.settings_file", settingsPath_.wstring())});
        loadingSettings_ = true;
        ShellMenuToggle().IsOn(lycan::IsShellMenuRegistered(ShellOptions()));
        FileAssocToggle().IsOn(lycan::IsFileAssociationsRegistered(AssocOptions()));
        loadingSettings_ = false;
        RebuildShellPresetRows();
        RefreshDataLocationUi();
        RefreshFileAssocStatus();
        RefreshWin11MenuAsync();
        ShowView(View::Settings);
    }

    // Tests (LYCAN_TEST_REG_ROOT=Software\\LycanZipTest_x) keep the menu and file association registrations under a key of their own
    static std::wstring TestRegistryRoot()
    {
        wchar_t buf[256];
        const DWORD n = GetEnvironmentVariableW(L"LYCAN_TEST_REG_ROOT", buf, static_cast<DWORD>(std::size(buf)));
        return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
    }

    lycan::FileAssocOptions MainWindow::AssocOptions()
    {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        lycan::FileAssocOptions o;
        o.exe = std::wstring(exe, n);
        if (const std::wstring root = TestRegistryRoot(); !root.empty()) { o.classesKey = root + L"\\Classes"; o.softwareKey = root + L"\\Software"; o.notifyShell = false; }
        return o;
    }

    // Shows which program opens a few common extensions now (read-only query)
    // The Store version (a package with an identity): the manifest declares the Windows 11 menu and the file types, so they are always on and cannot be
    // switched; the registry of a packaged program is virtualized, so the classic menu cannot be registered from it either. (Tests: LYCAN_TEST_STORE_UI=1)
    void MainWindow::ApplyStoreChannelUi()
    {
        if (!IsStoreUi())
        {
            // another copy while the Store version is installed: the menu and the file types are the Store version's, these switches stay off
            if (!lycan::IsStorePackageInstalled()) { OtherCopyNote().Visibility(Visibility::Collapsed); return; }
            OtherCopyNoteText().Text(lycan::Tr(L"main.store_version_provides_menu"));
            OtherCopyNote().Visibility(Visibility::Visible);
            const bool was = loadingSettings_;
            loadingSettings_ = true;
            for (auto t : {ShellMenuToggle(), FileAssocToggle(), Win11MenuToggle()}) { t.IsOn(false); t.IsEnabled(false); }
            loadingSettings_ = was;
            return;
        }
        const std::wstring other = OtherCopyLocation();
        OtherCopyNoteText().Text(hstring{lycan::Tf(L"main.other_copy_note", other)});
        OtherCopyNote().Visibility(other.empty() ? Visibility::Collapsed : Visibility::Visible);
        StoreExplorerNote().Visibility(Visibility::Visible);
        UpdatesHeader().Visibility(Visibility::Collapsed);   // the Store updates the program: no check for new versions here
        UpdatesCard().Visibility(Visibility::Collapsed);
        // The menus and the file types are part of the package (Windows adds them at install and removes them at uninstall): nothing to register,
        // so the cards that register are hidden. The menu switch stays: it is a setting that the menu DLL reads (shell_menu), and the DLL then shows
        // nothing (the new Windows 11 menu, Show more options, the classic menu of Windows 10, the right-drag menu). Explorer reads it live.
        FileAssocCard().Visibility(Visibility::Collapsed);
        Win11MenuCard().Visibility(Visibility::Collapsed);
        RestartExplorerCard().Visibility(Visibility::Collapsed);
        ShellMenuHint().Text(lycan::Tr(L"main.store_menu_switch_hint"));
        const bool was = loadingSettings_;
        loadingSettings_ = true;
        ShellMenuToggle().IsEnabled(true);
        ShellMenuToggle().IsOn(settings_.shellMenu);
        loadingSettings_ = was;
        OpenDefaultAppsButton().IsEnabled(true);
    }

    bool MainWindow::IsStoreUi()
    {
        return lycan::DetectChannel() == lycan::InstallChannel::Store || GetEnvironmentVariableW(L"LYCAN_TEST_STORE_UI", nullptr, 0) > 0;
    }

    // A copy that is not the Store version, while the Store version is installed: remove its own right-click menu and file associations (they are
    // LycanZip's keys under HKCU; the Store version's are in its package), so that File Explorer shows LycanZip once. Returns true when the Store
    // version is installed. The Windows 11 menu package of such a copy is removed by CheckInstalledCopiesAsync (the package manager is slow).
    bool MainWindow::StepAsideForStore()
    {
        if (lycan::GetDataLocation().packaged || !lycan::IsStorePackageInstalled()) return false;
        std::wstring error;
        if (lycan::IsShellMenuRegistered(ShellOptions()))
        {
            if (lycan::UnregisterShellMenu(ShellOptions(), &error)) lycan::diag::Info(L"the Store version is installed: the right-click menu of this copy was removed");
            else lycan::diag::Warn(L"the Store version is installed, the right-click menu of this copy could not be removed: {}", error);
        }
        if (lycan::IsFileAssociationsRegistered(AssocOptions()))
        {
            if (lycan::UnregisterFileAssociations(AssocOptions(), &error)) lycan::diag::Info(L"the Store version is installed: the file associations of this copy were removed");
            else lycan::diag::Warn(L"the Store version is installed, the file associations of this copy could not be removed: {}", error);
        }
        return true;
    }

    // The Store version: where another copy is that still has the classic right-click menu or the file associations registered (empty: none). A package
    // reads the real HKCU but writes to a private copy of it: what an earlier Store version registered itself (pointing into WindowsApps) is only in
    // that copy, and is removed here instead of being reported.
    std::wstring MainWindow::OtherCopyLocation()
    {
        auto inPackage = [](fs::path const& p) {
            std::wstring s = p.wstring();
            std::transform(s.begin(), s.end(), s.begin(), ::towlower);
            return s.find(L"\\windowsapps\\") != std::wstring::npos;
        };
        std::wstring where;
        if (lycan::IsShellMenuRegistered(ShellOptions()))
        {
            const fs::path dll = lycan::RegisteredShellDll(ShellOptions());
            if (inPackage(dll)) lycan::UnregisterShellMenu(ShellOptions());
            else
            {
                // an installed copy keeps the DLL outside the folder of the program and writes where the program is next to it (lycan::EnsureShellHome)
                std::ifstream f(dll.parent_path() / L"LycanZip.exepath", std::ios::binary);
                std::string utf8((std::istreambuf_iterator<char>(f)), {});
                while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' ')) utf8.pop_back();
                where = utf8.empty() ? dll.parent_path().wstring() : fs::path(std::wstring{winrt::to_hstring(utf8)}).parent_path().wstring();
            }
        }
        if (lycan::IsFileAssociationsRegistered(AssocOptions()))
        {
            const fs::path exe = lycan::RegisteredAssocExe(AssocOptions());
            if (inPackage(exe)) lycan::UnregisterFileAssociations(AssocOptions());
            else if (where.empty()) where = exe.parent_path().wstring();
        }
        return where;
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::TwoCopiesDialogAsync(std::wstring text)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.two_copies_title"), L"");
        TextBlock tb;
        tb.Text(hstring{text});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.IsTextSelectionEnabled(true);
        tb.MaxWidth(460);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.open_installed_apps"));
        dlg.CloseButtonText(lycan::Tr(L"main.ok"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary)
            ShellExecuteW(nullptr, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // The program was started on its own: two copies on this PC (said once), else the registrations that point to another folder
    winrt::fire_and_forget MainWindow::CheckInstalledCopiesAsync()
    {
        auto strong = get_strong();
        if (GetEnvironmentVariableW(L"LYCAN_NO_STALE_CHECK", nullptr, 0) > 0) co_return;  // tests: the real registrations belong to other builds
        const lycan::DataLocation loc = lycan::GetDataLocation();
        if (loc.packaged)
        {
            const std::wstring other = OtherCopyLocation();
            if (other.empty() || settings_.twoCopiesNoticeShown) co_return;
            settings_.twoCopiesNoticeShown = true;
            SaveSettings();
            co_await TwoCopiesDialogAsync(lycan::Tf(L"main.two_copies_other_has_menu", other));
            co_return;
        }
        if (!StepAsideForStore()) { CheckStaleRegistrationsAsync(); co_return; }
        if (TestRegistryRoot().empty())   // the Windows 11 menu package of a copy like this one (the package is not part of the test registry key)
        {
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            const lycan_app::Win11MenuRegistration w11 = lycan_app::QueryWin11MenuRegistration();
            if (w11.found)
            {
                const hstring failure = co_await lycan_app::UnregisterWin11MenuAsync();
                if (failure.empty()) lycan::diag::Info(L"the Store version is installed: the Windows 11 menu package of this copy was removed");
                else lycan::diag::Warn(L"the Store version is installed, the Windows 11 menu package could not be removed: {}", std::wstring{failure});
            }
            co_await ui;
        }
        RefreshFileAssocStatus();
        if (settings_.twoCopiesNoticeShown) co_return;
        settings_.twoCopiesNoticeShown = true;
        SaveSettings();
        co_await TwoCopiesDialogAsync(lycan::Tf(L"main.two_copies_store_owns", loc.exeDir.wstring()));
    }

    void MainWindow::RefreshFileAssocStatus()
    {
        // the Store version: the file types are part of the package, always registered (its HKCU view does not show them)
        bool registered = IsStoreUi() || lycan::IsFileAssociationsRegistered(AssocOptions());
        std::wstring text;
        for (auto ext : {L".zip", L".7z", L".rar", L".tar.gz"})
        {
            std::wstring e = ext;
            if (e == L".tar.gz") e = L".gz";  // the association follows the last extension
            std::wstring name = lycan::CurrentDefaultAppName(e.c_str());
            if (!text.empty()) text += L"\n";
            text += L"• " + std::wstring(ext) + std::wstring{lycan::Tr(L"fmt.gap")} + (name.empty() ? lycan::Tr(L"main.not_set") : name);
            if (lycan::IsLycanZipDefaultFor(e.c_str())) text += lycan::Tr(L"main.already_lycanzip");
        }
        if (!registered) text += lycan::Tr(L"main.not_registered_yet_turn_on_switch");
        AssocStatus().Text(hstring{text});
        OpenDefaultAppsButton().IsEnabled(registered);
        ApplyStoreChannelUi();
    }

    void MainWindow::OnFileAssocToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        bool want = FileAssocToggle().IsOn();
        std::wstring error;
        bool ok = want ? lycan::RegisterFileAssociations(AssocOptions(), &error) : lycan::UnregisterFileAssociations(AssocOptions(), &error);
        if (!ok)
        {
            loadingSettings_ = true;
            FileAssocToggle().IsOn(!want);
            loadingSettings_ = false;
            ShowMessage(lycan::Tr(L"main.cannot_update_file_associations"), hstring{error});
        }
        RefreshFileAssocStatus();
        if (ok && want)
            ShowMessage(lycan::Tr(L"main.registered"), lycan::Tr(L"main.lycanzip_can_now_handle_archives"));
    }

    void MainWindow::OnOpenDefaultAppsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!lycan::OpenDefaultAppsSettings()) ShowMessage(lycan::Tr(L"main.cannot_open_settings"), lycan::Tr(L"main.please_open_settings_apps_default"));
    }

    lycan::ShellRegOptions MainWindow::ShellOptions()
    {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        lycan::ShellRegOptions o;
        o.exe = std::wstring(exe, n);
        if (const std::wstring root = TestRegistryRoot(); !root.empty()) o.classesKey = root + L"\\Classes";
        return o;
    }

    // ---- Which entries the context menu shows (settings page, "Explorer" tab). The menu reads the setting itself, so
    // nothing is re-registered: a change shows the next time the menu opens.

    namespace
    {
        struct ShellItemLabel { const char* id; std::wstring label; };
        // The text of each entry (translated); the order is the order of the menu
        std::vector<ShellItemLabel> FileItems()
        {
            return {{"add", lycan::Tr(L"main.menu_add_to_archive")}, {"fmt1", lycan::Tr(L"main.compress_name_format")}, {"fmt2", lycan::Tr(L"main.compress_name_format")},
                    {"each", lycan::Tr(L"main.compress_each_separately_when")}, {"template", lycan::Tr(L"main.compress_with_preset")},
                    {"more", lycan::Tr(L"main.other_formats_tar_gz_tar_xz")}, {"encrypt", lycan::Tr(L"main.encrypted_archive")},
                    {"opencontent", lycan::Tr(L"main.open_contents_with_lycanzip_exe")},
                    {"hash", lycan::Tr(L"main.hash_values_menu_files_archives")}, {"hashsave", lycan::Tr(L"sums.menu_hash_save")}};
        }
        std::vector<ShellItemLabel> ArchiveItems()
        {
            return {{"open", lycan::Tr(L"main.open")}, {"here", lycan::Tr(L"main.menu_extract_here")}, {"folder", lycan::Tr(L"main.extract_folders_same_name")},
                    {"to", lycan::Tr(L"main.extract")}, {"recent", lycan::Tr(L"main.extract_recent_folder")},
                    {"merge", lycan::Tr(L"main.extract_all_into_one_folder_when")}, {"test", lycan::Tr(L"main.test_integrity")},
                    {"images", lycan::Tr(L"main.image_preview")}, {"audio", lycan::Tr(L"main.audio_preview")}};
        }
        const lycan::ArchiveType kShellFormats[] = {lycan::ArchiveType::Zip, lycan::ArchiveType::SevenZip, lycan::ArchiveType::TarGz,
                                                    lycan::ArchiveType::TarXz, lycan::ArchiveType::TarBz2};
    }

    void MainWindow::BuildShellItemsUi()
    {
        auto panel = ShellItemsPanel();
        panel.Children().Clear();
        shellChecks_.clear();
        auto weak = get_weak();

        auto title = TextBlock{};
        title.Text(lycan::Tr(L"main.context_menu_entries"));
        title.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
        title.Margin(ThicknessHelper::FromLengths(0, 16, 0, 0));
        panel.Children().Append(title);
        panel.Children().Append(Caption(lycan::Tr(L"main.choose_which_entries_shown_change"), 0.65));

        auto makeFormatCombo = [&](bool second) {
            ComboBox combo;
            combo.MinWidth(110);
            for (auto t : kShellFormats)
            {
                ComboBoxItem item;
                item.Content(box_value(hstring{t == lycan::ArchiveType::Zip ? L"ZIP" : lycan::FormatName(t)}));
                combo.Items().Append(item);
            }
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(combo, second ? lycan::Tr(L"main.format_second_compress_entry") : lycan::Tr(L"main.format_first_compress_entry"));
            combo.SelectionChanged([weak, second](auto&&, auto&&) {
                auto s = weak.get();
                if (!s || s->loadingSettings_) return;
                auto& combo = second ? s->shellFmt2_ : s->shellFmt1_;
                const int i = combo.SelectedIndex();
                if (i < 0 || i >= static_cast<int>(std::size(kShellFormats))) return;
                (second ? s->settings_.shellFormat2 : s->settings_.shellFormat1) = kShellFormats[i];
                s->SaveSettings();
            });
            return combo;
        };

        auto addGroup = [&](const wchar_t* heading, std::vector<ShellItemLabel> const& items) {
            panel.Children().Append(Caption(heading, 0.75));
            StackPanel rows;
            rows.Spacing(2);
            for (auto const& item : items)
            {
                const std::string id = item.id;
                Grid row;
                row.ColumnSpacing(12);
                ColumnDefinition c0, c1;
                c0.Width(GridLength{1, GridUnitType::Star});
                c1.Width(GridLength{0, GridUnitType::Auto});
                row.ColumnDefinitions().Append(c0);
                row.ColumnDefinitions().Append(c1);
                CheckBox box;
                const bool isFormat = id == "fmt1" || id == "fmt2";
                box.Content(box_value(hstring{item.label}));
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, id == "fmt1" ? lycan::Tr(L"main.compress_first_format") : id == "fmt2" ? lycan::Tr(L"main.compress_second_format") : item.label);
                box.Click([weak, id](IInspectable const& sender, RoutedEventArgs const&) {
                    auto s = weak.get();
                    if (!s || s->loadingSettings_) return;
                    s->settings_.shellItems = lycan::ShellItemsWith(s->settings_.shellItems, id, sender.as<CheckBox>().IsChecked().Value());
                    s->SaveSettings();
                });
                row.Children().Append(box);
                shellChecks_.emplace_back(id, box);
                if (isFormat)
                {
                    ComboBox combo = makeFormatCombo(id == "fmt2");
                    (id == "fmt2" ? shellFmt2_ : shellFmt1_) = combo;
                    Grid::SetColumn(combo, 1);
                    row.Children().Append(combo);
                }
                rows.Children().Append(row);
            }
            Border card;
            card.Style(RootGrid().Resources().Lookup(box_value(L"SettingCard")).as<Style>());
            card.Child(rows);
            panel.Children().Append(card);
        };
        addGroup(lycan::Tr(L"main.files_folders"), FileItems());
        addGroup(lycan::Tr(L"main.archives"), ArchiveItems());
        {
            panel.Children().Append(Caption(lycan::Tr(L"main.templates_compress_with_template"), 0.75));
            shellPresetRows_ = StackPanel{};
            shellPresetRows_.Spacing(2);
            Border card;
            card.Style(RootGrid().Resources().Lookup(box_value(L"SettingCard")).as<Style>());
            card.Child(shellPresetRows_);
            panel.Children().Append(card);
            RebuildShellPresetRows();
        }
        UpdateShellItemsUi();
    }

    // Is the template listed in the Explorer menu "Compress with a preset"? (the built-in ones: settings.ini; the others: presets.ini)
    bool MainWindow::PresetInMenu(std::wstring const& name, bool builtin)
    {
        if (builtin)
        {
            for (size_t i = 0; i < lycan::kBuiltinPresetCount; ++i)
                if (name == lycan::kBuiltinPresetNames[i]) return lycan::BuiltinPresetEnabled(settings_.shellPresets, i);
            return false;
        }
        for (auto const& u : cUser_) if (u.name == name) return u.inMenu;
        for (auto const& u : lycan::LoadPresets(lycan::DefaultPresetsPath())) if (u.name == name) return u.inMenu;
        return true;
    }

    void MainWindow::SetPresetInMenu(std::wstring const& name, bool builtin, bool on)
    {
        if (builtin)
        {
            for (size_t i = 0; i < lycan::kBuiltinPresetCount; ++i)
                if (name == lycan::kBuiltinPresetNames[i]) settings_.shellPresets = lycan::BuiltinPresetsWith(settings_.shellPresets, i, on);
            SaveSettings();
            return;
        }
        auto list = lycan::LoadPresets(lycan::DefaultPresetsPath());
        for (auto& u : list) if (u.name == name) u.inMenu = on;
        lycan::SavePresets(lycan::DefaultPresetsPath(), list);
        for (auto& u : cUser_) if (u.name == name) u.inMenu = on;  // the compress page keeps its own copy
        if (!cBase_.builtin && cBase_.name == name) cBase_.inMenu = on;
    }

    // The check boxes of the templates on the settings page (the list of the user's templates can change, so it is built again
    // whenever the page is opened)
    void MainWindow::RebuildShellPresetRows()
    {
        if (!shellPresetRows_) return;
        shellPresetRows_.Children().Clear();
        auto weak = get_weak();
        auto addRow = [&](std::wstring const& name, std::wstring const& label, bool builtin) {
            CheckBox box;
            box.Content(box_value(hstring{label}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, hstring{label});
            const bool was = loadingSettings_;
            loadingSettings_ = true;
            box.IsChecked(PresetInMenu(name, builtin));
            loadingSettings_ = was;
            box.Click([weak, name, builtin](IInspectable const& sender, RoutedEventArgs const&) {
                auto s = weak.get();
                if (!s || s->loadingSettings_) return;
                s->SetPresetInMenu(name, builtin, sender.as<CheckBox>().IsChecked().Value());
            });
            shellPresetRows_.Children().Append(box);
        };
        for (auto const* n : lycan::kBuiltinPresetNames) addRow(n, lycan::Tr(lycan::BuiltinPresetNameId(n)), true);
        for (auto const& u : lycan::LoadPresets(lycan::DefaultPresetsPath())) addRow(u.name, u.name, false);
    }

    void MainWindow::UpdateShellItemsUi()
    {
        const bool was = loadingSettings_;
        loadingSettings_ = true;  // setting the controls from code must not write back
        for (auto& [id, box] : shellChecks_) box.IsChecked(lycan::ShellItemEnabled(settings_.shellItems, id));
        auto select = [](ComboBox const& combo, lycan::ArchiveType t) {
            if (!combo) return;
            for (size_t i = 0; i < std::size(kShellFormats); ++i) if (kShellFormats[i] == t) combo.SelectedIndex(static_cast<int>(i));
        };
        select(shellFmt1_, settings_.shellFormat1);
        select(shellFmt2_, settings_.shellFormat2);
        loadingSettings_ = was;
    }

    void MainWindow::OnShellMenuToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        bool want = ShellMenuToggle().IsOn();
        if (IsStoreUi())   // the Store version: a setting that the menu DLL reads, no registration (ApplyStoreChannelUi)
        {
            settings_.shellMenu = want;
            SaveSettings();
            return;
        }
        std::wstring error;
        bool ok = want ? lycan::RegisterShellMenu(ShellOptions(), &error) : lycan::UnregisterShellMenu(ShellOptions(), &error);
        if (!ok)
        {
            loadingSettings_ = true;
            ShellMenuToggle().IsOn(!want);  // restore the state of the switch
            loadingSettings_ = false;
            ShowMessage(lycan::Tr(L"main.cannot_update_right_click_menu"), hstring{error});
        }
    }

    // ---- The welcome window: the first time the program is started on its own (see Welcome.h)
    bool MainWindow::MaybeShowWelcome()
    {
        const bool forced = GetEnvironmentVariableW(L"LYCAN_FORCE_WELCOME", nullptr, 0) > 0;
        if (!forced && (settings_.welcomeDone || GetEnvironmentVariableW(L"LYCAN_NO_WELCOME", nullptr, 0) > 0)) return false;
        if (lycan_app::WelcomeIsOpen()) return true;
        ShowWelcomeAsync();
        return true;
    }

    winrt::fire_and_forget MainWindow::ShowWelcomeAsync()
    {
        auto strong = get_strong();
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        const lycan_app::Win11MenuState state = lycan_app::QueryWin11Menu();
        co_await ui;
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        const bool available = lycan_app::Win11MenuPackageAvailable(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path()));

        lycan_app::WelcomeHost host;
        const lycan::Settings original = settings_;
        host.settings = settings_;
        for (int i = 0; i < kAccentCount - 1; ++i)
            host.accents.push_back({lycan::Tr(kAccentDefs[i].name), Windows::UI::Color{255, kAccentDefs[i].r, kAccentDefs[i].g, kAccentDefs[i].b}, false});
        host.accents.push_back({lycan::Tr(L"main.follow_system"), Windows::UI::Color{255, 120, 120, 128}, true});
        host.win11Possible = state != lycan_app::Win11MenuState::Unsupported && available;
        host.win11Note = state == lycan_app::Win11MenuState::Unsupported ? lycan::Tr(L"main.pc_not_running_windows_11_there") : lycan::Tr(L"main.version_has_no_signed_menu_package");
        if (lycan::GetDataLocation().packaged || GetEnvironmentVariableW(L"LYCAN_TEST_STORE_UI", nullptr, 0) > 0)
        {
            host.integrationNote = lycan::Tr(L"main.status_built_in");
            host.integrationOn = true;
            host.menuSwitchNote = lycan::Tr(L"main.store_menu_switch_hint");
        }
        else if (lycan::IsStorePackageInstalled()) host.integrationNote = lycan::Tr(L"main.store_version_provides_menu");
        auto weak = get_weak();
        host.preview = [weak](lycan::Settings const& s) {
            auto self = weak.get();
            if (!self) return;
            self->settings_.theme = s.theme;
            self->settings_.accent = s.accent;
            self->settings_.animations = s.animations;
            self->settings_.soundEffects = s.soundEffects;
            self->settings_.soundSet = s.soundSet;
            self->ApplySettings();
            self->ApplyAccent();
        };
        host.playSample = [weak] { if (auto self = weak.get()) self->PlaySfx(L"done"); };
        host.finish = [weak, original](lycan::Settings const& s, bool classic, bool win11, bool assoc) {
            if (auto self = weak.get()) self->ApplyWelcomeAsync(s, original, classic, win11, assoc);
        };
        host.skipped = [weak, original] {
            auto self = weak.get();
            if (!self) return;
            // nothing is kept of what was tried: back to the settings of before, and the window is not shown again
            self->settings_.theme = original.theme;
            self->settings_.accent = original.accent;
            self->settings_.animations = original.animations;
            self->settings_.soundEffects = original.soundEffects;
            self->settings_.soundSet = original.soundSet;
            self->settings_.welcomeDone = true;
            self->ApplySettings();
            self->ApplyAccent();
            self->SaveSettings();
            self->ShowMessage(lycan::Tr(L"main.setup_skipped"), lycan::Tr(L"main.you_can_change_everything_later"));
            self->CheckInstalledCopiesAsync();
        };
        lycan_app::ShowWelcome(std::move(host));
    }

    winrt::fire_and_forget MainWindow::ApplyWelcomeAsync(lycan::Settings s, lycan::Settings original, bool classicMenu, bool win11Menu, bool fileAssoc)
    {
        auto strong = get_strong();
        s.welcomeDone = true;
        settings_ = s;
        SaveSettings();
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
        std::wstring failed;
        std::wstring error;
        if (classicMenu && !lycan::IsShellMenuRegistered(ShellOptions()) && !lycan::RegisterShellMenu(ShellOptions(), &error)) failed += std::wstring{lycan::Tr(L"main.right_click_menu")} + lycan::Tf(L"fmt.colon_value", error) + L"\n";
        error.clear();
        if (fileAssoc && !lycan::IsFileAssociationsRegistered(AssocOptions()) && !lycan::RegisterFileAssociations(AssocOptions(), &error)) failed += std::wstring{lycan::Tr(L"main.file_associations")} + lycan::Tf(L"fmt.colon_value", error) + L"\n";
        if (win11Menu)
        {
            wchar_t exe[MAX_PATH * 2];
            const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
            hstring e = co_await lycan_app::RegisterWin11MenuAsync(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path()));
            if (!e.empty()) failed += std::wstring{lycan::Tr(L"main.windows_11_right_click_menu")} + lycan::Tf(L"fmt.colon_value", std::wstring{e}) + L"\n";
        }
        RefreshFileAssocStatus();
        RefreshWin11MenuAsync();
        loadingSettings_ = true;
        ShellMenuToggle().IsOn(lycan::IsShellMenuRegistered(ShellOptions()));
        FileAssocToggle().IsOn(lycan::IsFileAssociationsRegistered(AssocOptions()));
        loadingSettings_ = false;
        if (!failed.empty()) ShowMessage(lycan::Tr(L"main.some_things_could_not_be_set"), hstring{failed + lycan::Tr(L"main.you_can_try_again_on_settings")});
        else if (_wcsicmp(s.language.c_str(), original.language.c_str()) != 0) ShowMessage(lycan::Tr(L"main.language"), lycan::Tr(L"main.language_changes_after_lycanzip"));
        CheckInstalledCopiesAsync();   // was left out while the welcome window was up: two copies on this PC is said now, not at the next start (dialogs wait for each other)
    }

    // The Windows 11 menu toggle: the state comes from the package manager (asked on a background thread), and the toggle cannot be
    // switched on where it cannot work (not Windows 11, or no menu package next to the program); it can always be switched off
    winrt::fire_and_forget MainWindow::RefreshWin11MenuAsync()
    {
        auto strong = get_strong();
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        const lycan_app::Win11MenuState state = lycan_app::QueryWin11Menu();
        co_await ui;
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        const bool available = lycan_app::Win11MenuPackageAvailable(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path()));
        const bool registered = state == lycan_app::Win11MenuState::Registered;
        loadingSettings_ = true;
        Win11MenuToggle().IsOn(registered);
        loadingSettings_ = false;
        Win11MenuToggle().IsEnabled(registered || (state != lycan_app::Win11MenuState::Unsupported && available));
        // a card that can never be switched on (Windows 10, or a version without a signed menu package: the zip and installer versions) is not shown;
        // the FAQ of the web site says why. A package registered earlier keeps it, so that it can be switched off.
        Win11MenuCard().Visibility(registered || (state != lycan_app::Win11MenuState::Unsupported && available) ? Visibility::Visible : Visibility::Collapsed);
        Win11MenuStatus().Text(state == lycan_app::Win11MenuState::Unsupported ? lycan::Tr(L"main.pc_not_running_windows_11_there")
                               : registered ? lycan::Tr(L"main.status_on")
                               : available ? lycan::Tr(L"main.status_off")
                                           : lycan::Tr(L"main.version_has_no_signed_menu_package"));
        ApplyStoreChannelUi();
    }

    winrt::fire_and_forget MainWindow::OnWin11MenuToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_ || win11MenuBusy_) co_return;
        auto strong = get_strong();
        const bool want = Win11MenuToggle().IsOn();
        win11MenuBusy_ = true;
        Win11MenuToggle().IsEnabled(false);
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        hstring error = want ? co_await lycan_app::RegisterWin11MenuAsync(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path())) : co_await lycan_app::UnregisterWin11MenuAsync();
        win11MenuBusy_ = false;
        if (!error.empty()) ShowMessage(want ? lycan::Tr(L"main.could_not_add_windows_11_context") : lycan::Tr(L"main.could_not_remove_windows_11"), error);
        else if (want) ShowMessage(lycan::Tr(L"main.added"), lycan::Tr(L"main.if_menu_does_not_show_up"));
        RefreshWin11MenuAsync();
    }

    static bool SamePath(fs::path const& a, fs::path const& b)
    {
        return !a.empty() && !b.empty() && _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
    }

    namespace
    {
        // Ends the processes of File Explorer of this session. Windows starts the shell again by itself after a moment; when it did not
        // within a few seconds it is started here (explorer.exe opens a folder window when a shell runs already, so only when none does).
        void RestartExplorerProcesses()
        {
            DWORD session = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &session);
            auto explorerPids = [&]() {
                std::vector<DWORD> pids;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap == INVALID_HANDLE_VALUE) return pids;
                PROCESSENTRY32W e{sizeof(e)};
                for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
                {
                    DWORD s = 0;
                    if (_wcsicmp(e.szExeFile, L"explorer.exe") == 0 && ProcessIdToSessionId(e.th32ProcessID, &s) && s == session) pids.push_back(e.th32ProcessID);
                }
                CloseHandle(snap);
                return pids;
            };
            for (DWORD pid : explorerPids())
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid))
                {
                    TerminateProcess(h, 1);
                    WaitForSingleObject(h, 3000);
                    CloseHandle(h);
                }
            for (int i = 0; i < 20; ++i)   // up to 5 seconds
            {
                Sleep(250);
                if (!explorerPids().empty()) return;
            }
            ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    // "Restart File Explorer": a loaded context menu DLL is only replaced when Explorer starts again (after an update, or after the menu was switched on / off). It closes
    // the folder windows and the taskbar goes away for a moment, so it is asked first.
    winrt::fire_and_forget MainWindow::RestartExplorerAsync()
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.restart_explorer_title"), L"");
        TextBlock tb;
        tb.Text(lycan::Tr(L"main.restart_explorer_question"));
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(460);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.restart_explorer_do"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        lycan::diag::Info(L"restarting File Explorer");
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        RestartExplorerProcesses();
        co_await ui;
    }

    void MainWindow::OnRestartExplorerClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        RestartExplorerAsync();
    }

    // After the program was updated or moved: the context menus and the file associations that the user switched on have to point to this version.
    // - The same folder, an older menu DLL (a zip unpacked over the old one, winget, Scoop): the menu DLL has the version in its name, so the new
    //   one is a new file; the registration is pointed to it without a question (the user switched the menu on), the old DLLs are deleted when
    //   Explorer does not hold them any more, and a notice offers to restart Explorer.
    // - Another folder (a portable folder was moved or copied): ask, because it may be a copy that the user does not want to use.
    // Nothing is registered that the user has not switched on before.
    winrt::fire_and_forget MainWindow::CheckStaleRegistrationsAsync()
    {
        auto strong = get_strong();
        if (GetEnvironmentVariableW(L"LYCAN_NO_STALE_CHECK", nullptr, 0) > 0) co_return;  // tests: the real registrations point to another build
        wchar_t exeBuf[MAX_PATH * 2];
        const fs::path exe = std::wstring(exeBuf, GetModuleFileNameW(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf))));
        const fs::path exeDir = exe.parent_path();
        // where the menu DLL and the Windows 11 package are (the folder of the exe, or outside of an installed copy). Nothing is copied there
        // unless a registration needs it (RegisterShellMenu and EnsureShellHome below): a copy that never switched the menu on leaves no files.
        const fs::path dir = lycan::ShellHome(exeDir);
        const fs::path dllNow = lycan::ShellDllPath(dir);

        // ---- the same folder: update the registrations
        bool classicUpdate = false, win11Update = false, blocked = false;
        std::vector<std::pair<hstring, hstring>> failures;   // title, reason
        fs::path registeredDll;
        if (lycan::IsShellMenuRegistered(ShellOptions()))
        {
            registeredDll = lycan::RegisteredShellDll(ShellOptions());
            classicUpdate = !SamePath(registeredDll, dllNow) && SamePath(registeredDll.parent_path(), dir);
        }
        if (classicUpdate)
        {
            std::wstring error;
            if (lycan::RegisterShellMenu(ShellOptions(), &error)) registeredDll = dllNow;
            else { failures.push_back({lycan::Tr(L"main.cannot_update_right_click_menu"), hstring{error}}); blocked = true; }
        }
        lycan_app::Win11MenuRegistration w11;
        if (TestRegistryRoot().empty())  // (the package is not part of the test registry key)
        {
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            w11 = lycan_app::QueryWin11MenuRegistration();
            co_await ui;
        }
        const bool w11Here = w11.found && SamePath(fs::path(w11.location), dir);
        if (w11Here && (w11.major != LYCAN_VERSION_MAJOR || w11.minor != LYCAN_VERSION_MINOR || w11.build != LYCAN_VERSION_PATCH))
        {
            lycan::EnsureShellHome(exeDir);
            if (!lycan_app::Win11MenuPackageAvailable(dir)) blocked = true;   // nothing to register the new version with: the old DLL has to stay
            else
            {
                hstring failure = co_await lycan_app::UnregisterWin11MenuAsync();
                if (failure.empty()) failure = co_await lycan_app::RegisterWin11MenuAsync(dir);
                if (failure.empty()) win11Update = true;
                else { failures.push_back({lycan::Tr(L"main.could_not_update_windows_11"), failure}); blocked = true; }
            }
        }
        if (!blocked)
        {
            const int removed = lycan::RemoveOtherShellDlls(dir, dllNow);
            if (removed) lycan::diag::Info(L"removed {} menu DLL(s) of older versions", removed);
        }
        for (auto const& f : failures) ShowMessage(f.first, f.second);
        if (failures.empty() && (classicUpdate || win11Update))
        {
            std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
            links.push_back({{lycan::Tr(L"main.restart_explorer"), {}}, [this]() { RestartExplorerAsync(); }});
            ShowNotice(UpdateStrip(), false, lycan::Tr(L"main.menu_updated"), lycan::Tr(L"main.menu_updated_restart_explorer"), links);
        }

        // ---- another folder: ask
        if (SamePath(fs::path(settings_.ignoredMenuPath), exe)) co_return;
        std::vector<std::wstring> stale;
        std::wstring nowUsed;   // where they point now: the other copy (or the old folder)
        bool classicStale = false, assocStale = false, win11Stale = false;
        if (!registeredDll.empty() && !SamePath(registeredDll.parent_path(), dir))
        {
            classicStale = true;
            stale.push_back(lycan::Tr(L"main.right_click_menu"));
            std::ifstream f(registeredDll.parent_path() / L"LycanZip.exepath", std::ios::binary);   // an installed copy: the DLL is outside the folder of the program
            std::string utf8((std::istreambuf_iterator<char>(f)), {});
            while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' ')) utf8.pop_back();
            nowUsed = utf8.empty() ? registeredDll.parent_path().wstring() : fs::path(std::wstring{winrt::to_hstring(utf8)}).parent_path().wstring();
        }
        if (lycan::IsFileAssociationsRegistered(AssocOptions()) && !SamePath(lycan::RegisteredAssocExe(AssocOptions()), exe))
        {
            assocStale = true;
            stale.push_back(lycan::Tr(L"main.file_associations"));
            if (nowUsed.empty()) nowUsed = lycan::RegisteredAssocExe(AssocOptions()).parent_path().wstring();
        }
        if (w11.found && !w11Here)
        {
            win11Stale = true;
            stale.push_back(lycan::Tr(L"main.windows_11_right_click_menu"));
            if (nowUsed.empty()) nowUsed = w11.location;
        }
        if (stale.empty()) co_return;

        std::wstring list;
        for (auto const& s : stale) list += (list.empty() ? std::wstring{} : std::wstring{lycan::Tr(L"fmt.list_sep")}) + s;
        auto dlg = MakeDialog(lycan::Tr(L"main.lycanzip_was_moved"), L"\uE946");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.still_point_old_folder_so_right", list, exeDir.wstring(), nowUsed)});
        tb.IsTextSelectionEnabled(true);
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(460);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.use_current_folder"));
        dlg.SecondaryButtonText(lycan::Tr(L"main.do_not_remind_me_again"));
        dlg.CloseButtonText(lycan::Tr(L"main.later"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        const auto answer = co_await ShowDialogAsync(dlg);
        if (answer == ContentDialogResult::Secondary)
        {
            settings_.ignoredMenuPath = exe.wstring();
            SaveSettings();
            co_return;
        }
        if (answer != ContentDialogResult::Primary) co_return;

        std::wstring error;
        if (classicStale && !lycan::RegisterShellMenu(ShellOptions(), &error)) { ShowMessage(lycan::Tr(L"main.cannot_update_right_click_menu"), hstring{error}); co_return; }
        if (assocStale && !lycan::RegisterFileAssociations(AssocOptions(), &error)) { ShowMessage(lycan::Tr(L"main.cannot_update_file_associations"), hstring{error}); co_return; }
        if (win11Stale)
        {
            hstring failure = co_await lycan_app::UnregisterWin11MenuAsync();
            if (failure.empty() && lycan_app::Win11MenuPackageAvailable(dir)) failure = co_await lycan_app::RegisterWin11MenuAsync(dir);
            if (!failure.empty()) { ShowMessage(lycan::Tr(L"main.could_not_update_windows_11"), failure); co_return; }
        }
        ShowMessage(lycan::Tr(L"main.updated"), lycan::Tr(L"main.if_menu_does_not_show_up_yet_restart_file_explorer_sign_out"));
        RefreshWin11MenuAsync();
    }

    // Restore the default settings (the context menu and the file associations are not affected, they are system-level registry entries)
    fire_and_forget MainWindow::OnResetSettingsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.restore_default_settings_title"), L"\uE777");
        TextBlock tb;
        tb.Text(lycan::Tr(L"main.defaults_theme_accent_color"));
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.restore"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        settings_ = lycan::Settings{};
        SaveSettings();
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
        ApplyViewSettings();
    }

    // ---------------------------------------------------------------- Check for a new version

    // Asks latest.json (versioncheck.h). Automatic: only when the user turned it on, never for the Store version (the Store updates it), at
    // most once in seven days, silent when it fails. Manual (the button in About): always, and it says what happened.
    winrt::fire_and_forget MainWindow::CheckForUpdateAsync(bool manual)
    {
        auto strong = get_strong();
        const lycan::InstallChannel channel = lycan::DetectChannel();
        const long long now = static_cast<long long>(std::time(nullptr));
        if (!manual)
        {
            if (compact_ || !settings_.checkUpdates || channel == lycan::InstallChannel::Store) co_return;
            if (!lycan::UpdateCheckDue(settings_.lastUpdateCheck, now)) co_return;
        }
        else
        {
            UpdateStatusText().Text(lycan::Tr(L"main.update_checking"));
            UpdateCheckNowButton().IsEnabled(false);
        }
        std::wstring url;
        {
            wchar_t buf[512];
            const DWORD n = GetEnvironmentVariableW(L"LYCAN_UPDATE_URL", buf, static_cast<DWORD>(std::size(buf)));   // tests: a server on this computer
            if (n > 0 && n < std::size(buf)) url.assign(buf, n);
        }
        co_await resume_background();
        const auto latest = lycan::FetchLatest(url);
        auto dq = DispatcherQueue();
        auto weak = get_weak();
        dq.TryEnqueue([weak, latest, manual, channel, now]()
        {
            auto self = weak.get();
            if (!self) return;
            if (latest) { self->settings_.lastUpdateCheck = now; self->SaveSettings(); }
            if (manual) self->UpdateCheckNowButton().IsEnabled(true);
            const auto theirs = latest ? lycan::ParseVersion(latest->version) : std::nullopt;
            const lycan::Version mine{LYCAN_VERSION_MAJOR, LYCAN_VERSION_MINOR, LYCAN_VERSION_PATCH};
            if (!theirs)
            {
                lycan::diag::Info(L"update check: no answer");
                if (manual) self->UpdateStatusText().Text(lycan::Tr(L"main.update_check_failed"));
                return;
            }
            const bool newer = lycan::IsNewerVersion(*theirs, mine);
            lycan::diag::Info(L"update check: latest {}, {}", lycan::WideFromUtf8(latest->version), newer ? L"newer" : L"not newer");
            if (!newer)
            {
                if (manual) self->UpdateStatusText().Text(hstring{lycan::Tf(L"main.update_up_to_date", std::wstring(LYCAN_VERSION_WSTRING))});
                return;
            }
            if (!manual && self->settings_.skipUpdateVersion == latest->version) return;
            if (manual) self->UpdateStatusText().Text(hstring{lycan::Tf(L"main.update_available", lycan::WideFromUtf8(latest->version))});
            if (channel == lycan::InstallChannel::Store) return;   // the manual check says so above; the Store does the update
            self->ShowUpdateNotice(*latest);
            if (!manual && channel == lycan::InstallChannel::Installed) self->PrepareUpdateAsync();   // the installed copy has the update ready when the user looks
        });
    }

    // The installed copy, after the automatic check found a newer version: the signed feed is asked and the update is downloaded in the background
    // (nothing is installed; the notice then offers "Restart and update"). When anything fails, the notice with "Install now" stays and the user can try by hand.
    winrt::fire_and_forget MainWindow::PrepareUpdateAsync()
    {
        auto strong = get_strong();
#ifdef LYCAN_VELOPACK
        if (updateBusy_ || readyUpdater_) co_return;
        updateBusy_ = true;
        auto updater = std::make_shared<lycan::InstalledUpdater>(lycan::UpdateFeedBase(), lycan::GetUpdatePublicKey());
        std::wstring error;
        std::optional<lycan::UpdateOffer> offer;
        bool ok = false;
        {
            auto dq = DispatcherQueue();
            auto weak = get_weak();
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            offer = updater->Check(&error);
            if (offer)
            {
                std::atomic<int> shown{-10};
                ok = updater->Download([dq, weak, &shown](int percent) {
                    if (percent < shown.load() + 5 && percent < 100) return;   // not at every percent
                    shown = percent;
                    dq.TryEnqueue([weak, percent]() { if (auto self = weak.get()) self->ShowNotice(self->UpdateStrip(), false, hstring{lycan::Tf(L"main.update_downloading", percent)}, {}, {}); });
                }, &error);
            }
            co_await ui;
        }
        updateBusy_ = false;
        if (!offer || !ok)
        {
            lycan::diag::Info(L"update (background): {}", error.empty() ? std::wstring(L"no newer version in the feed") : error);
            if (offer) UpdateStrip().Visibility(Visibility::Collapsed);   // the progress notice goes; the user can try again from the About page
            co_return;
        }
        readyUpdater_ = updater;
        const std::wstring ver = lycan::WideFromUtf8(offer->version);
        lycan::diag::Info(L"update (background): {} is downloaded and checked", ver);
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        links.push_back({{lycan::Tr(L"main.update_restart_now"), {}}, [this]() { ApplyReadyUpdate(); }});
        ShowNotice(UpdateStrip(), false, hstring{lycan::Tf(L"main.update_ready", ver)}, hstring{lycan::Tr(L"main.update_ready_hint")}, links);
#else
        co_return;
#endif
    }

    void MainWindow::ApplyReadyUpdate()
    {
#ifdef LYCAN_VELOPACK
        if (!readyUpdater_) return;
        if (UnfinishedTaskCount() > 0)
        {
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_tasks_running"));
            return;
        }
        std::wstring error;
        if (!readyUpdater_->ApplyAfterExit(&error))
        {
            lycan::diag::Warn(L"update: {}", error);
            readyUpdater_.reset();
            UpdateStrip().Visibility(Visibility::Collapsed);
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_failed"));
            return;
        }
        lycan::diag::Info(L"update: the update program was started, closing");
        Application::Current().Exit();
#endif
    }

    void MainWindow::ShowUpdateNotice(lycan::LatestInfo const& info)
    {
        const lycan::InstallChannel channel = lycan::DetectChannel();
        const std::wstring ver = lycan::WideFromUtf8(info.version);
        auto openUrl = [](std::string const& u) { if (!u.empty()) ShellExecuteW(nullptr, L"open", lycan::WideFromUtf8(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        hstring message;
        if (channel == lycan::InstallChannel::Winget || channel == lycan::InstallChannel::Scoop)
        {
            const std::wstring cmd = channel == lycan::InstallChannel::Winget ? L"winget upgrade Aegisulf.LycanZip" : L"scoop update lycanzip";
            message = hstring{lycan::Tf(L"main.update_with_command", cmd)};
        }
        else if (channel == lycan::InstallChannel::Installed)
        {
            links.push_back({{lycan::Tr(L"main.update_install_now"), {}}, [this]() { InstallUpdateAsync(); }});
        }
        else
        {
            const std::string page = info.page.empty() ? std::string("https://aegisulf.com/lycanzip/download/") : info.page;
            links.push_back({{lycan::Tr(L"main.update_download"), {}}, [openUrl, page]() { openUrl(page); }});
        }
        if (!info.changelog.empty())
            links.push_back({{lycan::Tr(L"main.update_whats_new"), {}}, [openUrl, log = info.changelog]() { openUrl(log); }});
        links.push_back({{lycan::Tr(L"main.update_skip"), hstring{lycan::Tr(L"main.update_skip_tip")}}, [this, v = info.version]()
        {
            settings_.skipUpdateVersion = v;
            SaveSettings();
            UpdateStrip().Visibility(Visibility::Collapsed);
        }});
        ShowNotice(UpdateStrip(), false, hstring{lycan::Tf(L"main.update_available", ver)}, message, links);
    }

    // The installed copy: asks the signed feed (the signature and the hash of every package are checked, src/updater), asks the user, downloads, starts the
    // update program that waits for this process to end, and ends the process. Nothing is changed when anything fails.
    winrt::fire_and_forget MainWindow::InstallUpdateAsync()
    {
        auto strong = get_strong();
#ifdef LYCAN_VELOPACK
        if (readyUpdater_) { ApplyReadyUpdate(); co_return; }   // downloaded in the background already
        if (updateBusy_) co_return;
        if (UnfinishedTaskCount() > 0)
        {
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_tasks_running"));
            co_return;
        }
        updateBusy_ = true;
        auto updater = std::make_shared<lycan::InstalledUpdater>(lycan::UpdateFeedBase(), lycan::GetUpdatePublicKey());
        std::wstring error;
        std::optional<lycan::UpdateOffer> offer;
        {
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            offer = updater->Check(&error);
            co_await ui;
        }
        if (!offer)
        {
            lycan::diag::Info(L"update: {}", error.empty() ? std::wstring(L"no newer version in the feed") : error);
            updateBusy_ = false;
            ShowMessage(lycan::Tr(L"main.update_install_title"), error.empty() ? lycan::Tr(L"main.update_not_in_feed") : lycan::Tr(L"main.update_failed"));
            co_return;
        }
        {
            wchar_t size[64];
            swprintf_s(size, L"%.1f MB", static_cast<double>(offer->bytes) / (1024.0 * 1024.0));
            auto dlg = MakeDialog(lycan::Tr(L"main.update_install_title"), L"\uE896");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.update_install_question", lycan::WideFromUtf8(offer->version), std::wstring(size))});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"main.update_install_now"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Primary);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) { updateBusy_ = false; co_return; }
        }
        if (UnfinishedTaskCount() > 0)   // something was started while the question was open
        {
            updateBusy_ = false;
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_tasks_running"));
            co_return;
        }
        auto showProgress = [this](int percent) { ShowNotice(UpdateStrip(), false, hstring{lycan::Tf(L"main.update_downloading", percent)}, {}, {}); };
        showProgress(0);
        bool ok = false;
        {
            auto dq = DispatcherQueue();
            auto weak = get_weak();
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            std::atomic<int> shown{0};
            ok = updater->Download([dq, weak, &shown](int percent) {
                if (percent < shown.load() + 5 && percent < 100) return;   // not at every percent
                shown = percent;
                dq.TryEnqueue([weak, percent]() { if (auto self = weak.get()) self->ShowNotice(self->UpdateStrip(), false, hstring{lycan::Tf(L"main.update_downloading", percent)}, {}, {}); });
            }, &error);
            co_await ui;
        }
        if (ok && UnfinishedTaskCount() == 0) ok = updater->ApplyAfterExit(&error);
        if (!ok)
        {
            lycan::diag::Warn(L"update: {}", error);
            updateBusy_ = false;
            UpdateStrip().Visibility(Visibility::Collapsed);
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_failed"));
            co_return;
        }
        lycan::diag::Info(L"update: the update program was started, closing");
        Application::Current().Exit();
#else
        co_return;
#endif
    }

    void MainWindow::OnUpdateCheckNowClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        CheckForUpdateAsync(true);
    }

    // The links of the About page open in the browser of the user. Tests: LYCAN_TEST_URL_FILE=<file> appends the address to the file instead.
    static void OpenAboutLink(wchar_t const* url)
    {
        if (const wchar_t* f = _wgetenv(L"LYCAN_TEST_URL_FILE"))
        {
            std::ofstream(f, std::ios::app) << lycan::ToUtf8(url) << "\n";
            return;
        }
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::OnOpenWebsiteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { OpenAboutLink(L"https://aegisulf.com/lycanzip/"); }
    void MainWindow::OnOpenSourceClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { OpenAboutLink(L"https://github.com/Aegisulf/LycanZip"); }

    void MainWindow::OnShowWelcomeClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!lycan_app::WelcomeIsOpen()) ShowWelcomeAsync();
    }

    void MainWindow::OnOpenSettingsFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring args = L"/select,\"" + settingsPath_.wstring() + L"\"";
        std::error_code ec;
        if (fs::exists(settingsPath_, ec)) ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        else ShellExecuteW(nullptr, L"open", settingsPath_.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // "Data location" in the About tab: portable (the settings next to the program) or in the user's folder; see datalocation.h
    void MainWindow::RefreshDataLocationUi()
    {
        const lycan::DataLocation loc = lycan::GetDataLocation();
        DataModeText().Text(loc.packaged ? lycan::Tr(L"main.msix_package_settings_templates")
                            : loc.portable ? lycan::Tr(L"main.portable_mode_settings_templates")
                                           : lycan::Tr(L"main.normal_mode_settings_templates"));
        DataFolderText().Text(hstring{lycan::Tf(L"main.folder_settings", settingsPath_.parent_path().wstring())});
        DataSwitchButton().Visibility(loc.packaged ? Visibility::Collapsed : Visibility::Visible);
        if (loc.packaged) { DataNoteText().Text(L""); return; }
        DataSwitchButton().Content(box_value(loc.portable ? lycan::Tr(L"main.switch_normal_mode_user_folder") : lycan::Tr(L"main.switch_portable_mode_program")));
        const bool blocked = !loc.portable && !lycan::DirWritable(loc.exeDir);
        DataSwitchButton().IsEnabled(!blocked);
        DataNoteText().Text(blocked ? hstring{lycan::Tf(L"main.program_folder_cannot_be_written", loc.exeDir.wstring())}
                                    : lycan::Tr(L"main.switching_copies_settings"));
    }

    fire_and_forget MainWindow::OnDataSwitchClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        const lycan::DataLocation loc = lycan::GetDataLocation();
        if (loc.packaged) co_return;
        if (UnfinishedTaskCount() > 0)
        {
            ShowMessage(lycan::Tr(L"main.tasks_still_running"), lycan::Tr(L"main.wait_tasks_end_cancel_them_before"));
            co_return;
        }
        const bool toPortable = !loc.portable;
        auto dlg = MakeDialog(lycan::Tr(L"main.switch_data_location"), L"\uE8B7");
        TextBlock tb;
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(460);
        tb.Text(hstring{(toPortable ? lycan::Tf(L"main.settings_templates_copied_program", loc.exeDir.wstring())
                                    : lycan::Tf(L"main.settings_templates_copied_user", loc.userDir.wstring())) +
                        L"\n" + lycan::Tr(L"main.if_new_place_has_settings_files")});
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.switch_restart"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        if (UnfinishedTaskCount() > 0) co_return;  // a task started while the question was open
        SaveSettings();  // what is on the screen goes into the files that are copied
        const lycan::SwitchStatus st = lycan::SwitchDataLocation(loc.exeDir, loc.userDir, toPortable);
        lycan::diag::Write(lycan::diag::Level::Warn, std::wstring(L"data location: switch to ") + (toPortable ? L"the program folder" : L"the user folder") + L", result " + std::to_wstring(static_cast<int>(st)));
        if (st != lycan::SwitchStatus::Ok)
        {
            ShowMessage(lycan::Tr(L"main.could_not_switch"), st == lycan::SwitchStatus::NotWritable ? lycan::Tr(L"main.new_place_cannot_be_written")
                                                : st == lycan::SwitchStatus::CopyFailed ? lycan::Tr(L"main.copying_settings_failed")
                                                                                       : lycan::Tr(L"main.flag_file_portable_mode_lycanzip"));
            co_return;
        }
        // start the program again (it waits for this one to end) and end this one
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        Application::Current().Exit();
    }

    void MainWindow::OnOpenLogFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        std::error_code ec;
        const fs::path dir = lycan::diag::Dir();
        fs::create_directories(dir, ec);  // nothing may have been written yet
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // The version, Windows, the engine, the switches and the last lines of the log: one paste into a bug report
    void MainWindow::OnCopyDiagnosticsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetText(hstring{lycan::diag::DiagnosticText(40)});
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        DiagCopiedNote().Visibility(Visibility::Visible);
        auto timer = dispatcher_.CreateTimer();
        timer.IsRepeating(false);
        timer.Interval(std::chrono::seconds(3));
        timer.Tick([weak = get_weak()](auto&& sender, auto&&) {
            if (auto self = weak.get()) self->DiagCopiedNote().Visibility(Visibility::Collapsed);
            sender.Stop();
        });
        timer.Start();
    }

    void MainWindow::OnSettingsBackClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ShowView(viewBeforeSettings_ == View::Archive && !current_ ? View::Home : viewBeforeSettings_);
    }

    // ---------------------------------------------------------------- Drag and drop

    void MainWindow::OnDragOver(Windows::Foundation::IInspectable const&, DragEventArgs const& e)
    {
        if (dragOutActive_) return;  // files dragged out of an archive must not be received by our own window again
        if (e.DataView().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()))
        {
            e.AcceptedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
            e.DragUIOverride().Caption(lycan::Tr(L"main.release_process"));
            SetDragGlow(true);
        }
    }

    void MainWindow::OnDragLeave(Windows::Foundation::IInspectable const&, DragEventArgs const&) { SetDragGlow(false); }

    fire_and_forget MainWindow::OnDrop(Windows::Foundation::IInspectable const&, DragEventArgs const& e)
    {
        auto strong = get_strong();
        if (dragOutActive_) co_return;
        auto deferral = e.GetDeferral();
        SetDragGlow(false);
        PlaySfx(L"drop");
        std::vector<fs::path> paths;
        try
        {
            auto items = co_await e.DataView().GetStorageItemsAsync();
            for (auto const& it : items) paths.emplace_back(std::wstring(it.Path()));
        }
        catch (...) {}
        deferral.Complete();
        if (paths.empty()) co_return;
        HandleDroppedPaths(std::move(paths));
    }

    // ---------------------------------------------------------------- Opening an archive

    fire_and_forget MainWindow::OnOpenArchiveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeOpenArchive, false, true);
        if (files.Size() > 0) OpenFromUi(fs::path(std::wstring(files.GetAt(0))));
    }

    lycan::OpenOptions MainWindow::MakeOpenOptions()
    {
        lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
        oo.nameCodepage = currentCp_;
        if (!password_.empty())
            oo.password = [pw = password_](std::wstring& out) { out = pw; return true; };
        return oo;
    }

    // ---------------------------------------------------------------- Password

    // The dialog opens in the middle of a double click (the second press and release come after it): the list takes the focus back. So the
    // focus is put on the password box a moment after the dialog is there.
    static fire_and_forget FocusSoon(Microsoft::UI::Dispatching::DispatcherQueue dq, TextBox plain, PasswordBox hidden, CheckBox showPw)
    {
        co_await resume_after(150ms);
        co_await resume_ui{dq};
        if (showPw.IsChecked().Value()) { plain.Focus(FocusState::Programmatic); plain.SelectAll(); }
        else hidden.Focus(FocusState::Programmatic);
    }

    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskPasswordAsync(hstring name, bool previousWasWrong)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        // Show the password in plain text by default: a hidden password box disables the input method, so Chinese and other characters could not be typed
        TextBox plain;
        plain.Header(box_value(lycan::Tr(L"main.password")));
        plain.MinWidth(320);
        Automation::AutomationProperties::SetName(plain, lycan::Tr(L"main.password"));
        PasswordBox hidden;
        hidden.Header(box_value(lycan::Tr(L"main.password")));
        hidden.MinWidth(320);
        hidden.PasswordRevealMode(PasswordRevealMode::Peek);
        hidden.Visibility(Visibility::Collapsed);
        Automation::AutomationProperties::SetName(hidden, lycan::Tr(L"main.password"));
        CheckBox showPw;
        showPw.Content(box_value(lycan::Tr(L"main.show_password_keep_checked_when")));
        showPw.IsChecked(true);
        Automation::AutomationProperties::SetName(showPw, lycan::Tr(L"main.show_password"));
        showPw.Click([plain, hidden, showPw](auto&&, auto&&) {
            const bool show = showPw.IsChecked().Value();
            if (show) plain.Text(hidden.Password()); else hidden.Password(plain.Text());
            plain.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
            hidden.Visibility(show ? Visibility::Collapsed : Visibility::Visible);
        });

        auto dlg = MakeDialog(lycan::Tr(L"main.password_required"), L"\uE72E");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(380);
        {
            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Spacing(10);
            row.Children().Append(MakeGlyph(L"\uE7B8", 20, BrushForCategory(FileCategory::Archive)));
            TextBlock tb;
            tb.Text(name);
            tb.VerticalAlignment(VerticalAlignment::Center);
            tb.TextTrimming(TextTrimming::CharacterEllipsis);
            row.Children().Append(tb);
            sp.Children().Append(MakeCardBox(row));
        }
        if (previousWasWrong)
        {
            StackPanel warnRow;
            warnRow.Orientation(Orientation::Horizontal);
            warnRow.Spacing(8);
            auto red = Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})};
            warnRow.Children().Append(MakeGlyph(L"\uE783", 15, red));
            TextBlock warn;
            warn.Text(lycan::Tr(L"main.wrong_password_please_try_again"));
            warn.Foreground(red);
            warnRow.Children().Append(warn);
            sp.Children().Append(warnRow);
        }
        sp.Children().Append(plain);
        sp.Children().Append(hidden);
        sp.Children().Append(showPw);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        dlg.Opened([dq = dispatcher_, plain, hidden, showPw](auto&&, auto&&) { FocusSoon(dq, plain, hidden, showPw); });
        auto result = co_await ShowDialogAsync(dlg);
        if (result != ContentDialogResult::Primary) co_return hstring{};
        co_return showPw.IsChecked().Value() ? plain.Text() : hidden.Password();
    }

    // Encrypted content (zip AES / 7z data encryption ...): ask and "verify" the password first, and ask again if wrong, instead of finding out only when the extraction fails
    Windows::Foundation::IAsyncOperation<bool> MainWindow::EnsurePasswordAsync()
    {
        auto strong = get_strong();
        if (!current_ || !current_->Caps().encrypted || passwordVerified_) co_return true;
        auto src = current_;
        hstring name{currentPath_.filename().wstring()};
        bool wrong = false;
        for (;;)
        {
            auto pw = co_await AskPasswordAsync(name, wrong);
            if (pw.empty()) co_return false;  // the user cancelled
            SetBusy(true);
            co_await resume_background();
            bool ok = src->CheckPassword(std::wstring(pw));
            co_await resume_ui{dispatcher_};
            SetBusy(false);
            if (ok)
            {
                password_ = std::wstring(pw);
                passwordVerified_ = true;
                if (tree_ && view_ == View::Archive) RefreshList(true);   // the lock icons give way to the icons of the file types
                co_return true;
            }
            wrong = true;
        }
    }

    fire_and_forget MainWindow::OpenArchiveAsync(fs::path path, unsigned codepage, std::wstring password, bool previousWasWrong)
    {
        auto strong = get_strong();
        struct PendingGuard
        {
            MainWindow* w;
            explicit PendingGuard(MainWindow* x, fs::path const& p) : w(x) { ++w->openPending_; w->openingPath_ = p; w->NotifyStateChanged(true); }
            ~PendingGuard() { if (--w->openPending_ == 0) w->openingPath_.clear(); w->NotifyStateChanged(true); }
        } pending{this, path};
        // nothing on screen to keep (the home page of a fresh window): the loading view; otherwise the ring over the content
        const bool fresh = !current_ && view_ == View::Home && !compact_;
        if (fresh) ShowLoading(path.filename().wstring());
        else SetBusy(true);
        std::shared_ptr<lycan::ArchiveSource> src;
        std::wstring errorText;
        lycan::Message tempMsg;  // the scratch file of a tar.gz did not fit
        bool needPassword = false, wrong = previousWasWrong;

        co_await resume_background();
        const auto openStart = std::chrono::steady_clock::now();
        if (const wchar_t* d = _wgetenv(L"LYCAN_TEST_OPEN_DELAY")) Sleep(static_cast<DWORD>(_wtoi(d)));  // tests: a slow open, to see the loading view
        try
        {
            lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
            oo.nameCodepage = codepage;
            if (!password.empty()) oo.password = [password](std::wstring& out) { out = password; return true; };
            src = lycan::ArchiveSource::Open(path, oo);
        }
        catch (lycan::Error const& e)
        {
            if (e.wrongPassword()) { needPassword = true; wrong = true; }
            else if (e.cancelled()) needPassword = true;  // a password is needed (encrypted headers)
            else if (e.tempSpace()) tempMsg = e.message();
            else errorText = e.message().Text();
        }
        catch (std::exception const& e) { errorText = FromUtf8(e.what()); }

        co_await resume_ui{dispatcher_};
        SetBusy(false);
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - openStart).count();
            if (src) lycan::diag::Info(L"opened {}: {}, {} item(s), {} ms", lycan::diag::Name(path), src->FormatName(), src->Entries().size(), ms);
            else if (needPassword) lycan::diag::Info(L"opening {}: a password is needed{}", lycan::diag::Name(path), wrong ? L" (the one given was wrong)" : L"");
            else if (tempMsg.empty()) lycan::diag::Warn(L"could not open {} ({} ms): {}", lycan::diag::Name(path), ms, errorText);
            else lycan::diag::Warn(L"could not open {}: the scratch folder has too little room", lycan::diag::Name(path));
        }

        if (!tempMsg.empty())
        {
            uint64_t need = 0, have = 0;
            std::wstring dir;
            lycan::ParseTempSpaceMessage(tempMsg, need, have, dir);
            if (loading_) { loadingShown_ = false; ApplyLoadingVisibility(); }
            const hstring base = co_await AskTempLocationAsync(need, have, dir);
            if (!base.empty())
            {
                OpenArchiveAsync(path, codepage, password, previousWasWrong);  // the place was chosen: it is used from now on
                co_return;
            }
            restorePos_ = {};
            EndLoading(true);
            co_return;
        }

        if (needPassword)
        {
            if (loading_) { loadingShown_ = false; ApplyLoadingVisibility(); }  // the question comes first, without the spinner behind it
            auto pw = co_await AskPasswordAsync(hstring{path.filename().wstring()}, wrong);
            if (!pw.empty()) OpenArchiveAsync(path, codepage, std::wstring(pw), false);
            else
            {
                restorePos_ = {};
                EndLoading(true);
            }
            co_return;
        }
        if (!src)
        {
            restorePos_ = {};
            EndLoading(true);
            ShowMessage(lycan::Tr(L"main.cannot_open_archive"), hstring{errorText.empty() ? lycan::Tr(L"main.file_format_unrecognized_file_damaged") : errorText});
            co_return;
        }
        password_ = password;
        passwordVerified_ = !password.empty();  // an archive with encrypted headers: if it could be opened the password is right
        ShowArchive(std::move(src), path, codepage);
    }

    void MainWindow::ShowArchive(std::shared_ptr<lycan::ArchiveSource> src, fs::path path, unsigned codepage)
    {
        EndLoading(false);
        if (current_ && currentPath_ != path)
        {
            // this window now shows another archive ("open in this window"): its previews would go on showing the old one
            lycan_app::CloseImagePreview(this);
            lycan_app::CloseAudioPreview(this);
        }
        current_ = src;
        currentPath_ = path;
        currentCp_ = codepage;
        LockArchiveFile(path);
        UpdateArchiveIcon();
        UpdateExtractButton();
        thumbSessionKey_.clear();
        thumbNoteShown_ = false;

        uint64_t total = 0;
        for (auto const& e : src->Entries())
            if (!e.isDir) total += e.size;
        {
            auto caps = src->Caps();
            archiveInfo_ = src->FormatName() + lycan::Tf(L"fmt.status_items_size", src->Entries().size(), FormatSize(total));
            if (caps.solid) archiveInfo_ += lycan::Tr(L"main.solid_archive");
            if (caps.encrypted) archiveInfo_ += lycan::Tr(L"main.encrypted");
        }
        SetWindowTitle(path.filename().wstring());

        // View state: starts from the default of the settings (default: one level browsing)
        tree_ = std::make_shared<lycan::ArchiveTree>(src->Entries(), settings_.hideMacMetadata);
        tree_->SetMethodNames(src->MethodNames());
        flat_ = settings_.browseFlat;
        viewMode_ = settings_.viewMode;
        treeWanted_ = settings_.showFolderTree;
        sortKey_ = lycan::SortKey::Name;
        sortAsc_ = true;
        query_.clear();
        if (keptView_.valid && restoreFor_ == path)  // this archive was just changed: keep the view the user had
        {
            flat_ = keptView_.flat;
            viewMode_ = keptView_.mode;
            sortKey_ = keptView_.sort;
            sortAsc_ = keptView_.asc;
            treeWanted_ = keptView_.tree;
            query_ = keptView_.query;
        }
        keptView_ = {};
        syncingUi_ = true;
        SearchBox().Text(hstring{query_});
        FlatToggle().IsChecked(flat_);
        TreeToggle().IsChecked(treeWanted_);
        syncingUi_ = false;
        BuildFolderTree();
        UpdateTreeVisibility();
        NavigateTo(tree_->Root(), true);
        // The archive has only one folder (the outermost one is often just a wrapper): open one level down automatically, at most three times
        for (int depth = 0; depth < 3 && !flat_; ++depth)
        {
            auto kids = tree_->Children(curNode_);
            if (kids.size() != 1 || !tree_->Node(kids[0]).isDir) break;
            NavigateTo(kids[0], true);
        }
        // After switching the file name encoding go back to the folder where we were (same archive, the item with the same order number goes up to the original depth)
        if (restorePos_.entry != lycan::kNoEntry && restorePos_.archive == path)
        {
            uint32_t found = lycan::kNoNode;
            for (uint32_t n = 0; n < tree_->NodeCount(); ++n)
                if (tree_->Node(n).entryIndex == restorePos_.entry) { found = n; break; }
            if (found != lycan::kNoNode)
            {
                auto depthOf = [&](uint32_t n) { auto const& p = tree_->Node(n).path; return static_cast<int>(std::count(p.begin(), p.end(), L'/')) + 1; };
                for (int d = depthOf(found); d > restorePos_.depth && tree_->Node(found).parent != lycan::kNoNode; --d) found = tree_->Node(found).parent;
                if (found != tree_->Root()) NavigateTo(found, true);
            }
        }
        restorePos_ = {};
        // after a change: the folder that was shown, and the items that were added / renamed selected
        if (restoreFor_ == path)
        {
            if (!flat_)
            {
                const uint32_t n = restoreFolder_.empty() ? tree_->Root() : tree_->Find(restoreFolder_);
                if (n != lycan::kNoNode && tree_->Node(n).isDir) NavigateTo(n, true);
            }
            if (!restoreSelect_.empty() && entries_)
            {
                bool first = true;
                for (auto const& it : entries_)
                {
                    auto row = it.as<LycanZip::EntryRow>();
                    if (std::find(restoreSelect_.begin(), restoreSelect_.end(), tree_->Node(row.NodeId()).path) == restoreSelect_.end()) continue;
                    ActiveList().SelectedItems().Append(row);
                    if (first) { ActiveList().ScrollIntoView(row); first = false; }
                }
            }
        }
        restoreFor_.clear();
        restoreFolder_.clear();
        restoreSelect_.clear();

        auto caps = src->Caps();

        // File name encoding
        auto ne = src->NameEncoding();
        EncodingPanel().Visibility(ne.applicable ? Visibility::Visible : Visibility::Collapsed);
        EncodingStrip().Visibility(Visibility::Collapsed);
        if (ne.applicable)
        {
            updatingEncoding_ = true;
            int idx = 0;
            for (uint32_t i = 0; i < EncodingCombo().Items().Size(); ++i)
            {
                int32_t tag = unbox_value<int32_t>(EncodingCombo().Items().GetAt(i).as<ComboBoxItem>().Tag());
                if (codepage ? static_cast<unsigned>(tag) == ne.codepage : tag == 0) { idx = static_cast<int>(i); break; }
            }
            EncodingCombo().SelectedIndex(idx);
            updatingEncoding_ = false;
            if (ne.autoDetected)
            {
                ShowNotice(EncodingStrip(), ne.ambiguous, ne.ambiguous ? lycan::Tr(L"main.filename_encoding_uncertain") : lycan::Tr(L"main.filename_encoding_detected"),
                           hstring{lycan::Tf(L"main.currently_using_if_file_names_look", lycan::CodepageName(ne.codepage))}, {});
            }
        }

        ShowSumsBanner();
        listFxUntil_ = GetTickCount64() + 400;
        PlaySfx(L"open");
        ShowView(View::Archive);
        NotifyStateChanged(true);
        if (!openNote_.empty()) { ShowStatusNote(openNote_); openNote_.clear(); }
        MaybeOfferWelcome();
    }

    // The welcome window opens by itself only when the program is started on its own. The Microsoft Store does not start a program after it is
    // installed, so the first start is often a double-click on an archive: then the first full window (not the compact windows of the context
    // menu, not a preview) offers it in a one-line notice above the list. "Not now" counts as seen; closing the notice asks again next time.
    void MainWindow::MaybeOfferWelcome()
    {
        static bool offered = false;   // once per process (several windows)
        if (offered || compact_ || previewOnly_ || settings_.welcomeDone || lycan_app::WelcomeIsOpen()) return;
        if (GetEnvironmentVariableW(L"LYCAN_NO_WELCOME", nullptr, 0) > 0 && GetEnvironmentVariableW(L"LYCAN_FORCE_WELCOME", nullptr, 0) == 0) return;
        offered = true;
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        links.push_back({{lycan::Tr(L"main.welcome_offer_start"), {}}, [this]() { WelcomeStrip().Visibility(Visibility::Collapsed); ShowWelcomeAsync(); }});
        links.push_back({{lycan::Tr(L"main.welcome_offer_no"), {}}, [this]() {
            WelcomeStrip().Visibility(Visibility::Collapsed);
            settings_.welcomeDone = true;
            SaveSettings();
        }});
        ShowNotice(WelcomeStrip(), false, lycan::Tr(L"main.welcome_offer_title"), lycan::Tr(L"main.welcome_offer_hint"), links);
    }

    void MainWindow::OnBackClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { ShowHome(); }

    void MainWindow::OnEncodingChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (updatingEncoding_ || !current_) return;
        auto item = EncodingCombo().SelectedItem().try_as<ComboBoxItem>();
        if (!item) return;
        unsigned cp = static_cast<unsigned>(unbox_value<int32_t>(item.Tag()));
        if (cp == currentCp_) return;
        restorePos_ = {};
        if (tree_ && curNode_ != tree_->Root())
        {
            for (uint32_t n : tree_->Descendants(curNode_))
                if (tree_->Node(n).entryIndex != lycan::kNoEntry) { restorePos_.entry = tree_->Node(n).entryIndex; break; }
            auto const& p = tree_->Node(curNode_).path;
            restorePos_.depth = static_cast<int>(std::count(p.begin(), p.end(), L'/')) + 1;
            restorePos_.archive = currentPath_;
        }
        OpenArchiveAsync(currentPath_, cp, password_);
    }

    // ---------------------------------------------------------------- Archive view: folder tree / navigation / sorting / search / selection

    static uint32_t NodeIdOf(TreeViewNode const& tn) { return tn.Content().as<LycanZip::EntryRow>().NodeId(); }

    // The picture for a row: the cached icon of its kind of file (when the setting is on and it is loaded already)
    void MainWindow::ApplyShellIcon(LycanZip::EntryRow const& row)
    {
        auto* r = get_self<implementation::EntryRow>(row);
        if (!settings_.shellIcons) { r->SetShellIcons(nullptr, nullptr); return; }
        const std::wstring key{r->IconKey()};
        if (key.empty()) return;
        const double scale = GetDpiForWindow(Hwnd()) / 96.0;
        auto find = [&](int px) -> Microsoft::UI::Xaml::Media::ImageSource {
            auto it = shellIconCache_.find(key + L"|" + std::to_wstring(px));
            return it == shellIconCache_.end() ? nullptr : it->second;
        };
        auto smallIcon = find(lycan_app::ShellIconPx(18, scale));
        auto largeIcon = viewMode_ == lycan::ViewMode::Icons ? find(lycan_app::ShellIconPx(static_cast<int>(IconDipFor(settings_.thumbSize)), scale)) : nullptr;
        if (smallIcon || largeIcon) r->SetShellIcons(smallIcon, largeIcon);
    }

    // Looks at the rows and the tree for kinds of file that have no icon yet and loads them (a background thread), then gives the rows their pictures
    void MainWindow::RefreshShellIcons()
    {
        if (!settings_.shellIcons) return;
        const double scale = GetDpiForWindow(Hwnd()) / 96.0;
        const int pxSmall = lycan_app::ShellIconPx(18, scale), pxLarge = lycan_app::ShellIconPx(static_cast<int>(IconDipFor(settings_.thumbSize)), scale);
        std::set<std::wstring> missing;   // "key|px"
        auto need = [&](std::wstring const& key) {
            if (key.empty()) return;
            if (!shellIconCache_.count(key + L"|" + std::to_wstring(pxSmall))) missing.insert(key + L"|" + std::to_wstring(pxSmall));
            if (viewMode_ == lycan::ViewMode::Icons && !shellIconCache_.count(key + L"|" + std::to_wstring(pxLarge))) missing.insert(key + L"|" + std::to_wstring(pxLarge));
        };
        need(L"<dir>");
        if (entries_) for (auto const& it : entries_) need(std::wstring{it.as<LycanZip::EntryRow>().IconKey()});
        if (!missing.empty()) LoadShellIconsAsync(std::vector<std::wstring>(missing.begin(), missing.end()));
    }

    winrt::fire_and_forget MainWindow::LoadShellIconsAsync(std::vector<std::wstring> keys)
    {
        auto strong = get_strong();
        for (auto& k : keys) shellIconPending_.insert(k);
        if (shellIconBusy_) co_return;   // the one that is running picks the new keys up when it comes back
        shellIconBusy_ = true;
        while (!shellIconPending_.empty())
        {
            std::vector<std::wstring> todo(shellIconPending_.begin(), shellIconPending_.end());
            shellIconPending_.clear();
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            std::vector<std::pair<std::wstring, lycan_app::IconPixels>> loaded;
            for (auto const& k : todo)
            {
                const auto bar = k.rfind(L'|');
                loaded.emplace_back(k, lycan_app::LoadShellIconPixels(k.substr(0, bar), _wtoi(k.c_str() + bar + 1)));
            }
            co_await ui;
            for (auto& [k, px] : loaded)
                if (auto img = lycan_app::MakeIconBitmap(px)) shellIconCache_.insert_or_assign(k, img);
        }
        shellIconBusy_ = false;
        // everything that is on screen gets its picture
        if (entries_) for (auto const& it : entries_) ApplyShellIcon(it.as<LycanZip::EntryRow>());
        for (auto const& [id, tn] : treeNodes_) if (tn.Content()) ApplyShellIcon(tn.Content().as<LycanZip::EntryRow>());
    }

    void MainWindow::BuildFolderTree()
    {
        FolderTree().RootNodes().Clear();
        treeNodes_.clear();
        if (!tree_) return;
        auto root = MakeTreeNode(tree_->Root());
        FolderTree().RootNodes().Append(root);
        RealizeTreeChildren(root);
        root.IsExpanded(true);
    }

    // The nodes of the folder tree create their children only when expanded (a large archive may have tens of thousands of folders)
    TreeViewNode MainWindow::MakeTreeNode(uint32_t id)
    {
        auto const& n = tree_->Node(id);
        TreeViewNode tn;
        hstring name = id == tree_->Root() ? hstring{currentPath_.filename().wstring()} : hstring{n.name};
        auto row = make<implementation::EntryRow>(id, name, hstring{}, hstring{}, hstring{}, hstring{}, true, false, false);
        if (id == tree_->Root()) get_self<implementation::EntryRow>(row)->SetRootIcon(FileIconImage(currentPath_));
        else ApplyShellIcon(row);
        tn.Content(row);
        bool hasDirs = std::any_of(n.children.begin(), n.children.end(), [&](uint32_t c) { return tree_->Node(c).isDir; });
        tn.HasUnrealizedChildren(hasDirs);
        treeNodes_[id] = tn;
        return tn;
    }

    void MainWindow::RealizeTreeChildren(TreeViewNode const& tn)
    {
        if (!tree_ || !tn.HasUnrealizedChildren()) return;
        for (uint32_t c : tree_->Children(NodeIdOf(tn)))
            if (tree_->Node(c).isDir) tn.Children().Append(MakeTreeNode(c));
        tn.HasUnrealizedChildren(false);
    }

    void MainWindow::OnTreeExpanding(TreeView const&, TreeViewExpandingEventArgs const& args) { RealizeTreeChildren(args.Node()); }

    void MainWindow::OnTreeItemInvoked(TreeView const&, TreeViewItemInvokedEventArgs const& args)
    {
        auto item = args.InvokedItem();
        if (auto tn = item.try_as<TreeViewNode>()) NavigateTo(NodeIdOf(tn), false);
        else if (auto row = item.try_as<LycanZip::EntryRow>()) NavigateTo(row.NodeId(), false);
    }

    // Makes the tree selection follow the current folder (when navigating from the breadcrumb / a double click / up one level)
    void MainWindow::ExpandTreeTo(uint32_t node)
    {
        std::vector<uint32_t> chain;
        for (uint32_t x = node;; x = tree_->Node(x).parent)
        {
            chain.push_back(x);
            if (x == tree_->Root()) break;
        }
        std::reverse(chain.begin(), chain.end());
        TreeViewNode last{nullptr};
        for (size_t i = 0; i < chain.size(); ++i)
        {
            auto it = treeNodes_.find(chain[i]);
            if (it == treeNodes_.end()) break;
            last = it->second;
            if (i + 1 < chain.size())
            {
                RealizeTreeChildren(last);
                last.IsExpanded(true);
            }
        }
        if (!last) return;
        // Setting the selection must wait until the control is loaded (right after an archive is opened the tree is not shown yet and the template is not applied; setting it then would crash XAML)
        dispatcher_.TryEnqueue(Microsoft::UI::Dispatching::DispatcherQueuePriority::Low, [weak = get_weak(), last] {
            auto self = weak.get();
            if (!self || !self->tree_ || !self->FolderTree().IsLoaded()) return;
            self->syncingUi_ = true;
            self->FolderTree().SelectedNode(last);
            self->syncingUi_ = false;
        });
    }

    void MainWindow::NavigateTo(uint32_t node, bool syncTree)
    {
        if (!tree_ || node >= tree_->NodeCount() || !tree_->Node(node).isDir) return;
        curNode_ = node;
        if (!query_.empty())
        {
            query_.clear();
            syncingUi_ = true;
            SearchBox().Text(L"");
            syncingUi_ = false;
        }
        if (syncTree) ExpandTreeTo(node);
        listFxUntil_ = GetTickCount64() + 350;
        RefreshList();
    }

    static int EstimateNameLines(std::wstring const& name, double width);

    void MainWindow::RefreshList(bool keepSelection)
    {
        if (!tree_) return;
        std::set<uint32_t> keep;
        if (keepSelection)
        {
            if (!keepOverride_.empty()) keep.insert(keepOverride_.begin(), keepOverride_.end());
            else for (auto n : SelectedNodes()) keep.insert(n);
            keepOverride_.clear();
        }

        const ULONGLONG t0 = GetTickCount64();
        const bool showPath = flat_ || !query_.empty();
        std::vector<uint32_t> ids = !query_.empty() ? tree_->Search(curNode_, query_, sortKey_, sortAsc_)
                                    : flat_         ? tree_->Descendants(curNode_, sortKey_, sortAsc_, false)  // list everything: only files
                                                    : tree_->Children(curNode_, sortKey_, sortAsc_);
        std::vector<Windows::Foundation::IInspectable> rows;
        rows.reserve(ids.size());
        const bool thumbMode = ThumbsActive();
        const ThumbDims td = ThumbDimsFor(settings_.thumbSize);
        bool anyImage = false;   // thumbnails are shown and this folder has pictures
        if (thumbMode)
            for (uint32_t id : ids)
                if (auto const& n = tree_->Node(id); !n.isDir && lycan_app::IsPreviewableImageName(n.name)) { anyImage = true; break; }
        // The cells of the icon view: all the same size (so that they line up), the size that was chosen (Settings > View): a picture shows its thumbnail as big as the
        // picture area, any other file its icon nearly as big; the name gets as many lines as the longest name needs (at most 2, the narrow cells 3)
        (void)anyImage;
        const double cellW = td.item, cellBox = td.boxH, iconDip = IconDipFor(settings_.thumbSize);
        int nameLines = 1;
        if (viewMode_ == lycan::ViewMode::Icons)
        {
            const int maxLines = cellW <= 104.0 ? 3 : 2;
            for (uint32_t id : ids)
            {
                nameLines = std::max(nameLines, EstimateNameLines(tree_->Node(id).name, cellW - 8.0));
                if (nameLines >= maxLines) { nameLines = maxLines; break; }
            }
        }
        else nameLines = 3;
        const bool solidArchive = current_ && current_->Caps().solid;
        for (uint32_t id : ids)
        {
            auto const& n = tree_->Node(id);
            auto row = make<implementation::EntryRow>(
                id, hstring{n.name}, showPath ? hstring{tree_->ParentPath(id)} : hstring{},
                n.isDir ? hstring{lycan::Tf(L"fmt.n_items_short", n.children.size())} : hstring{FormatSize(n.size)},
                n.isDir ? hstring{} : hstring{FormatSize(n.packedSize)},
                n.mtimeFileTime ? hstring{FormatTime(n.mtimeFileTime)} : hstring{}, n.isDir, n.isEncrypted && !passwordVerified_, showPath);   // after the password was given the lock is gone
            // the columns that are off by default: ratio (a solid archive does not tell the packed size of an item), CRC, method, the lock
            hstring ratioText, crcText, methodText;
            if (!n.isDir)
            {
                ratioText = (n.size == 0 || n.packedSize == 0 || solidArchive) ? hstring{L"\u2014"} : hstring{std::to_wstring(std::lround(100.0 * static_cast<double>(n.packedSize) / static_cast<double>(n.size))) + L"%"};
                if (n.hasCrc) { wchar_t b[16]; swprintf_s(b, L"%08X", n.crc); crcText = b; }
                methodText = hstring{tree_->MethodName(n.methodIdx)};
            }
            row.SetExtra(ratioText, crcText, methodText, n.isEncrypted);
            ApplyShellIcon(row);
            row.SetLayout(cellW, cellBox, nameLines, iconDip);
            rows.push_back(row);
        }
        const ULONGLONG t1 = GetTickCount64();
        entries_ = single_threaded_vector<Windows::Foundation::IInspectable>(std::move(rows));
        ApplyViewMode();  // after switching the view, the other control must release its data source
        auto list = ActiveList();
        list.ItemsSource(entries_);
        const ULONGLONG t2 = GetTickCount64();
        if (!keep.empty())
            for (auto const& it : entries_)
                if (keep.count(it.as<LycanZip::EntryRow>().NodeId())) list.SelectedItems().Append(it);

        RefreshShellIcons();   // the kinds of file that have no icon yet are loaded now
        UpdateListColumns();
        if (ids.size() >= 5000 || GetTickCount64() - t0 >= 300) lycan::diag::Info(L"list{}: {} rows, {} ms to get and build them, {} ms to hand them to the list, {} ms for the rest", query_.empty() ? L"" : L" (search)", ids.size(), t1 - t0, t2 - t1, GetTickCount64() - t2);
        auto label = [&](TextBlock const& tb, const wchar_t* base, lycan::SortKey k) {
            std::wstring s = base;
            if (sortKey_ == k) s += sortAsc_ ? L" ▲" : L" ▼";
            tb.Text(hstring{s});
        };
        label(SortNameText(), lycan::Tr(L"main.name"), lycan::SortKey::Name);
        label(SortSizeText(), lycan::Tr(L"main.size"), lycan::SortKey::Size);
        label(SortPackedText(), lycan::Tr(L"main.packed"), lycan::SortKey::PackedSize);
        label(SortModifiedText(), lycan::Tr(L"main.modified"), lycan::SortKey::Modified);
        label(SortRatioText(), lycan::Tr(L"main.ratio"), lycan::SortKey::Ratio);
        label(SortEncText(), lycan::Tr(L"main.col_encrypted"), lycan::SortKey::Encrypted);
        label(SortMethodText(), lycan::Tr(L"main.col_method"), lycan::SortKey::Method);
        label(SortCrcText(), lycan::Tr(L"main.col_crc"), lycan::SortKey::Crc);
        label(SortTypeText(), lycan::Tr(L"main.type"), lycan::SortKey::Type);
        label(SortPathText(), lycan::Tr(L"main.location"), lycan::SortKey::Path);

        // Breadcrumb
        crumbNodes_.clear();
        for (uint32_t x = curNode_;; x = tree_->Node(x).parent)
        {
            crumbNodes_.push_back(x);
            if (x == tree_->Root()) break;
        }
        std::reverse(crumbNodes_.begin(), crumbNodes_.end());
        std::vector<Windows::Foundation::IInspectable> names;
        for (uint32_t x : crumbNodes_)
            names.push_back(make<implementation::CrumbItem>(x == tree_->Root() ? hstring{currentPath_.filename().wstring()} : hstring{tree_->Node(x).name}, x == curNode_));
        Crumbs().ItemsSource(single_threaded_vector<Windows::Foundation::IInspectable>(std::move(names)));
        UpButton().IsEnabled(curNode_ != tree_->Root());
        UpdateViewMenuChecks();

        std::wstring status = lycan::Tf(L"fmt.n_items", ids.size());
        if (!query_.empty()) status = lycan::Tf(L"main.found_matching_including", ids.size(), query_);
        else if (flat_) status += lycan::Tr(L"main.list_all_suffix");
        const std::wstring noThumbs = thumbMode && anyImage ? ThumbUnavailableReason() : std::wstring{};
        if (!noThumbs.empty()) status += std::wstring{lycan::Tr(L"fmt.dot_gap")} + noThumbs;  // a permanent note: nothing is broken, this kind of archive makes no thumbnails
        status = archiveInfo_ + std::wstring{lycan::Tr(L"fmt.bar_gap")} + status;
        ArchiveStatus().Text(hstring{status});
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(ArchiveStatus(), box_value(hstring{status}));
        UpdateSelectionUi();
        if (!noThumbs.empty() && !thumbNoteShown_)
        {
            thumbNoteShown_ = true;
            ShowStatusNote(lycan::Tf(L"main.image_thumbnails_on_but_double", noThumbs));
        }
    }

    // How many lines a name needs in a cell of this width (an estimate: CJK characters are about twice as wide as Latin ones; a little room is kept for the word breaks)
    static int EstimateNameLines(std::wstring const& name, double width)
    {
        double w = 0;
        for (wchar_t c : name) w += (c >= 0x2E80 || (c >= 0xFF00 && c <= 0xFFEF)) ? 14.0 : (c == L' ' ? 4.0 : 7.6);
        return std::max(1, static_cast<int>(std::ceil(w / (width * 0.9))));
    }

    // The size of the icon of a file that is not a picture in the icon view: nearly as big as the picture area
    double MainWindow::IconDipFor(unsigned size)
    {
        const ThumbDims td = ThumbDimsFor(size);
        return std::round((std::min)(td.boxH, td.item - 16.0) * 0.8);
    }

    // Size of the thumbnails of the icon view: item width, height of the thumbnail area, upper limit of the decoded width
    MainWindow::ThumbDims MainWindow::ThumbDimsFor(unsigned size)
    {
        switch (size)
        {
            case 0: return {104.0, 60.0, 128};
            case 2: return {208.0, 156.0, 320};
            case 3: return {312.0, 234.0, 480};   // 1.5 times the large one (a 4K screen)
            default: return {136.0, 96.0, 192};
        }
    }

    std::wstring MainWindow::ThumbUnavailableReason() const
    {
        if (!current_) return L"";
        auto caps = current_->Caps();
        if (caps.solid) return lycan::Tr(L"main.archive_solid_so_no_thumbnails");
        if (!caps.randomAccess) return lycan::Tr(L"main.archive_format_cannot_read_single");
        return L"";
    }

    // The reading session is made when a file is first needed (an encrypted archive waits until the password is verified)
    bool MainWindow::EnsureThumbSession()
    {
        if (!current_ || !ThumbUnavailableReason().empty()) return false;
        if (current_->Caps().encrypted && !passwordVerified_) return false;
        const std::wstring key = currentPath_.wstring() + L"|" + password_;
        if (key != thumbSessionKey_)
        {
            thumbs_.Reset(currentPath_, MakeOpenOptions(), true);
            thumbSessionKey_ = key;
        }
        return true;
    }

    void MainWindow::BindThumb(LycanZip::EntryRow const& row)
    {
        if (!row || !tree_ || !ThumbsActive() || row.NodeId() >= tree_->NodeCount()) return;
        auto const& n = tree_->Node(row.NodeId());
        if (n.isDir || (n.isEncrypted && !passwordVerified_) || n.entryIndex == lycan::kNoEntry || !lycan_app::CanPreviewImage(n.name) || lycan::IsMacMetadataPath(n.path)) return;
        if (!EnsureThumbSession()) return;
        const uint32_t px = ThumbDimsFor(settings_.thumbSize).px;
        if (auto img = thumbs_.Get(n.entryIndex, px)) { row.SetThumb(img.as<Microsoft::UI::Xaml::Media::ImageSource>()); return; }
        thumbs_.Request(n.entryIndex, n.size, px, [row](Windows::Foundation::IInspectable const& i) {
            if (auto src = i.try_as<Microsoft::UI::Xaml::Media::ImageSource>()) row.SetThumb(src);
        });
    }

    void MainWindow::OnCrumbClicked(BreadcrumbBar const&, BreadcrumbBarItemClickedEventArgs const& args)
    {
        auto i = static_cast<size_t>(args.Index());
        if (i < crumbNodes_.size()) NavigateTo(crumbNodes_[i], true);
    }

    void MainWindow::OnUpClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (tree_ && curNode_ != tree_->Root()) NavigateTo(tree_->Node(curNode_).parent, true);
    }

    void MainWindow::OnSortClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        if (!tree_) return;
        static constexpr lycan::SortKey kKeys[] = {lycan::SortKey::Name, lycan::SortKey::Size, lycan::SortKey::PackedSize, lycan::SortKey::Modified, lycan::SortKey::Type, lycan::SortKey::Path,
                                                   lycan::SortKey::Ratio, lycan::SortKey::Encrypted, lycan::SortKey::Crc, lycan::SortKey::Method};
        int i = _wtoi(unbox_value<hstring>(sender.as<FrameworkElement>().Tag()).c_str());
        lycan::SortKey k = kKeys[std::clamp(i, 0, 9)];
        if (k == sortKey_) sortAsc_ = !sortAsc_;
        else { sortKey_ = k; sortAsc_ = true; }
        RefreshList(true);  // sorting must not clear the items the user has ticked
    }

    void MainWindow::OnSearchTextChanged(Windows::Foundation::IInspectable const&, TextChangedEventArgs const&)
    {
        if (syncingUi_ || !tree_) return;
        if (!searchTimer_)
        {
            searchTimer_ = dispatcher_.CreateTimer();
            searchTimer_.Interval(250ms);  // search only after typing pauses, so a large archive does not recompute at every key
            searchTimer_.IsRepeating(false);
            searchTimer_.Tick([weak = get_weak()](auto&&, auto&&) {
                auto self = weak.get();
                if (!self || !self->tree_) return;
                std::wstring q{self->SearchBox().Text()};
                while (!q.empty() && iswspace(q.back())) q.pop_back();
                size_t b = 0;
                while (b < q.size() && iswspace(q[b])) ++b;
                q.erase(0, b);
                if (q == self->query_) return;
                self->query_ = q;
                self->RefreshList();
            });
        }
        searchTimer_.Stop();
        searchTimer_.Start();
    }

    void MainWindow::OnFlatToggleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (syncingUi_ || !tree_) return;
        flat_ = FlatToggle().IsChecked();
        RefreshList();
    }

    // View menu: show the clutter macOS adds (__MACOSX, ._ files, .DS_Store)
    void MainWindow::OnMacMetaItemClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (syncingUi_) return;
        settings_.hideMacMetadata = !MacMetaItem().IsChecked();
        const bool was = loadingSettings_;
        loadingSettings_ = true;
        HideMacMetaToggle().IsOn(settings_.hideMacMetadata);
        loadingSettings_ = was;
        SaveSettings();
        RebuildTree();
    }

    // A new tree of the open archive (the entries are the same, only the hidden ones differ); stays in the same folder
    void MainWindow::RebuildTree()
    {
        if (!current_ || !tree_) return;
        const std::wstring where = curNode_ < tree_->NodeCount() ? tree_->Node(curNode_).path : std::wstring();
        lycan_app::CloseImagePreview(this);  // the previews hold node numbers of the old tree
        lycan_app::CloseAudioPreview(this);
        tree_ = std::make_shared<lycan::ArchiveTree>(current_->Entries(), settings_.hideMacMetadata);
        tree_->SetMethodNames(current_->MethodNames());
        BuildFolderTree();
        uint32_t n = where.empty() ? tree_->Root() : tree_->Find(where);
        if (n == lycan::kNoNode || !tree_->Node(n).isDir) n = tree_->Root();
        NavigateTo(n, true);
    }

    void MainWindow::OnTreeToggleClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (syncingUi_) return;
        treeWanted_ = TreeToggle().IsChecked();
        UpdateTreeVisibility();
    }

    void MainWindow::OnRootSizeChanged(Windows::Foundation::IInspectable const&, SizeChangedEventArgs const&)
    {
        UpdateTreeVisibility();
        UpdateCompressLayout();
        UpdateTaskPanelMax();
        UpdateListColumns();
        LayoutHomeScene();
        LayoutArchiveStrip();
        UpdateToolbarText();
    }

    // Settings tabs: one group of settings at a time (the header and the tab strip stay on top)
    void MainWindow::OnSettingsTabChanged(SelectorBar const& bar, SelectorBarSelectionChangedEventArgs const&)
    {
        const auto sel = bar.SelectedItem();
        const std::pair<SelectorBarItem, UIElement> tabs[] = {
            {TabItemGeneral(), TabGeneral()}, {TabItemCompress(), TabCompress()}, {TabItemView(), TabView()},
            {TabItemShell(), TabShell()},     {TabItemKeys(), TabKeys()},         {TabItemAbout(), TabAbout()}};
        for (auto const& t : tabs)
        {
            const bool show = sel == t.first;
            const bool wasHidden = t.second.Visibility() == Visibility::Collapsed;
            t.second.Visibility(show ? Visibility::Visible : Visibility::Collapsed);
            if (show && wasHidden) FadeIn(t.second, 8, 180);   // the tab's content fades in
        }
        SettingsScroll().ChangeView(nullptr, 0.0, nullptr, true);
    }

    // When the window is too narrow the folder tree is hidden automatically to leave the room for the list
    void MainWindow::UpdateTreeVisibility()
    {
        bool show = treeWanted_ && RootGrid().ActualWidth() >= 640;
        TreePane().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        TreeCol().Width({show ? static_cast<double>(settings_.treeWidth) : 0.0, GridUnitType::Pixel});
        SplitCol().Width({show ? 10.0 : 0.0, GridUnitType::Pixel});
        TreeSplitter().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
    }

    // The splitter of the folder tree (drag to change the width, double click to restore) and the context menu of the column headers (choose which columns to show)
    void MainWindow::InitTreeSplitter()
    {
        auto splitter = TreeSplitter();
        auto weak = get_weak();
        splitter.PointerEntered([weak](auto&&, auto&&) { if (auto s = weak.get()) s->TreeSplitterLine().Opacity(0.6); });
        splitter.PointerExited([weak](auto&&, auto&&) { if (auto s = weak.get()) if (!s->treeDragging_) s->TreeSplitterLine().Opacity(0.0); });
        splitter.PointerPressed([weak](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s) return;
            s->treeDragging_ = true;
            s->treeDragStartX_ = e.GetCurrentPoint(s->RootGrid()).Position().X;
            s->treeDragStartW_ = s->settings_.treeWidth;
            sender.as<UIElement>().CapturePointer(e.Pointer());
            s->TreeSplitterLine().Opacity(1.0);
            e.Handled(true);
        });
        splitter.PointerMoved([weak](Windows::Foundation::IInspectable const&, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s || !s->treeDragging_) return;
            const double dx = e.GetCurrentPoint(s->RootGrid()).Position().X - s->treeDragStartX_;
            const double maxW = std::min(420.0, std::max(140.0, s->RootGrid().ActualWidth() * 0.5));
            const double w = std::clamp(s->treeDragStartW_ + dx, 140.0, maxW);
            s->TreeCol().Width({w, GridUnitType::Pixel});
            s->treeWidth_ = w;
        });
        auto finish = [weak](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
            auto s = weak.get();
            if (!s || !s->treeDragging_) return;
            s->treeDragging_ = false;
            sender.as<UIElement>().ReleasePointerCapture(e.Pointer());
            s->TreeSplitterLine().Opacity(0.0);
            s->settings_.treeWidth = static_cast<unsigned>(std::lround(s->treeWidth_));
            s->SaveSettings();
        };
        splitter.PointerReleased(finish);
        splitter.DoubleTapped([weak](auto&&, auto&&) {
            if (auto s = weak.get()) { s->settings_.treeWidth = 200; s->SaveSettings(); s->UpdateTreeVisibility(); }
        });

        // Right click on a column header: tick the columns to show (the name is always shown)
        ListHeader().ContextRequested([weak](UIElement const& sender, Input::ContextRequestedEventArgs const& args) {
            auto s = weak.get();
            if (!s) return;
            args.Handled(true);
            MenuFlyout fly;
            struct Col { const wchar_t* text; unsigned bit; };
            for (Col c : {Col{lycan::Tr(L"main.type"), 1}, Col{lycan::Tr(L"main.size"), 2}, Col{lycan::Tr(L"main.packed"), 4}, Col{lycan::Tr(L"main.ratio"), 16}, Col{lycan::Tr(L"main.modified"), 8},
                          Col{lycan::Tr(L"main.col_encrypted"), 32}, Col{lycan::Tr(L"main.col_method"), 128}, Col{lycan::Tr(L"main.col_crc"), 64}})
            {
                ToggleMenuFlyoutItem it;
                it.Text(c.text);
                it.IsChecked((s->settings_.listColumns & c.bit) != 0);
                it.Click([weak, bit = c.bit](auto&&, auto&&) {
                    if (auto self = weak.get())
                    {
                        self->settings_.listColumns ^= bit;
                        self->SaveSettings();
                        self->UpdateListColumns();
                    }
                });
                fly.Items().Append(it);
            }
            fly.Items().Append(MenuFlyoutSeparator{});
            MenuFlyoutItem reset;
            reset.Text(lycan::Tr(L"main.reset_column_widths"));
            reset.Click([weak](auto&&, auto&&) { if (auto self = weak.get()) self->ResetColWidths(); });
            fly.Items().Append(reset);
            Windows::Foundation::Point pt{};
            if (args.TryGetPosition(sender, pt))
            {
                Primitives::FlyoutShowOptions opt;
                opt.Position(pt);
                fly.ShowAt(sender, opt);
            }
            else fly.ShowAt(sender.as<FrameworkElement>());
        });
    }

    void MainWindow::OnFindAccelerator(Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& args)
    {
        if (view_ != View::Archive) return;
        SearchBox().Focus(FocusState::Programmatic);
        args.Handled(true);
    }

    ListViewBase MainWindow::ActiveList()
    {
        return viewMode_ == lycan::ViewMode::Details ? EntryList().as<ListViewBase>() : EntryGrid().as<ListViewBase>();
    }

    // A ListView for the details (with column headers); list / icons share one GridView, only the template and the panel change, so selection, sorting and right click logic are exactly the same
    void MainWindow::ApplyViewMode()
    {
        const bool details = viewMode_ == lycan::ViewMode::Details;
        EntryList().Visibility(details ? Visibility::Visible : Visibility::Collapsed);
        HeaderViewport().Visibility(details ? Visibility::Visible : Visibility::Collapsed);
        EntryGrid().Visibility(details ? Visibility::Collapsed : Visibility::Visible);
        (details ? EntryGrid().as<ListViewBase>() : EntryList().as<ListViewBase>()).ItemsSource(nullptr);
        if (details) return;
        const bool icons = viewMode_ == lycan::ViewMode::Icons;
        if (!icons)
        {
            // list: a GridViewItem is very tall by default (about 96px), make it the same row height as the details
            try
            {
                Style style{xaml_typename<GridViewItem>()};
                style.BasedOn(Application::Current().Resources().Lookup(box_value(L"DefaultGridViewItemStyle")).as<Style>());
                style.Setters().Append(Setter(FrameworkElement::MinHeightProperty(), box_value(settings_.compactRows ? 30.0 : 40.0)));
                style.Setters().Append(Setter(FrameworkElement::MinWidthProperty(), box_value(0.0)));
                EntryGrid().ItemContainerStyle(style);
            }
            catch (...) {}
        }
        else
        {
            // icons: leave a gap of 4 between items (the gap belongs to no item, so pressing and dragging there is a band selection); remove the default minimum size and padding
            try
            {
                Style style{xaml_typename<GridViewItem>()};
                style.BasedOn(Application::Current().Resources().Lookup(box_value(L"DefaultGridViewItemStyle")).as<Style>());
                style.Setters().Append(Setter(FrameworkElement::MarginProperty(), box_value(ThicknessHelper::FromUniformLength(4))));
                style.Setters().Append(Setter(Control::PaddingProperty(), box_value(ThicknessHelper::FromUniformLength(0))));
                style.Setters().Append(Setter(FrameworkElement::MinWidthProperty(), box_value(0.0)));
                style.Setters().Append(Setter(FrameworkElement::MinHeightProperty(), box_value(0.0)));
                EntryGrid().ItemContainerStyle(style);
            }
            catch (...) { EntryGrid().ItemContainerStyle(nullptr); }
        }
        auto res = RootGrid().Resources();
        EntryGrid().ItemTemplate(res.Lookup(box_value(icons ? L"IconItemTemplate" : L"ListItemTemplate")).as<DataTemplate>());
        EntryGrid().ItemsPanel(res.Lookup(box_value(icons ? L"IconPanel" : L"ListPanel")).as<ItemsPanelTemplate>());
        // icons: laid out horizontally, scrolls vertically; list: top to bottom, then to the right, scrolls horizontally (like the "List" of Explorer)
        ScrollViewer::SetHorizontalScrollBarVisibility(EntryGrid(), icons ? ScrollBarVisibility::Disabled : ScrollBarVisibility::Auto);
        ScrollViewer::SetHorizontalScrollMode(EntryGrid(), icons ? ScrollMode::Disabled : ScrollMode::Enabled);
        ScrollViewer::SetVerticalScrollBarVisibility(EntryGrid(), icons ? ScrollBarVisibility::Auto : ScrollBarVisibility::Disabled);
        ScrollViewer::SetVerticalScrollMode(EntryGrid(), icons ? ScrollMode::Enabled : ScrollMode::Disabled);
    }

    void MainWindow::UpdateViewMenuChecks()
    {
        syncingUi_ = true;
        ViewDetailsItem().IsChecked(viewMode_ == lycan::ViewMode::Details);
        ViewListItem().IsChecked(viewMode_ == lycan::ViewMode::List);
        ThumbsItem().IsChecked(settings_.iconThumbnails);
        ThumbsItem().Text(ThumbUnavailableReason().empty() ? hstring{lycan::Tr(L"main.show_image_thumbnails_icon_view")}
                                                           : hstring{lycan::Tf(L"main.show_image_thumbnails_icon_view_not_supported_by_archive", current_ && current_->Caps().solid ? lycan::Tr(L"main.solid_compression") : lycan::Tr(L"main.format_limitation"))});
        MacMetaItem().IsChecked(!settings_.hideMacMetadata);
        const bool icons = viewMode_ == lycan::ViewMode::Icons;
        ThumbSmallItem().IsChecked(icons && settings_.thumbSize == 0);
        ThumbMediumItem().IsChecked(icons && settings_.thumbSize == 1);
        ThumbLargeItem().IsChecked(icons && settings_.thumbSize == 2);
        ThumbHugeItem().IsChecked(icons && settings_.thumbSize == 3);
        SortNameItem().IsChecked(sortKey_ == lycan::SortKey::Name);
        SortSizeItem().IsChecked(sortKey_ == lycan::SortKey::Size);
        SortPackedItem().IsChecked(sortKey_ == lycan::SortKey::PackedSize);
        SortModifiedItem().IsChecked(sortKey_ == lycan::SortKey::Modified);
        SortTypeItem().IsChecked(sortKey_ == lycan::SortKey::Type);
        SortAscItem().IsChecked(sortAsc_);
        SortDescItem().IsChecked(!sortAsc_);
        syncingUi_ = false;
    }

    // The "View" menu: view mode / sort key / ascending or descending (the first letter of Tag is the category)
    void MainWindow::OnViewMenuClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        if (syncingUi_ || !tree_) return;
        std::wstring tag{unbox_value<hstring>(sender.as<FrameworkElement>().Tag())};
        if (tag.size() < 2) return;
        ApplyViewChoice(tag[0], tag[1] - L'0');
    }

    void MainWindow::ApplyViewChoice(wchar_t category, int n)
    {
        if (!tree_) return;
        keepOverride_ = SelectedNodes();  // must be taken before viewMode_ changes (afterwards ActiveList() points to the other control)
        static constexpr lycan::SortKey kKeys[] = {lycan::SortKey::Name, lycan::SortKey::Size, lycan::SortKey::PackedSize, lycan::SortKey::Modified, lycan::SortKey::Type};
        const std::wstring tag{category};
        if (tag[0] == L'v' || tag[0] == L'i')   // v = details / list / icons; i = the icon view in a size (small .. huge)
        {
            viewMode_ = tag[0] == L'i' ? lycan::ViewMode::Icons : static_cast<lycan::ViewMode>(std::clamp(n, 0, 2));
            bool save = false;
            if (tag[0] == L'i' && settings_.thumbSize != static_cast<unsigned>(std::clamp(n, 0, 3)))
            {
                settings_.thumbSize = static_cast<unsigned>(std::clamp(n, 0, 3));   // the size is a setting of its own (the default view of the settings page uses it too)
                save = true;
            }
            if (settings_.rememberViewMode && settings_.viewMode != viewMode_)
            {
                settings_.viewMode = viewMode_;  // remember the view mode chosen last
                save = true;
            }
            if (save) { SaveSettings(); LoadSettingsToUi(); }
        }
        else if (tag[0] == L't') { settings_.iconThumbnails = !settings_.iconThumbnails; thumbNoteShown_ = false; SaveSettings(); }
        else if (tag[0] == L's') sortKey_ = kKeys[std::clamp(n, 0, 4)];
        else if (tag[0] == L'd') sortAsc_ = n != 0;
        RefreshList(true);  // keep the selection
    }

    // Row height: compact / comfortable. It has to be ItemContainerStyle to take effect at once; it must be BasedOn the default style, otherwise the items lose their template and become empty
    void MainWindow::ApplyDensity()
    {
        try
        {
            Style style{xaml_typename<ListViewItem>()};
            style.BasedOn(Application::Current().Resources().Lookup(box_value(L"DefaultListViewItemStyle")).as<Style>());
            style.Setters().Append(Setter(FrameworkElement::MinHeightProperty(), box_value(settings_.compactRows ? 30.0 : 40.0)));
            style.Setters().Append(Setter(Control::HorizontalContentAlignmentProperty(), box_value(HorizontalAlignment::Stretch)));
            style.Setters().Append(Setter(Control::PaddingProperty(), box_value(ThicknessHelper::FromLengths(8, 0, 8, 0))));  // the default left and right padding is too wide; line up with the 8 of the column headers
            EntryList().ItemContainerStyle(style);
        }
        catch (...) {}  // if the default style is not found keep things as they are (default row height)
    }

    // ---- Selection

    // Checkbox (shown when the mouse is over it or it is selected): the pointer events are attached once to each item container, Click once to the checkbox
    void MainWindow::OnContainerChanging(ListViewBase const&, ContainerContentChangingEventArgs const& args)
    {
        if (args.InRecycleQueue())
        {
            // an item that left the screen: release the thumbnail and cancel reads that have not started (the thumbnail stays in the cache, which has a limit)
            if (auto row = args.Item().try_as<LycanZip::EntryRow>())
            {
                row.SetThumb(nullptr);
                if (tree_ && row.NodeId() < tree_->NodeCount()) thumbs_.Cancel(tree_->Node(row.NodeId()).entryIndex);
            }
            return;
        }
        auto item = args.ItemContainer();
        if (GetTickCount64() < listFxUntil_) FadeIn(item, 6, 170, static_cast<int>(std::min<uint32_t>(static_cast<uint32_t>(args.ItemIndex()), 10)) * 15);
        auto rowOf = [](Windows::Foundation::IInspectable const& s) {
            auto it = s.try_as<Primitives::SelectorItem>();
            return it ? it.Content().try_as<LycanZip::EntryRow>() : nullptr;
        };
        if (!item.Tag())
        {
            item.Tag(box_value(L"hooked"));
            item.PointerEntered([rowOf](Windows::Foundation::IInspectable const& s, Input::PointerRoutedEventArgs const&) {
                if (auto row = rowOf(s)) get_self<implementation::EntryRow>(row)->IsHover(true);
            });
            item.PointerExited([rowOf](Windows::Foundation::IInspectable const& s, Input::PointerRoutedEventArgs const&) {
                if (auto row = rowOf(s)) get_self<implementation::EntryRow>(row)->IsHover(false);
            });
        }
        if (auto row = rowOf(item)) get_self<implementation::EntryRow>(row)->IsHover(false);  // when a container is reused clear the old hover state
        auto hook = [weak = get_weak()](ListViewBase const&, ContainerContentChangingEventArgs const& a) {
            auto self = weak.get();
            if (!self || a.InRecycleQueue()) return;
            auto root = a.ItemContainer().ContentTemplateRoot().try_as<FrameworkElement>();
            if (root) self->ApplyRowColumns(root, a.Item().try_as<LycanZip::EntryRow>());
            self->BindThumb(a.Item().try_as<LycanZip::EntryRow>());
            auto cb = root ? root.FindName(L"RowCheck").try_as<CheckBox>() : nullptr;
            if (!cb || cb.Tag()) return;
            cb.Tag(box_value(L"hooked"));
            cb.Click([weak](Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) {
                auto me = weak.get();
                auto box = s.try_as<CheckBox>();
                if (!me || !box) return;
                auto row = RowFromSource(box);
                if (!row) return;
                auto sel = me->ActiveList().SelectedItems();
                uint32_t idx = 0;
                bool present = sel.IndexOf(row, idx);
                if (box.IsChecked() && box.IsChecked().Value()) { if (!present) sel.Append(row); }
                else if (present) sel.RemoveAt(idx);
            });
        };
        if (args.Phase() == 0) args.RegisterUpdateCallback(hook);
        else hook(nullptr, args);
    }

    void MainWindow::OnEntrySelectionChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const& args)
    {
        for (auto const& it : args.RemovedItems())
            if (auto row = it.try_as<LycanZip::EntryRow>()) get_self<implementation::EntryRow>(row)->IsSelected(false);
        for (auto const& it : args.AddedItems())
            if (auto row = it.try_as<LycanZip::EntryRow>()) get_self<implementation::EntryRow>(row)->IsSelected(true);
        UpdateSelectionUi();
    }

    void MainWindow::ShowStatusNote(std::wstring const& text)
    {
        StatusNoteText().Text(hstring{text});
        StatusNote().Visibility(Visibility::Visible);
        SelectionBar().Visibility(Visibility::Collapsed);
        statusNoteActive_ = true;
        lastNoteSelection_ = SelectedNodes();
        if (!statusNoteTimer_)
        {
            statusNoteTimer_ = dispatcher_.CreateTimer();
            statusNoteTimer_.IsRepeating(false);
            statusNoteTimer_.Interval(std::chrono::seconds(15));
            statusNoteTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) { s->HideStatusNote(); s->UpdateSelectionUi(); } });
        }
        statusNoteTimer_.Stop();
        statusNoteTimer_.Start();
    }

    void MainWindow::HideStatusNote()
    {
        if (statusNoteTimer_) statusNoteTimer_.Stop();
        statusNoteActive_ = false;
        StatusNote().Visibility(Visibility::Collapsed);
    }

    void MainWindow::UpdateSelectionUi()
    {
        if (statusNoteActive_ && lastNoteSelection_ == SelectedNodes()) return;  // a message is shown and the selection did not change: keep the message
        HideStatusNote();
        if (!tree_ || !entries_ || !current_) { SelectionBar().Visibility(Visibility::Collapsed); return; }
        auto nodes = SelectedNodes();
        const size_t count = nodes.size();
        SelectionBar().Visibility(count ? Visibility::Visible : Visibility::Collapsed);
        if (count)
        {
            uint64_t bytes = 0;
            auto const& all = current_->Entries();
            for (uint32_t i : tree_->EntryIndices(nodes))
                if (!all[i].isDir) bytes += all[i].size;
            SelectionText().Text(hstring{lycan::Tf(L"main.selected", count, FormatSize(bytes))});
        }
        if (count == 0) SelectAllBox().IsChecked(false);
        else if (count == entries_.Size()) SelectAllBox().IsChecked(true);
        else SelectAllBox().IsChecked(nullptr);  // partly selected: the indeterminate state
    }

    void MainWindow::OnSelectAllClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!entries_) return;
        if (SelectedNodes().size() == entries_.Size()) ActiveList().SelectedItems().Clear();
        else ActiveList().SelectAll();
        UpdateSelectionUi();
    }

    void MainWindow::OnClearSelectionClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ActiveList().SelectedItems().Clear();
    }

    void MainWindow::OnCopyPathClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { CopyPaths(SelectedNodes()); }

    void MainWindow::CopyPaths(std::vector<uint32_t> const& nodes)
    {
        if (!tree_ || nodes.empty()) return;
        std::wstring text;
        for (uint32_t n : nodes) text += (text.empty() ? L"" : L"\r\n") + tree_->Node(n).path;
        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetText(hstring{text});
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
    }

    // ---- Open / double click / keyboard

    LycanZip::EntryRow MainWindow::RowFromSource(Windows::Foundation::IInspectable const& source)
    {
        auto d = source.try_as<DependencyObject>();
        while (d)
        {
            if (auto item = d.try_as<Primitives::SelectorItem>()) return item.Content().try_as<LycanZip::EntryRow>();
            d = Media::VisualTreeHelper::GetParent(d);
        }
        return nullptr;
    }

    void MainWindow::OpenNode(uint32_t node)
    {
        if (!tree_ || node >= tree_->NodeCount()) return;
        auto const& n = tree_->Node(node);
        if (n.isDir) NavigateTo(node, true);
        else if (settings_.doubleClickPreview && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableImageName(n.name))
        {
            if (lycan_app::IsImageDecoderAvailable(n.name)) PreviewNode(node);
            else
            {
                // a known picture format, but Windows has no decoder (or it is not installed): open with the associated program directly and say why below
                const std::wstring name = n.name;
                OpenEntryFileAsync(node);
                ShowStatusNote(lycan::Tf(L"main.opened_with_associated_app_instead", name, lycan_app::MissingDecoderHint(name)));
            }
        }
        else if (settings_.doubleClickAudioPreview && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableAudioName(n.name)) PreviewAudio(node);
        else OpenEntryFileAsync(node);
    }

    void MainWindow::OnEntryDoubleTapped(Windows::Foundation::IInspectable const&, Input::DoubleTappedRoutedEventArgs const& args)
    {
        for (auto d = args.OriginalSource().try_as<DependencyObject>(); d && !d.try_as<Primitives::SelectorItem>(); d = Media::VisualTreeHelper::GetParent(d))
            if (d.try_as<CheckBox>()) return;  // repeated clicks on the checkbox do not count as "open"
        if (auto row = RowFromSource(args.OriginalSource()))
        {
            args.Handled(true);
            OpenNode(row.NodeId());
        }
    }

    void MainWindow::OnEntryKeyDown(Windows::Foundation::IInspectable const&, Input::KeyRoutedEventArgs const& args)
    {
        if (!tree_) return;
        if (args.Key() == Windows::System::VirtualKey::Enter)
        {
            auto nodes = SelectedNodes();
            if (nodes.size() == 1) { args.Handled(true); OpenNode(nodes[0]); }
        }
        else if (args.Key() == Windows::System::VirtualKey::Back)
        {
            args.Handled(true);
            OnUpClick(nullptr, nullptr);
        }
        else if (args.Key() == Windows::System::VirtualKey::Delete)
        {
            auto nodes = SelectedNodes();
            if (!nodes.empty()) { args.Handled(true); DeleteNodesAsync(std::move(nodes)); }
        }
        else if (args.Key() == Windows::System::VirtualKey::F2)
        {
            auto nodes = SelectedNodes();
            if (nodes.size() == 1) { args.Handled(true); RenameNodeAsync(nodes[0]); }
        }
    }

    // "Open with the default program": first extract this one file to a temporary folder (keeping Zone.Identifier), when done hand it to the shell to open
    fire_and_forget MainWindow::OpenEntryFileAsync(uint32_t node)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || node >= tree_->NodeCount()) co_return;
        auto const& n = tree_->Node(node);
        if (n.isDir || n.entryIndex == lycan::kNoEntry) co_return;
        const uint32_t entryIndex = n.entryIndex;
        const std::wstring name = n.name;
        const std::wstring parent = tree_->ParentPath(node);
        const uint32_t parentNode = n.parent;        // copied: `n` must not be used after a dialog (the tree may be rebuilt)
        const fs::path archivePath = currentPath_;

        auto safe = lycan::SanitizeEntryPath(name);
        if (safe.rejected)
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), hstring{lycan::Tf(L"main.file_s_name_unsafe_was_rejected", safe.reason.Text())});
            co_return;
        }
        // Programs, installers and scripts: ask first (unless turned off), and extract their folder so that they find
        // the files they depend on. Everything else: just the one file.
        const bool exec = lycan::IsExecutableName(name);
        bool withFolder = false;
        // nothing else in its folder: there is no scope to choose (and nothing extra to extract)
        const bool alone = parentNode != lycan::kNoNode && tree_->Node(parentNode).fileCount <= 1;
        if (exec)
        {
            withFolder = settings_.execScope == lycan::OpenScope::Folder;
            if (settings_.warnExecutable)
            {
                auto dlg = MakeDialog(lycan::Tr(L"main.run_program"), L"");
                StackPanel sp;
                sp.Spacing(12);
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.comes_from_archive_will_run", name)});
                tb.TextWrapping(TextWrapping::Wrap);
                tb.MaxWidth(440);
                sp.Children().Append(tb);
                RadioButtons scope;
                if (!alone) scope.Header(box_value(lycan::Tr(L"main.which_files_should_be_extracted")));
                scope.Items().Append(TitledOption(lycan::Tr(L"main.files_same_folder_recommended"), lycan::Tr(L"main.needed_when_program_uses_files")));
                scope.Items().Append(TitledOption(lycan::Tr(L"main.extract_only_file"), lycan::Tr(L"main.faster_but_program_may_not_find")));
                scope.SelectedIndex(withFolder ? 0 : 1);
                if (!alone) sp.Children().Append(scope);
                CheckBox never;
                never.Content(box_value(lycan::Tr(L"main.do_not_warn_again_can_be")));
                sp.Children().Append(never);
                dlg.Content(sp);
                dlg.PrimaryButtonText(lycan::Tr(L"main.extract_run"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Close);
                if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
                if (!alone)
                {
                    withFolder = scope.SelectedIndex() == 0;
                    settings_.execScope = withFolder ? lycan::OpenScope::Folder : lycan::OpenScope::FileOnly;
                }
                if (never.IsChecked().Value()) settings_.warnExecutable = false;
                LoadSettingsToUi();
                SaveSettings();
                if (!current_ || !tree_ || currentPath_ != archivePath || node >= tree_->NodeCount()) co_return;  // the archive changed while the question was open
            }
        }

        // The folder the program is in (with subfolders). More than 1 GB: ask before extracting that much.
        std::vector<uint32_t> entries{entryIndex};
        if (withFolder && !alone && parentNode != lycan::kNoNode)
        {
            const uint64_t folderBytes = tree_->Node(parentNode).totalSize;
            bool useFolder = true;
            if (folderBytes > kExecFolderWarnBytes)
            {
                auto dlg = MakeDialog(lycan::Tr(L"main.folder_large"), L"");
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.folder_program_extract_all", FormatSize(folderBytes))});
                tb.TextWrapping(TextWrapping::Wrap);
                tb.MaxWidth(420);
                dlg.Content(tb);
                dlg.PrimaryButtonText(lycan::Tr(L"main.extract_whole_folder"));
                dlg.SecondaryButtonText(lycan::Tr(L"main.extract_only_file"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Secondary);
                const auto r = co_await ShowDialogAsync(dlg);
                if (r == ContentDialogResult::None) co_return;
                useFolder = r == ContentDialogResult::Primary;
                if (!current_ || !tree_ || currentPath_ != archivePath || node >= tree_->NodeCount()) co_return;
            }
            if (useFolder) entries = tree_->EntryIndices({parentNode});
        }
        if (!co_await EnsurePasswordAsync()) co_return;

        static std::atomic<uint32_t> seq{0};
        std::error_code ec;
        if (lycan_app::OpenTempRoot().empty()) ec = std::make_error_code(std::errc::no_such_file_or_directory);
        fs::path dir = lycan_app::OpenTempRoot() /
                       (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++seq));
        if (ec)
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), lycan::Tr(L"main.temporary_folder_not_found"));
            co_return;
        }

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        eo.overwrite = lycan::OverwriteMode::Overwrite;  // a new folder, no conflict possible
        eo.stripPrefix = parent.empty() ? std::wstring() : parent + L"/";
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };

        TaskMeta meta;
        meta.archive = currentPath_;
        meta.openFile = dir / safe.relative;
        std::lock_guard lk(metaM_);
        uint64_t id = queue_.EnqueueExtract(currentPath_, dir, eo, MakeOpenOptions(), entries);
        taskMeta_[id] = std::move(meta);
    }

    // ---------------------------------------------------------------- Columns of the details view

    // The scroll viewer inside a list control (the list keeps its own vertical scroll bar; the horizontal one is switched on in XAML)
    static ScrollViewer FindScrollViewer(DependencyObject const& root)
    {
        const int n = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChildrenCount(root);
        for (int i = 0; i < n; ++i)
        {
            auto c = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetChild(root, i);
            if (auto sv = c.try_as<ScrollViewer>()) return sv;
            if (auto r = FindScrollViewer(c)) return r;
        }
        return nullptr;
    }

    // The names of the columns in settings.ini (list_col_widths=name=320,type=70)
    static const char* const kColKeys[] = {"name", "path", "type", "size", "packed", "ratio", "modified", "encrypted", "method", "crc"};

    void MainWindow::LoadColWidths()
    {
        for (int c = 0; c < LcCount; ++c) colW_[c] = kLcDefault[c];
        const std::string& text = settings_.listColWidths;
        size_t pos = 0;
        while (pos < text.size())
        {
            size_t end = text.find(',', pos);
            if (end == std::string::npos) end = text.size();
            const std::string item = text.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = item.find('=');
            if (eq == std::string::npos) continue;
            for (int c = 0; c < LcCount; ++c)
                if (item.compare(0, eq, kColKeys[c]) == 0)
                {
                    try { colW_[c] = std::clamp(static_cast<double>(std::stoi(item.substr(eq + 1))), kLcMin[c], kLcMax); } catch (...) {}
                }
        }
    }

    // What the user dragged: only the columns that are not at their default width
    std::string MainWindow::ColWidthsText() const
    {
        std::string out;
        for (int c = 0; c < LcCount; ++c)
            if (std::abs(colW_[c] - kLcDefault[c]) >= 1.0)
                out += (out.empty() ? "" : ",") + std::string(kColKeys[c]) + "=" + std::to_string(static_cast<int>(colW_[c] + 0.5));
        return out;
    }

    // Which columns are shown: the ticked ones, and the folder column in "list everything" and in the search results. No column is hidden because the
    // window is narrow any more: the list scrolls sideways (the header with it).
    MainWindow::ListCols MainWindow::ComputeListCols()
    {
        ListCols c;
        c.path = flat_ || !query_.empty();
        c.type = (settings_.listColumns & 1) != 0;
        c.size = (settings_.listColumns & 2) != 0;
        c.packed = (settings_.listColumns & 4) != 0;
        c.modified = (settings_.listColumns & 8) != 0;
        c.ratio = (settings_.listColumns & 16) != 0;
        c.encrypted = (settings_.listColumns & 32) != 0;
        c.crc = (settings_.listColumns & 64) != 0;
        c.method = (settings_.listColumns & 128) != 0;
        return c;
    }

    void MainWindow::ApplyRowColumns(FrameworkElement const& root, LycanZip::EntryRow const&)
    {
        auto grid = root.try_as<Grid>();
        if (!grid || grid.ColumnDefinitions().Size() != static_cast<uint32_t>(LcCount + 1)) return;
        auto cols = grid.ColumnDefinitions();
        grid.Width(rowW_ > 0 ? rowW_ : std::numeric_limits<double>::quiet_NaN());   // the width of all columns; the list scrolls when it is wider than the window
        for (int c = 0; c < LcCount; ++c) cols.GetAt(static_cast<uint32_t>(c + 1)).Width(GridLength{curW_[c], GridUnitType::Pixel});
        const auto& v = colVis_;
        auto show = [&](const wchar_t* name, bool on) {
            if (auto el = root.FindName(name).try_as<UIElement>()) el.Visibility(on ? Visibility::Visible : Visibility::Collapsed);
        };
        show(L"RowFolder", v.path);
        show(L"RowType", v.type);
        show(L"RowSize", v.size);
        show(L"RowPacked", v.packed);
        show(L"RowRatio", v.ratio);
        show(L"RowModified", v.modified);
        show(L"RowEnc", v.encrypted);
        show(L"RowMethod", v.method);
        show(L"RowCrc", v.crc);
    }

    // The header follows the list when it scrolls sideways
    void MainWindow::HookListScroll()
    {
        if (listScroll_) return;
        auto sv = FindScrollViewer(EntryList());
        if (!sv) return;
        listScroll_ = sv;
        sv.ViewChanged([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) s->HeaderShift().X(-s->listScroll_.HorizontalOffset()); });
        HeaderShift().X(-sv.HorizontalOffset());
    }

    void MainWindow::UpdateListColumns()
    {
        const ListCols v = ComputeListCols();
        const ListCols before = colVis_;
        const bool first = !colsInit_;
        colVis_ = v;
        colsInit_ = true;
        HookListScroll();

        // The widths (DIP): what the user chose, or the defaults. The name takes what is left when everything fits; otherwise the row is as wide as all columns.
        const bool on[LcCount] = {true, v.path, v.type, v.size, v.packed, v.ratio, v.modified, v.encrypted, v.method, v.crc};
        double w[LcCount], sum = 0;
        for (int c = 0; c < LcCount; ++c) { w[c] = on[c] ? colW_[c] : 0.0; sum += w[c]; }
        const double viewport = EntryList().ActualWidth();
        const double fixedPart = 24 + 16;   // the check box column, and the padding left and right of the row (the header has the same)
        double content = fixedPart + sum;
        if (viewport > 0 && content < viewport)
        {
            w[LcName] += viewport - content;
            content = viewport;
        }
        bool changed = first || !(before == v) || std::abs(rowW_ - (content - 16)) >= 0.5;
        for (int c = 0; c < LcCount && !changed; ++c) changed = std::abs(w[c] - curW_[c]) >= 0.5;
        for (int c = 0; c < LcCount; ++c) curW_[c] = w[c];
        rowW_ = content - 16;

        // Column headers
        auto px = [](double x) { return GridLength{x, GridUnitType::Pixel}; };
        auto vis = [](bool b) { return b ? Visibility::Visible : Visibility::Collapsed; };
        ListHeader().Width(content);
        HeaderNameCol().Width(px(curW_[LcName]));
        HeaderPathCol().Width(px(curW_[LcPath]));
        HeaderTypeCol().Width(px(curW_[LcType]));
        HeaderSizeCol().Width(px(curW_[LcSize]));
        HeaderPackedCol().Width(px(curW_[LcPacked]));
        HeaderRatioCol().Width(px(curW_[LcRatio]));
        HeaderModifiedCol().Width(px(curW_[LcModified]));
        HeaderEncCol().Width(px(curW_[LcEncrypted]));
        HeaderMethodCol().Width(px(curW_[LcMethod]));
        HeaderCrcCol().Width(px(curW_[LcCrc]));
        SortPathButton().Visibility(vis(v.path));
        SortTypeButton().Visibility(vis(v.type));
        SortSizeButton().Visibility(vis(v.size));
        SortPackedButton().Visibility(vis(v.packed));
        SortModifiedButton().Visibility(vis(v.modified));
        SortRatioButton().Visibility(vis(v.ratio));
        SortEncButton().Visibility(vis(v.encrypted));
        SortMethodButton().Visibility(vis(v.method));
        SortCrcButton().Visibility(vis(v.crc));
        for (int c = 0; c < LcCount; ++c)
            if (colGrip_[c]) colGrip_[c].Visibility(vis(on[c]));

        if (!changed || viewMode_ != lycan::ViewMode::Details) return;
        // Rows that exist already (rows that were not virtualized in are handled in ContainerContentChanging)
        if (auto panel = EntryList().ItemsPanelRoot())
            for (auto const& child : panel.Children())
                if (auto item = child.try_as<Primitives::SelectorItem>())
                    if (auto root = item.ContentTemplateRoot().try_as<FrameworkElement>())
                        ApplyRowColumns(root, item.Content().try_as<LycanZip::EntryRow>());
    }

    // The header: a grip at the right edge of every column to drag its width (double click = the default width), and the clip that keeps
    // the scrolled header inside the list
    void MainWindow::InitListHeader()
    {
        LoadColWidths();
        for (int c = 0; c < LcCount; ++c) curW_[c] = kLcDefault[c];
        for (int c = 0; c < LcCount; ++c)
        {
            Border grip;
            grip.Width(10);
            grip.HorizontalAlignment(HorizontalAlignment::Right);
            grip.Margin(ThicknessHelper::FromLengths(0, -2, -5, -2));
            grip.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
            Microsoft::UI::Xaml::Shapes::Rectangle line;
            line.Width(2);
            line.RadiusX(1);
            line.RadiusY(1);
            line.HorizontalAlignment(HorizontalAlignment::Center);
            line.Margin(ThicknessHelper::FromLengths(0, 4, 0, 4));
            line.Opacity(0);
            line.Fill(Application::Current().Resources().Lookup(box_value(L"AccentFillColorDefaultBrush")).as<Microsoft::UI::Xaml::Media::Brush>());
            grip.Child(line);
            Automation::AutomationProperties::SetName(grip, lycan::Tr(L"main.resize_column"));
            Grid::SetColumn(grip, static_cast<int>(c) + 1);
            auto weak = get_weak();
            grip.PointerEntered([weak, line](auto&&, auto&&) { if (auto s = weak.get()) if (s->colDrag_ < 0) line.Opacity(0.6); });
            grip.PointerExited([weak, line, c](auto&&, auto&&) { if (auto s = weak.get()) if (s->colDrag_ != c) line.Opacity(0); });
            grip.PointerPressed([weak, line, c](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s) return;
                s->colDrag_ = c;
                s->colDragX_ = e.GetCurrentPoint(nullptr).Position().X;
                s->colDragW_ = s->curW_[c];
                sender.as<UIElement>().CapturePointer(e.Pointer());
                line.Opacity(1.0);
                e.Handled(true);
            });
            grip.PointerMoved([weak, c](auto&&, Input::PointerRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s || s->colDrag_ != c) return;
                const double dx = e.GetCurrentPoint(nullptr).Position().X - s->colDragX_;
                s->colW_[c] = std::clamp(s->colDragW_ + dx, kLcMin[c], kLcMax);
                s->UpdateListColumns();
                e.Handled(true);
            });
            auto finish = [weak, line, c](Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s || s->colDrag_ != c) return;
                s->colDrag_ = -1;
                sender.as<UIElement>().ReleasePointerCaptures();
                line.Opacity(0);
                s->settings_.listColWidths = s->ColWidthsText();
                s->SaveSettings();
                e.Handled(true);
            };
            grip.PointerReleased(finish);
            grip.PointerCanceled(finish);
            grip.DoubleTapped([weak, c](auto&&, Input::DoubleTappedRoutedEventArgs const& e) {
                auto s = weak.get();
                if (!s) return;
                s->colW_[c] = kLcDefault[c];
                s->settings_.listColWidths = s->ColWidthsText();
                s->SaveSettings();
                s->UpdateListColumns();
                e.Handled(true);
            });
            ListHeader().Children().Append(grip);
            colGrip_[c] = grip;
        }
        HeaderViewport().SizeChanged([weak = get_weak()](auto&&, SizeChangedEventArgs const& e) {
            auto s = weak.get();
            if (!s) return;
            Microsoft::UI::Xaml::Media::RectangleGeometry clip;
            clip.Rect(Windows::Foundation::Rect{0, 0, e.NewSize().Width, e.NewSize().Height});
            s->HeaderViewport().Clip(clip);
        });
    }

    void MainWindow::ResetColWidths()
    {
        for (int c = 0; c < LcCount; ++c) colW_[c] = kLcDefault[c];
        settings_.listColWidths.clear();
        SaveSettings();
        UpdateListColumns();
    }

    // ---------------------------------------------------------------- Band selection with the mouse

    void MainWindow::OnBandPressed(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        auto list = sender.try_as<ListViewBase>();
        if (!list || bandActive_ || !tree_) return;
        auto pt = e.GetCurrentPoint(list);
        if (pt.PointerDeviceType() != Microsoft::UI::Input::PointerDeviceType::Mouse || !pt.Properties().IsLeftButtonPressed()) return;

        // Pressing on a scroll bar, checkbox, icon or text is handled as usual (select / drag out / scroll).
        // Pressing on an empty part of an item (for example right of the name): note it first, and the band selection starts after a small move; a simple click still selects that row.
        bool onItem = false, onContent = false, onSelectedItem = false;
        for (auto const& hit : Microsoft::UI::Xaml::Media::VisualTreeHelper::FindElementsInHostCoordinates(e.GetCurrentPoint(nullptr).Position(), list))
        {
            if (hit.try_as<Primitives::ScrollBar>() || hit.try_as<Primitives::ButtonBase>() || hit.try_as<CheckBox>()) return;
            if (auto si = hit.try_as<Primitives::SelectorItem>()) { onItem = true; onSelectedItem = si.IsSelected(); }
            else if (hit.try_as<TextBlock>() || hit.try_as<FontIcon>() || hit.try_as<Image>()) onContent = true;
        }
        if (onItem && onContent) return;
        if (onItem && onSelectedItem) return;  // grabbing a selected row anywhere drags the selection (the band starts on unselected rows)
        if (onItem)
        {
            bandPending_ = true;
            bandPressPos_ = pt.Position();
            bandList_ = list;
            return;
        }

        bandScroll_ = FindScrollViewer(list);
        if (!bandScroll_) return;
        bandList_ = list;
        const auto mods = e.KeyModifiers();
        const bool additive = (static_cast<uint32_t>(mods) & (static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Control) |
                                                              static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Shift))) != 0;
        bandBase_.clear();
        bandHit_.clear();
        if (additive)
        {
            for (auto const& it : list.SelectedItems())
            {
                uint32_t idx;
                if (list.Items().IndexOf(it, idx)) bandBase_.insert(static_cast<int32_t>(idx));
            }
        }
        else list.SelectedItems().Clear();  // a click on an empty part = clear the selection (as in Explorer)

        const auto p = pt.Position();
        bandStart_ = {static_cast<float>(p.X + bandScroll_.HorizontalOffset()), static_cast<float>(p.Y + bandScroll_.VerticalOffset())};
        bandLast_ = p;
        bandActive_ = true;
        bandVisible_ = false;
        list.CapturePointer(e.Pointer());
        e.Handled(true);

        if (!bandTimer_)
        {
            bandTimer_ = dispatcher_.CreateTimer();
            bandTimer_.Interval(30ms);
            bandTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) if (s->bandActive_) s->UpdateBand(s->bandLast_); });
        }
        bandTimer_.Start();
    }

    void MainWindow::OnBandMoved(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        if (bandPending_ && !bandActive_)
        {
            auto list = sender.try_as<ListViewBase>();
            auto pt = e.GetCurrentPoint(list);
            if (!list || list != bandList_ || !pt.Properties().IsLeftButtonPressed()) { bandPending_ = false; return; }
            const auto p = pt.Position();
            if (std::abs(p.X - bandPressPos_.X) < 3 && std::abs(p.Y - bandPressPos_.Y) < 3) return;
            bandPending_ = false;
            bandScroll_ = FindScrollViewer(list);
            if (!bandScroll_) return;
            const auto mods = e.KeyModifiers();
            const bool additive = (static_cast<uint32_t>(mods) & (static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Control) |
                                                                  static_cast<uint32_t>(Windows::System::VirtualKeyModifiers::Shift))) != 0;
            bandBase_.clear();
            bandHit_.clear();
            if (additive)
            {
                for (auto const& it : list.SelectedItems())
                {
                    uint32_t idx;
                    if (list.Items().IndexOf(it, idx)) bandBase_.insert(static_cast<int32_t>(idx));
                }
            }
            else list.SelectedItems().Clear();
            bandStart_ = {static_cast<float>(bandPressPos_.X + bandScroll_.HorizontalOffset()), static_cast<float>(bandPressPos_.Y + bandScroll_.VerticalOffset())};
            bandLast_ = p;
            bandActive_ = true;
            bandVisible_ = false;
            list.CapturePointer(e.Pointer());
            if (!bandTimer_)
            {
                bandTimer_ = dispatcher_.CreateTimer();
                bandTimer_.Interval(30ms);
                bandTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) if (s->bandActive_) s->UpdateBand(s->bandLast_); });
            }
            bandTimer_.Start();
        }
        if (!bandActive_) return;
        auto list = sender.try_as<ListViewBase>();
        if (!list || list != bandList_) return;
        bandLast_ = e.GetCurrentPoint(list).Position();
        UpdateBand(bandLast_);
    }

    // The item under the pointer starts its own drag recognizer a few pixels after the press and takes the pointer capture from
    // the band. While the button is still down that is not the end of the band: take the capture back (the item drag itself
    // is cancelled by DragItemsStarting while the band runs).
    void MainWindow::OnBandCaptureLost(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        auto list = sender.try_as<ListViewBase>();
        if (bandActive_ && !bandRecapturing_ && list && list == bandList_ && e.GetCurrentPoint(list).Properties().IsLeftButtonPressed())
        {
            bandRecapturing_ = true;
            list.CapturePointer(e.Pointer());
            bandRecapturing_ = false;
            return;
        }
        OnBandReleased(sender, e);
    }

    void MainWindow::OnBandReleased(Windows::Foundation::IInspectable const& sender, Input::PointerRoutedEventArgs const& e)
    {
        bandPending_ = false;
        if (!bandActive_) return;
        if (bandList_) bandList_.ReleasePointerCaptures();
        EndBand();
    }

    void MainWindow::EndBand()
    {
        bandActive_ = false;
        bandVisible_ = false;
        if (bandTimer_) bandTimer_.Stop();
        BandLayer().Visibility(Visibility::Collapsed);
        bandHit_.clear();
        bandBase_.clear();
    }

    // Updates the band selection by the pointer position: near the edge it scrolls automatically; items inside the band (the ones that are realized) are selected, outside ones are unselected
    void MainWindow::UpdateBand(Windows::Foundation::Point pos)
    {
        auto list = bandList_;
        if (!list || !bandScroll_) return;
        const double w = list.ActualWidth(), h = list.ActualHeight();

        // auto scroll
        constexpr double kEdge = 28, kStep = 22;
        double dx = 0, dy = 0;
        if (pos.Y < kEdge) dy = -kStep; else if (pos.Y > h - kEdge) dy = kStep;
        if (pos.X < kEdge) dx = -kStep; else if (pos.X > w - kEdge) dx = kStep;
        if (dx != 0 || dy != 0)
        {
            const double nx = std::clamp(bandScroll_.HorizontalOffset() + dx, 0.0, bandScroll_.ScrollableWidth());
            const double ny = std::clamp(bandScroll_.VerticalOffset() + dy, 0.0, bandScroll_.ScrollableHeight());
            bandScroll_.ChangeView(nx, ny, nullptr, true);
        }

        const double offX = bandScroll_.HorizontalOffset(), offY = bandScroll_.VerticalOffset();
        const double cx = pos.X + offX, cy = pos.Y + offY;  // current position (content coordinates)
        const double l = std::min<double>(bandStart_.X, cx), t = std::min<double>(bandStart_.Y, cy);
        const double r = std::max<double>(bandStart_.X, cx), b = std::max<double>(bandStart_.Y, cy);
        if (!bandVisible_ && (r - l) < 4 && (b - t) < 4) return;  // not really dragged yet
        bandVisible_ = true;

        // selection rectangle (view coordinates, clamped to the list area)
        const double vl = std::max(0.0, l - offX), vt = std::max(0.0, t - offY);
        const double vr = std::min(w, r - offX), vb = std::min(h, b - offY);
        BandLayer().Visibility(Visibility::Visible);
        auto accent = AccentColorNow();
        BandRect().Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x38, accent.R, accent.G, accent.B}});
        BandRect().Stroke(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0xC0, accent.R, accent.G, accent.B}});
        Canvas::SetLeft(BandRect(), vl);
        Canvas::SetTop(BandRect(), vt);
        BandRect().Width(std::max(0.0, vr - vl));
        BandRect().Height(std::max(0.0, vb - vt));

        auto panel = list.ItemsPanelRoot();
        if (!panel) return;
        const Windows::Foundation::Rect whole{0, 0, static_cast<float>(w), static_cast<float>(h)};
        for (auto const& child : panel.Children())
        {
            auto item = child.try_as<Primitives::SelectorItem>();
            if (!item) continue;
            const int32_t idx = list.IndexFromContainer(item);
            if (idx < 0) continue;
            auto fe = item.as<FrameworkElement>();
            auto bounds = item.TransformToVisual(list).TransformBounds(Windows::Foundation::Rect{0, 0, static_cast<float>(fe.ActualWidth()), static_cast<float>(fe.ActualHeight())});
            const double il = bounds.X + offX, it = bounds.Y + offY, ir = il + bounds.Width, ib = it + bounds.Height;
            const bool hit = il < r && ir > l && it < b && ib > t;
            if (hit) bandHit_.insert(idx); else bandHit_.erase(idx);
            const bool want = hit || bandBase_.count(idx) || bandHit_.count(idx);
            if (item.IsSelected() != want)
            {
                Microsoft::UI::Xaml::Data::ItemIndexRange range{idx, 1};
                if (want) list.SelectRange(range); else list.DeselectRange(range);
            }
        }
        (void)whole;
    }

    // ---------------------------------------------------------------- Keyboard shortcuts

    bool MainWindow::FocusInTextInput()
    {
        auto f = Input::FocusManager::GetFocusedElement(RootGrid().XamlRoot());
        return f && (f.try_as<TextBox>() || f.try_as<PasswordBox>() || f.try_as<RichEditBox>());
    }

    void MainWindow::ChooseViewMode(int n)
    {
        if (!tree_) return;
        keepOverride_ = SelectedNodes();
        viewMode_ = static_cast<lycan::ViewMode>(std::clamp(n, 0, 2));
        RefreshList(true);
    }

    void MainWindow::InitShortcuts()
    {
        using Windows::System::VirtualKey;
        using Windows::System::VirtualKeyModifiers;
        const auto N = VirtualKeyModifiers::None;
        const auto C = VirtualKeyModifiers::Control;
        const auto CS = static_cast<VirtualKeyModifiers>(static_cast<uint32_t>(VirtualKeyModifiers::Control) | static_cast<uint32_t>(VirtualKeyModifiers::Shift));
        const auto A = VirtualKeyModifiers::Menu;
        const auto AS = static_cast<VirtualKeyModifiers>(static_cast<uint32_t>(VirtualKeyModifiers::Menu) | static_cast<uint32_t>(VirtualKeyModifiers::Shift));

        struct Def
        {
            const wchar_t* group;
            const wchar_t* label;
            const wchar_t* keys;
            VirtualKey key;                  // None = only shown in the table (handled elsewhere)
            VirtualKeyModifiers mods;
            std::function<bool(MainWindow&)> act;  // returns true = handled (otherwise the key is passed on to the control as usual)
        };
        auto inArchive = [](MainWindow& w) { return w.view_ == View::Archive && w.current_ && w.tree_ && !w.dialogOpen_; };
        auto hasSel = [](MainWindow& w) { return !w.SelectedNodes().empty(); };

        std::vector<Def> defs;
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.open_archive_on_new_archive_page"), L"Ctrl + O", VirtualKey::O, C, [](MainWindow& w) {
            if (w.dialogOpen_) return false;
            if (w.view_ == View::Compress) w.AddSourcesFromPicker(false); else w.OnOpenArchiveClick(nullptr, nullptr);
            return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.new_archive_choose_files"), L"Ctrl + N", VirtualKey::N, C, [](MainWindow& w) { if (w.dialogOpen_) return false; w.OnNewFromFilesClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.new_archive_choose_folder"), L"Ctrl + Shift + N", VirtualKey::N, CS, [](MainWindow& w) { if (w.dialogOpen_) return false; w.OnNewFromFolderClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.open_close_settings"), L"Ctrl + ,", static_cast<VirtualKey>(188), C, [](MainWindow& w) { if (w.dialogOpen_) return false; w.OnSettingsClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.go_back_close_current_page"), L"Esc", VirtualKey::Escape, N, [](MainWindow& w) {
            if (w.dialogOpen_) return false;
            if (w.view_ == View::Settings) { w.OnSettingsBackClick(nullptr, nullptr); return true; }
            if (w.view_ == View::Compress) { w.LeaveCompressPage(); return true; }
            if (w.view_ == View::Archive && !w.FocusInTextInput() && !w.SelectedNodes().empty()) { w.ActiveList().SelectedItems().Clear(); return true; }
            return false; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.show_collapse_task_panel"), L"Ctrl + J", VirtualKey::J, C, [](MainWindow& w) {
            if (w.TaskPanel().Visibility() != Visibility::Visible) return false;
            w.OnTaskCollapseClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.clear_finished_tasks"), L"Ctrl + Shift + Delete", VirtualKey::Delete, CS, [](MainWindow& w) {
            if (w.TaskPanel().Visibility() != Visibility::Visible) return false;
            w.OnClearFinishedClick(nullptr, nullptr); return true; }});

        defs.push_back({lycan::Tr(L"main.global"), lycan::Tr(L"main.paste_files_folders_new_archive"), L"Ctrl + V", VirtualKey::V, C, [](MainWindow& w) { return w.TryPasteFiles(); }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.search"), L"Ctrl + F", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.select_all"), L"Ctrl + A", VirtualKey::A, C, [inArchive](MainWindow& w) {
            if (!inArchive(w) || w.FocusInTextInput()) return false;
            w.ActiveList().SelectAll(); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.open_selected_item_file_enter"), L"Enter", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.rename_selected_item_zip_7z_tar"), L"F2", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.delete_selected_items_from_archive"), L"Delete", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.up_one_level"), L"Backspace  /  Alt + ↑", VirtualKey::Up, A, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.OnUpClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.extract_here_only_selected_items"), L"Ctrl + E", VirtualKey::E, C, [inArchive, hasSel](MainWindow& w) {
            if (!inArchive(w)) return false;
            if (hasSel(w)) w.OnExtractSelHereClick(nullptr, nullptr); else w.OnExtractHereClick(nullptr, nullptr);
            return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.extract_only_selection_when"), L"Ctrl + Shift + E", VirtualKey::E, CS, [inArchive, hasSel](MainWindow& w) {
            if (!inArchive(w)) return false;
            if (hasSel(w)) w.OnExtractSelToClick(nullptr, nullptr); else w.OnExtractToClick(nullptr, nullptr);
            return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.test_integrity"), L"Ctrl + T", VirtualKey::T, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.OnTestClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.copy_path"), L"Ctrl + Shift + C", VirtualKey::C, CS, [inArchive, hasSel](MainWindow& w) {
            if (!inArchive(w) || w.FocusInTextInput() || !hasSel(w)) return false;
            w.CopyPaths(w.SelectedNodes()); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.image_preview_selected_image_first"), L"Alt + P", VirtualKey::P, A, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.PreviewImages(); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.audio_preview_selected_audio_file"), L"Alt + Shift + P", VirtualKey::P, AS, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.PreviewAudio(); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.properties_with_nothing_selected"), L"Alt + Enter", VirtualKey::Enter, A, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            auto nodes = w.SelectedNodes();
            if (nodes.empty()) { w.ShowArchiveInfoAsync(); return true; }  // nothing selected: the archive itself
            if (nodes.size() != 1) return false;
            w.ShowProperties(nodes); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.view_details"), L"Ctrl + 1", VirtualKey::Number1, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.ChooseViewMode(0); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.view_list"), L"Ctrl + 2", VirtualKey::Number2, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.ChooseViewMode(1); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.view_icons"), L"Ctrl + 3", VirtualKey::Number3, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.ChooseViewMode(2); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.list_all_flatten_all_levels"), L"Ctrl + Shift + F", VirtualKey::F, CS, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.FlatToggle().IsChecked(!w.flat_); w.OnFlatToggleClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.show_hide_folder_tree"), L"Ctrl + B", VirtualKey::B, C, [inArchive](MainWindow& w) {
            if (!inArchive(w)) return false;
            w.TreeToggle().IsChecked(!w.treeWanted_); w.OnTreeToggleClick(nullptr, nullptr); return true; }});
        defs.push_back({lycan::Tr(L"main.archive_view"), lycan::Tr(L"main.close_archive_return_home_page"), L"Ctrl + W", VirtualKey::W, C, [inArchive](MainWindow& w) { if (!inArchive(w)) return false; w.OnBackClick(nullptr, nullptr); return true; }});

        defs.push_back({lycan::Tr(L"main.new_archive_page"), lycan::Tr(L"main.start_compressing"), L"Ctrl + Enter", VirtualKey::Enter, C, [](MainWindow& w) {
            if (w.view_ != View::Compress || w.dialogOpen_) return false;
            w.OnCompressStart(); return true; }});
        defs.push_back({lycan::Tr(L"main.new_archive_page"), lycan::Tr(L"main.add_folder_button"), L"Ctrl + Shift + O", VirtualKey::O, CS, [](MainWindow& w) {
            if (w.view_ != View::Compress || w.dialogOpen_) return false;
            w.AddSourcesFromPicker(true); return true; }});
        defs.push_back({lycan::Tr(L"main.new_archive_page"), lycan::Tr(L"main.save_as_new_template"), L"Ctrl + S", VirtualKey::S, C, [](MainWindow& w) {
            if (w.view_ != View::Compress || w.dialogOpen_) return false;
            w.OnPresetSaveNew(); return true; }});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.previous_next"), lycan::Tr(L"main.space_pageup_pagedown_mouse_wheel"), VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.first_last"), L"Home / End", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.view_mode_fit_window_actual_size"), L"Ctrl + 0  /  Ctrl + 1  /  Ctrl + 2  /  Ctrl + 3  F", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.zoom_out_when_zoomed_drag_use"), lycan::Tr(L"main.ctrl_wheel"), VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.rotate_changes_display_only"), L"R / Shift + R", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.full_screen_thumbnail_strip"), L"F11  /  T  Ctrl + B", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.copy_image_copy_path"), L"Ctrl + C  /  Ctrl + Shift + C", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.save_image_as_open_with_associated"), L"Ctrl + S  Ctrl + E  /  Enter", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.animations_gif_animated_webp_play"), L"P", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.image_info_size_format_photo_info"), L"I  /  Alt + Enter", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.menu_button_at_top_right_right"), lycan::Tr(L"main.right_click"), VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.image_preview_window"), lycan::Tr(L"main.close_leaves_full_screen_first"), L"Esc", VirtualKey::None, N, nullptr});
        defs.push_back({lycan::Tr(L"main.dialogs"), lycan::Tr(L"main.ok_cancel"), L"Enter  /  Esc", VirtualKey::None, N, nullptr});

        // Register the shortcuts
        auto weak = get_weak();
        for (auto const& d : defs)
        {
            if (d.key == VirtualKey::None || !d.act) continue;
            Input::KeyboardAccelerator ka;
            ka.Key(d.key);
            ka.Modifiers(d.mods);
            ka.Invoked([weak, act = d.act](Input::KeyboardAccelerator const&, Input::KeyboardAcceleratorInvokedEventArgs const& args) {
                if (auto s = weak.get()) args.Handled(act(*s));
            });
            RootGrid().KeyboardAccelerators().Append(ka);
        }

        // The table of the settings page: one card for each group, the action on the left, the keys on the right
        auto panel = ShortcutPanel();
        const wchar_t* lastGroup = nullptr;
        StackPanel rows{nullptr};
        for (auto const& d : defs)
        {
            if (!lastGroup || wcscmp(lastGroup, d.group) != 0)
            {
                lastGroup = d.group;
                panel.Children().Append(Caption(d.group, 0.75));
                rows = StackPanel{};
                rows.Spacing(10);
                Border card;
                card.Style(RootGrid().Resources().Lookup(box_value(L"SettingCard")).as<Style>());
                card.Child(rows);
                panel.Children().Append(card);
            }
            Grid row;
            row.ColumnSpacing(16);
            ColumnDefinition c0, c1;
            c0.Width(GridLength{1, GridUnitType::Star});
            c1.Width(GridLength{0, GridUnitType::Auto});
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            TextBlock label;
            label.Text(d.label);
            label.TextWrapping(TextWrapping::Wrap);
            label.VerticalAlignment(VerticalAlignment::Center);
            row.Children().Append(label);
            Border cap;
            cap.CornerRadius(CornerRadiusHelper::FromUniformRadius(5));
            cap.Padding(ThicknessHelper::FromLengths(10, 3, 10, 3));
            cap.BorderThickness(ThicknessHelper::FromUniformLength(1));
            cap.BorderBrush(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x50, 0x80, 0x80, 0x80}});
            cap.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x20, 0x80, 0x80, 0x80}});
            cap.VerticalAlignment(VerticalAlignment::Center);
            TextBlock keys;
            keys.Text(d.keys);
            keys.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{L"Consolas, Segoe UI"});
            keys.FontSize(13);
            cap.Child(keys);
            Grid::SetColumn(cap, 1);
            row.Children().Append(cap);
            rows.Children().Append(row);
        }

        // The tooltips of buttons show the shortcut
        auto tip = [](FrameworkElement const& el, const wchar_t* text) { ToolTipService::SetToolTip(el, box_value(hstring{text})); };
        tip(OpenButton(), lycan::Tr(L"main.open_archive_ctrl_o"));
        tip(NewButton(), lycan::Tr(L"main.new_archive_ctrl_n"));
        tip(SettingsButton(), lycan::Tr(L"main.settings_ctrl"));
        tip(BackButton(), lycan::Tr(L"main.back_home_ctrl_w"));
        tip(ExtractToButton(), lycan::Tr(L"main.extract_ctrl_shift_e"));
        tip(UpButton(), lycan::Tr(L"main.up_one_level_backspace_alt"));
        tip(TaskCollapseButton(), lycan::Tr(L"main.collapse_expand_ctrl_j"));
        tip(CStartButton(), lycan::Tr(L"main.start_compressing_ctrl_enter"));
        tip(CCancelButton(), lycan::Tr(L"main.cancel_esc"));
        tip(CBackButton(), lycan::Tr(L"main.back_esc"));
        tip(SettingsBackButton(), lycan::Tr(L"main.back_esc"));
    }

    // ---- Paths dragged in / opened

    // A single archive -> open. A single .exe: try to open it as an archive; if the format is not an ordinary PE (a self-extracting archive or an installer
    // that contains 7z / zip data) ask "open the content" or "add to an archive" (asked every time); an ordinary program .exe is simply a file to compress. Anything else -> add to an archive.
    fire_and_forget MainWindow::HandleDroppedPaths(std::vector<fs::path> paths)
    {
        auto strong = get_strong();
        if (paths.size() == 1 && IsArchivePath(paths[0])) { OpenFromUi(paths[0]); co_return; }
        // an archive that can be changed is on screen: what is dropped goes into it (into the folder that is shown)
        if (view_ == View::Archive && current_ && tree_ && !compact_ && current_->Caps().canModify)
        {
            AddPathsToArchive(std::move(paths), CurrentFolderPath());
            co_return;
        }
        // checksum files: check them (against the archive that is on screen, when there is one)
        if (!paths.empty() && std::all_of(paths.begin(), paths.end(), [](fs::path const& p) { return lycan::IsSumFile(p); }))
        {
            if (view_ == View::Archive && current_ && !compact_) ShowVerifyDialogAsync(std::move(paths), currentPath_);
            else QueueVerify(std::move(paths), false);
            co_return;
        }
        if (paths.size() == 1 && IsExePath(paths[0]))
        {
            const fs::path file = paths[0];
            co_await resume_background();
            std::wstring format;
            try { format = lycan::ArchiveSource::Open(file)->FormatName(); } catch (...) {}
            co_await resume_ui{dispatcher_};
            if (!format.empty() && format != L"PE")
            {
                auto dlg = MakeDialog(lycan::Tr(L"main.self_extracting_archive"), L"\uE7B8");
                StackPanel sp;
                sp.Spacing(10);
                sp.MinWidth(420);
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.contains_compressed_data_format", file.filename().wstring(), format)});
                tb.TextWrapping(TextWrapping::Wrap);
                sp.Children().Append(tb);
                dlg.Content(sp);
                dlg.PrimaryButtonText(lycan::Tr(L"main.open_contents"));
                dlg.SecondaryButtonText(lycan::Tr(L"main.add_to_archive_title"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Primary);
                auto r = co_await ShowDialogAsync(dlg);
                if (r == ContentDialogResult::Primary) OpenFromUi(file);
                else if (r == ContentDialogResult::Secondary) StartCompress({file});
                co_return;
            }
        }
        StartCompress(std::move(paths));
    }

    // ---- Picture preview

    // Toolbar "Preview > Images" / Alt+P: the selected image, otherwise the first one in the current list. Nothing to
    // preview is only reported after the user asked (no detection beforehand).
    void MainWindow::PreviewImages()
    {
        if (!current_ || !tree_ || !entries_) return;
        uint32_t pick = lycan::kNoNode, fallback = lycan::kNoNode;
        auto nodes = SelectedNodes();
        if (nodes.size() == 1)
        {
            auto const& n = tree_->Node(nodes[0]);
            if (!n.isDir && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableImageName(n.name)) pick = nodes[0];
        }
        if (pick == lycan::kNoNode)
            for (auto const& it : entries_)
            {
                const uint32_t id = it.as<LycanZip::EntryRow>().NodeId();
                auto const& n = tree_->Node(id);
                if (n.isDir || n.entryIndex == lycan::kNoEntry || lycan::IsMacMetadataPath(n.path)) continue;
                if (lycan_app::CanPreviewImage(n.name)) { pick = id; break; }
                if (fallback == lycan::kNoNode && lycan_app::IsPreviewableImageName(n.name)) fallback = id;  // known format, but no decoder
            }
        if (pick == lycan::kNoNode) pick = fallback;  // PreviewNode explains what is missing
        if (pick == lycan::kNoNode)
        {
            ShowMessage(lycan::Tr(L"main.no_images_preview"), lycan::Tr(L"main.there_no_images_current_folder"));
            return;
        }
        PreviewNode(pick);
    }

    // Toolbar "Preview > Audio" / Alt+Shift+P: the selected audio file, otherwise the first one in the current list.
    fire_and_forget MainWindow::PreviewAudio(uint32_t pick)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || !entries_) co_return;
        lycan_app::AudioRequest req;
        uint32_t picked = lycan::kNoNode;
        auto nodes = SelectedNodes();
        if (pick != lycan::kNoNode) nodes = {pick};
        if (nodes.size() == 1)
        {
            auto const& n = tree_->Node(nodes[0]);
            if (!n.isDir && n.entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableAudioName(n.name)) picked = nodes[0];
        }
        for (auto const& it : entries_)
        {
            const uint32_t id = it.as<LycanZip::EntryRow>().NodeId();
            auto const& n = tree_->Node(id);
            if (n.isDir || n.entryIndex == lycan::kNoEntry || !lycan_app::IsPreviewableAudioName(n.name) || lycan::IsMacMetadataPath(n.path)) continue;
            if (id == picked) req.start = req.items.size();
            req.items.push_back({n.entryIndex, n.name, n.path, n.size});
        }
        if (req.items.empty())
        {
            ShowMessage(lycan::Tr(L"main.no_audio_preview"), lycan::Tr(L"main.there_no_audio_files_current"));
            co_return;
        }
        if (!co_await EnsurePasswordAsync()) co_return;  // an encrypted archive needs its password first
        if (!current_ || !tree_) co_return;
        req.owner = this;
        req.archive = currentPath_;
        req.open = MakeOpenOptions();
        req.loopMode = static_cast<int>(settings_.audioLoop);
        req.onLoopMode = [weak = get_weak()](int m) { if (auto s = weak.get()) { s->settings_.audioLoop = static_cast<lycan::AudioLoop>(m); s->SaveSettings(); } };
        lycan_app::ShowAudioPreview(std::move(req));
    }

    void MainWindow::OnPreviewImagesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { PreviewImages(); }
    void MainWindow::OnPreviewAudioClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { PreviewAudio(); }

    fire_and_forget MainWindow::PreviewNode(uint32_t node)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || node >= tree_->NodeCount()) co_return;
        {
            auto const& n = tree_->Node(node);
            if (n.isDir || n.entryIndex == lycan::kNoEntry || !lycan_app::IsPreviewableImageName(n.name)) co_return;
            if (!lycan_app::IsImageDecoderAvailable(n.name))
            {
                ShowStatusNote(lycan::Tf(L"main.cannot_preview_use_open_with", n.name, lycan_app::MissingDecoderHint(n.name)));
                co_return;
            }
        }
        if (!co_await EnsurePasswordAsync()) co_return;  // an encrypted archive needs its password verified first
        if (!current_ || !tree_) co_return;

        lycan_app::PreviewRequest req;
        req.owner = this;
        req.archive = currentPath_;
        req.open = MakeOpenOptions();
        req.prefetchBytes = static_cast<uint64_t>(settings_.prefetchMB) << 20;
        req.enlargeSmall = settings_.previewEnlargeSmall;
            req.animations = AnimOn();
        req.setEnlargeSmall = [weak = get_weak()](bool on) {
            if (auto s = weak.get()) {
                s->settings_.previewEnlargeSmall = on;
                s->loadingSettings_ = true;  // keep the settings page in step without re-reading every control
                s->EnlargeToggle().IsOn(on);
                s->loadingSettings_ = false;
                s->SaveSettings();
            }
        };
        req.openExternal = [weak = get_weak(), archive = currentPath_](uint32_t n) {
            if (auto s = weak.get()) if (s->current_ && s->currentPath_ == archive) s->OpenEntryFileAsync(n);  // the archive has been replaced: do not handle it
        };
        req.thumbReads = current_->Caps().randomAccess && !current_->Caps().solid;  // a solid archive is not read actively for thumbnails
        // All pictures in the current list (same folder / search result), in the order of the screen; the left and right keys page
        if (entries_)
        {
            for (auto const& it : entries_)
            {
                const uint32_t id = it.as<LycanZip::EntryRow>().NodeId();
                auto const& n = tree_->Node(id);
                if (n.isDir || n.entryIndex == lycan::kNoEntry || !lycan_app::CanPreviewImage(n.name) || lycan::IsMacMetadataPath(n.path)) continue;
                if (id == node) req.start = req.items.size();
                req.items.push_back({n.entryIndex, n.name, n.path, n.size, id, n.packedSize, n.mtimeFileTime});
            }
        }
        if (req.items.empty())
        {
            auto const& n = tree_->Node(node);
            req.items.push_back({n.entryIndex, n.name, n.path, n.size, node, n.packedSize, n.mtimeFileTime});
            req.start = 0;
        }
        lycan_app::ShowImagePreview(std::move(req));
    }

    // ---- Dragging to Explorer

    // Only when the target (Explorer / desktop) asks for the files are they extracted to a temporary folder and the file list handed over (provided on demand, see DataPackage::SetDataProvider).
    // Small files have almost no delay; for large files the target waits. When cancelled / dropped where it is not accepted nothing is extracted.
    // This is the "extract to temporary first" way (a setting can choose it, and it is the fallback when streaming does not fit); when wrap is not empty everything goes under dir\wrap.
    static fire_and_forget ProvideDragItems(Windows::ApplicationModel::DataTransfer::DataProviderRequest request, fs::path archive,
                                            lycan::OpenOptions oo, lycan::ExtractOptions eo, std::vector<uint32_t> indices, fs::path dir,
                                            std::wstring wrap)
    {
        auto deferral = request.GetDeferral();
        co_await resume_background();
        try
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
            auto src = lycan::ArchiveSource::Open(archive, oo);
            fs::path target = dir;
            if (!wrap.empty())
            {
                const lycan::SafePath w = lycan::SanitizeEntryPath(wrap);
                target = dir / (w.rejected ? fs::path(L"extracted") : w.relative);
            }
            auto rep = lycan::ExtractTo(*src, target, eo, indices);
            std::vector<Windows::Storage::IStorageItem> items;
            for (auto const& e : fs::directory_iterator(dir, ec))
            {
                if (e.is_directory(ec)) items.push_back(Windows::Storage::StorageFolder::GetFolderFromPathAsync(e.path().wstring()).get());
                else items.push_back(Windows::Storage::StorageFile::GetFileFromPathAsync(e.path().wstring()).get());
            }
            if (!items.empty()) request.SetData(single_threaded_vector<Windows::Storage::IStorageItem>(std::move(items)).GetView());
        }
        catch (...) {}
        deferral.Complete();
    }

    // Starts dragging `nodes` out of the archive. strip: like ExtractOptions::stripPrefix (what stays above the dragged
    // items); wrap: put everything in a folder of this name (the archive's root). Cancel = no drag; Done = the whole drag
    // already ran (virtual files); Provider = `data` carries the delayed-files provider and the caller lets WinUI go on.
    MainWindow::DragOutResult MainWindow::BeginDragOut(std::vector<uint32_t> const& nodes, std::wstring const& strip, std::wstring const& wrap,
                                                       Windows::ApplicationModel::DataTransfer::DataPackage const& data)
    {
        auto indices = nodes.empty() ? std::vector<uint32_t>{} : tree_->EntryIndices(nodes);
        if (indices.empty()) return DragOutResult::Cancel;
        if (current_->Caps().encrypted && !passwordVerified_)
        {
            EnsurePasswordAsync();  // verify the password first; after it succeeds just drag again
            return DragOutResult::Cancel;
        }

        // Where the files go when they have to be unpacked: a fresh folder in the temporary area
        static std::atomic<uint32_t> seq{0};
        if (lycan_app::OpenTempRoot().empty()) return DragOutResult::Cancel;
        fs::path dir = lycan_app::OpenTempRoot() /
                       (L"drag-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++seq));

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        eo.overwrite = lycan::OverwriteMode::Overwrite;  // a new folder
        eo.stripPrefix = strip;
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };
        auto oo = MakeOpenOptions();
        const fs::path archive = currentPath_;

        dragOutActive_ = true;  // when dragged onto our own window it must not count as files to compress
        SetDragGlow(false);

        // Virtual files (DragOut.h): nothing is unpacked beforehand, so the cursor never shows "no drop" while a big file
        // is being prepared. Only Explorer-like targets take virtual files (the other programs ask for real files), so the
        // setting can turn it off; selections that cannot be virtual (solid archives with several files, very long names)
        // always use the temporary folder.
        {
            lycan_app::VirtualDragRequest vr;
            const auto caps = current_->Caps();
            if (settings_.dragOutVirtual && !_wgetenv(L"LYCAN_NO_VIRTUAL_DRAG") &&
                lycan_app::PlanVirtualDrag(current_->Entries(), indices, strip, wrap, settings_.skipMacMetadata, caps.randomAccess && !caps.solid, vr))
            {
                vr.archive = archive;
                vr.open = oo;
                ActiveList().ReleasePointerCaptures();
                // Right here, inside the pointer event that started the drag: the OLE drag loop runs from this call
                lycan_app::RunVirtualDrag(std::move(vr));
                dragOutActive_ = false;
                return DragOutResult::Done;
            }
        }

        data.RequestedOperation(Windows::ApplicationModel::DataTransfer::DataPackageOperation::Copy);
        data.SetDataProvider(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems(),
                             [archive, oo, eo, indices = std::move(indices), dir, wrap](Windows::ApplicationModel::DataTransfer::DataProviderRequest const& req) {
                                 ProvideDragItems(req, archive, oo, eo, indices, dir, wrap);
                             });
        return DragOutResult::Provider;
    }

    void MainWindow::OnEntryDragStarting(Windows::Foundation::IInspectable const&, DragItemsStartingEventArgs const& e)
    {
        if (!current_ || !tree_ || dragOutActive_ || bandPending_ || bandActive_) { e.Cancel(true); return; }
        std::vector<uint32_t> nodes;
        for (auto const& it : e.Items())
            if (auto row = it.try_as<LycanZip::EntryRow>()) nodes.push_back(row.NodeId());
        if (BeginDragOut(nodes, StripPrefixForView(), L"", e.Data()) != DragOutResult::Provider) e.Cancel(true);  // (virtual files: the drag already ran)
    }

    // The folder tree: a folder is dragged with its name on top; the root (the archive) is dragged like "extract to a
    // folder named after the archive"
    void MainWindow::OnTreeDragStarting(Windows::Foundation::IInspectable const&, TreeViewDragItemsStartingEventArgs const& e)
    {
        if (!current_ || !tree_ || dragOutActive_) { e.Cancel(true); return; }
        uint32_t node = lycan::kNoNode;
        for (auto const& it : e.Items())
        {
            if (auto tn = it.try_as<TreeViewNode>()) { node = NodeIdOf(tn); break; }
            if (auto row = it.try_as<LycanZip::EntryRow>()) { node = row.NodeId(); break; }
        }
        if (node == lycan::kNoNode || node >= tree_->NodeCount()) { e.Cancel(true); return; }
        std::wstring strip, wrap;
        if (node == tree_->Root()) wrap = lycan::ArchiveBaseName(currentPath_);
        else
        {
            const std::wstring parent = tree_->ParentPath(node);
            strip = parent.empty() ? std::wstring() : parent + L"/";
        }
        if (BeginDragOut({node}, strip, wrap, e.Data()) != DragOutResult::Provider) e.Cancel(true);
    }

    void MainWindow::OnEntryDragCompleted(ListViewBase const&, DragItemsCompletedEventArgs const&) { dragOutActive_ = false; }

    // At start remove temporary files older than one day (a file that is open is not deleted by mistake: only very old folders are cleared, and failures are skipped)
    void MainWindow::CleanupStaleTemp()
    {
        const fs::path root = lycan_app::OpenTempRoot();
        std::thread([root] {
            try
            {
                std::error_code ec;
                if (root.empty() || !fs::exists(root, ec)) return;
                auto now = fs::file_time_type::clock::now();
                for (auto const& d : fs::directory_iterator(root, ec))
                {
                    std::error_code e2;
                    auto t = fs::last_write_time(d.path(), e2);
                    if (!e2 && now - t > std::chrono::hours(24)) fs::remove_all(d.path(), e2);
                }
            }
            catch (...) {}
        }).detach();
    }

    // ---- Context menu

    void MainWindow::OnEntryContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& args)
    {
        if (!current_ || !tree_ || !entries_) return;
        args.Handled(true);

        // Right click on an item that is not selected: select only that one first (as in Explorer)
        if (auto row = RowFromSource(args.OriginalSource()))
        {
            bool selected = false;
            for (auto const& it : ActiveList().SelectedItems())
                if (it.as<LycanZip::EntryRow>().NodeId() == row.NodeId()) { selected = true; break; }
            if (!selected)
            {
                ActiveList().SelectedItems().Clear();
                ActiveList().SelectedItems().Append(row);
            }
        }

        auto nodes = SelectedNodes();
        MenuFlyout fly;
        auto add = [&](hstring const& text, std::function<void()> fn, hstring const& accel = {}, const wchar_t* glyph = nullptr, IconTone tone = IconTone::None) {
            MenuFlyoutItem item;
            item.Text(text);
            if (!accel.empty()) item.KeyboardAcceleratorTextOverride(accel);
            if (glyph) item.Icon(MenuIcon(glyph, tone, RootGrid().ActualTheme() != ElementTheme::Light));
            item.Click([fn = std::move(fn)](auto&&, auto&&) { fn(); });
            fly.Items().Append(item);
        };
        auto sep = [&] { fly.Items().Append(MenuFlyoutSeparator{}); };

        if (!nodes.empty()) AddNodeMenuItems(fly, nodes, StripPrefixForView());
        else
        {
            // View / sort: the same options as the "View" menu of the toolbar (shared ApplyViewChoice)
            auto radio = [&](MenuFlyoutSubItem const& sub, hstring const& text, bool checked, wchar_t cat, int n, hstring const& group) {
                RadioMenuFlyoutItem it;
                it.Text(text);
                it.GroupName(group);
                it.IsChecked(checked);
                it.Click([weak = get_weak(), cat, n](auto&&, auto&&) { if (auto s = weak.get()) s->ApplyViewChoice(cat, n); });
                sub.Items().Append(it);
            };
            MenuFlyoutSubItem viewSub;
            viewSub.Text(lycan::Tr(L"main.view"));
            radio(viewSub, lycan::Tr(L"main.details"), viewMode_ == lycan::ViewMode::Details, L'v', 0, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.list"), viewMode_ == lycan::ViewMode::List, L'v', 1, L"ctxview");
            const bool iconView = viewMode_ == lycan::ViewMode::Icons;
            radio(viewSub, lycan::Tr(L"main.view_small_icons"), iconView && settings_.thumbSize == 0, L'i', 0, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.view_medium_icons"), iconView && settings_.thumbSize == 1, L'i', 1, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.view_large_icons"), iconView && settings_.thumbSize == 2, L'i', 2, L"ctxview");
            radio(viewSub, lycan::Tr(L"main.view_huge_icons"), iconView && settings_.thumbSize == 3, L'i', 3, L"ctxview");
            viewSub.Items().Append(MenuFlyoutSeparator{});
            {
                ToggleMenuFlyoutItem th;
                th.Text(lycan::Tr(L"main.show_image_thumbnails_icon_view"));
                th.IsChecked(settings_.iconThumbnails);
                th.Click([weak = get_weak()](auto&&, auto&&) { if (auto s = weak.get()) s->ApplyViewChoice(L't', 1); });
                viewSub.Items().Append(th);
            }
            viewSub.Icon(MenuIcon(L"\uE890"));
            fly.Items().Append(viewSub);
            MenuFlyoutSubItem sortSub;
            sortSub.Text(lycan::Tr(L"main.sort_by"));
            sortSub.Icon(MenuIcon(L"\uE8CB"));
            radio(sortSub, lycan::Tr(L"main.name"), sortKey_ == lycan::SortKey::Name, L's', 0, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.size"), sortKey_ == lycan::SortKey::Size, L's', 1, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.packed_size"), sortKey_ == lycan::SortKey::PackedSize, L's', 2, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.modified"), sortKey_ == lycan::SortKey::Modified, L's', 3, L"ctxsort");
            radio(sortSub, lycan::Tr(L"main.type"), sortKey_ == lycan::SortKey::Type, L's', 4, L"ctxsort");
            sortSub.Items().Append(MenuFlyoutSeparator{});
            radio(sortSub, lycan::Tr(L"main.ascending"), sortAsc_, L'd', 1, L"ctxdir");
            radio(sortSub, lycan::Tr(L"main.descending"), !sortAsc_, L'd', 0, L"ctxdir");
            fly.Items().Append(sortSub);
            sep();
            add(lycan::Tr(L"main.add_files"), [this] { AddFromPickerAsync(false, CurrentFolderPath()); }, {}, L"\uE710", IconTone::Accent);
            add(lycan::Tr(L"main.add_folder"), [this] { AddFromPickerAsync(true, CurrentFolderPath()); }, {}, L"\uE8F4", IconTone::Accent);
            add(lycan::Tr(L"main.new_folder"), [this] { NewFolderAsync(CurrentFolderPath()); }, {}, L"\uE8F4", IconTone::Accent);
            if (current_->Caps().canModify && ClipboardHasFiles()) add(lycan::Tr(L"main.paste"), [this] { PasteFilesAsync(); }, L"Ctrl+V", L"\uE77F", IconTone::Accent);   // what was copied in Explorer goes into this folder
            sep();
            add(lycan::Tr(L"main.select_all"), [this] { ActiveList().SelectAll(); }, L"Ctrl+A", L"\uE8B3");
            add(flat_ ? lycan::Tr(L"main.switch_single_level_browsing") : lycan::Tr(L"main.list_all"), [this] {
                flat_ = !flat_;
                syncingUi_ = true;
                FlatToggle().IsChecked(flat_);
                syncingUi_ = false;
                RefreshList();
            });
            if (curNode_ != tree_->Root()) add(lycan::Tr(L"main.up_one_level"), [this] { OnUpClick(nullptr, nullptr); }, L"Backspace", L"\uE74A");
        }

        Windows::Foundation::Point pt{};
        if (args.TryGetPosition(sender, pt))
        {
            Primitives::FlyoutShowOptions opt;
            opt.Position(pt);
            fly.ShowAt(sender, opt);
        }
        else fly.ShowAt(sender.as<FrameworkElement>());
    }

    // The items for selected folders / files, shared by the list and the folder tree. strip: what stays above them when
    // they are extracted (the folder the list shows; the parent of the node in the tree).
    void MainWindow::AddNodeMenuItems(MenuFlyout const& fly, std::vector<uint32_t> const& nodes, std::wstring const& strip)
    {
        auto add = [&](hstring const& text, std::function<void()> fn, hstring const& accel = {}, const wchar_t* glyph = nullptr, IconTone tone = IconTone::None) {
            MenuFlyoutItem item;
            item.Text(text);
            if (!accel.empty()) item.KeyboardAcceleratorTextOverride(accel);
            if (glyph) item.Icon(MenuIcon(glyph, tone, RootGrid().ActualTheme() != ElementTheme::Light));
            item.Click([fn = std::move(fn)](auto&&, auto&&) { fn(); });
            fly.Items().Append(item);
        };
        auto sep = [&] { fly.Items().Append(MenuFlyoutSeparator{}); };
        const bool single = nodes.size() == 1;
        if (single)
        {
            uint32_t n = nodes[0];
            const bool isImage = !tree_->Node(n).isDir && tree_->Node(n).entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableImageName(tree_->Node(n).name);
            const bool enterPreviews = isImage && settings_.doubleClickPreview && lycan_app::IsImageDecoderAvailable(tree_->Node(n).name);
            if (tree_->Node(n).isDir) add(lycan::Tr(L"main.open_folder"), [this, n] { OpenNode(n); }, L"Enter", L"\uE838");
            else
            {
                if (isImage && lycan_app::IsImageDecoderAvailable(tree_->Node(n).name)) add(lycan::Tr(L"main.open_image_preview"), [this, n] { PreviewNode(n); }, enterPreviews ? L"Enter" : L"Alt+P", L"\uE91B");
                if (tree_->Node(n).entryIndex != lycan::kNoEntry && lycan_app::IsPreviewableAudioName(tree_->Node(n).name)) add(lycan::Tr(L"main.open_audio_preview"), [this] { PreviewAudio(); }, L"Alt+Shift+P", L"\uE8D6");
                add(lycan::Tr(L"main.open_with_associated_app"), [this, n] { OpenEntryFileAsync(n); }, enterPreviews ? L"" : L"Enter", L"\uE8A7");
                if (tree_->Node(n).entryIndex != lycan::kNoEntry) add(lycan::Tr(L"main.edit_put_back_into_archive"), [this, n] { EditEntryAsync(n); }, {}, L"\uE70F", IconTone::Accent);
            }
            sep();
        }
        add(lycan::Tr(L"main.extract_selected_items_here"), [this, nodes, strip] { ExtractNodes(nodes, strip, false); }, {}, L"\uE896");
        add(lycan::Tr(L"main.extract_selected_items"), [this, nodes, strip] { ExtractNodes(nodes, strip, true); }, {}, L"\uE896");
        sep();
        add(lycan::Tr(L"main.copy_path"), [this, nodes] { CopyPaths(nodes); }, {}, L"\uE8C8");
        add(lycan::Tr(L"main.calculate_hash_values"), [this, nodes] { HashNodes(nodes); }, {}, L"\uE7C3");
        if (single && !tree_->Node(nodes[0]).isDir && tree_->Node(nodes[0]).entryIndex != lycan::kNoEntry)
        {
            add(lycan::Tr(L"main.virustotal_lookup"), [this, n = nodes[0]] { VirusTotalNode(n); }, {}, L"\uE774", IconTone::Security);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(fly.Items().GetAt(fly.Items().Size() - 1), box_value(lycan::Tr(L"main.virustotal_lookup_tip")));
        }
        if (single && (flat_ || !query_.empty())) add(lycan::Tr(L"main.go_containing_folder"), [this, n = nodes[0]] { ShowNodeInFolder(n); });
        if (single) add(lycan::Tr(L"main.properties"), [this, nodes] { ShowProperties(nodes); }, {}, L"\uE946");
        sep();
        if (single && tree_->Node(nodes[0]).isDir)
            add(lycan::Tr(L"main.add_files_folder"), [this, p = tree_->Node(nodes[0]).path] { AddFromPickerAsync(false, p); }, {}, L"\uE710", IconTone::Accent);
        if (single && tree_->Node(nodes[0]).isDir && current_->Caps().canModify && ClipboardHasFiles())
            add(lycan::Tr(L"main.paste"), [this, p = tree_->Node(nodes[0]).path] { PasteFilesAsync(p, true); }, {}, L"\uE77F", IconTone::Accent);
        if (single) add(lycan::Tr(L"main.rename"), [this, n = nodes[0]] { RenameNodeAsync(n); }, L"F2", L"\uE8AC", IconTone::Accent);
        add(lycan::Tr(L"main.delete"), [this, nodes] { DeleteNodesAsync(nodes); }, L"Del", L"\uE74D", IconTone::Danger);
    }

    // The root of the folder tree is the archive itself
    void MainWindow::AddArchiveMenuItems(MenuFlyout const& fly)
    {
        auto add = [&](hstring const& text, std::function<void()> fn, const wchar_t* glyph = nullptr, IconTone tone = IconTone::None) {
            MenuFlyoutItem item;
            item.Text(text);
            if (glyph) item.Icon(MenuIcon(glyph, tone, RootGrid().ActualTheme() != ElementTheme::Light));
            item.Click([fn = std::move(fn)](auto&&, auto&&) { fn(); });
            fly.Items().Append(item);
        };
        auto sep = [&] { fly.Items().Append(MenuFlyoutSeparator{}); };
        add(lycan::Tr(L"main.menu_extract_here"), [this] { StartExtract(currentPath_.parent_path(), false); }, L"\uE896");
        add(lycan::Tr(L"main.extract"), [this] { OnExtractToClick(nullptr, nullptr); }, L"\uE896");
        add(lycan::Tr(L"main.test_integrity"), [this] { OnTestClick(nullptr, nullptr); }, L"\uE73E", IconTone::Verify);
        add(lycan::Tr(L"main.hash_values_archive"), [this] { OnHashArchiveClick(nullptr, nullptr); }, L"\uE7C3");
        add(lycan::Tr(L"sums.verify_menu"), [this] { VerifySumsFromPickerAsync(); }, L"\uE73E", IconTone::Verify);
        add(lycan::Tr(L"main.virustotal_lookup"), [this] { OnVirusTotalArchiveClick(nullptr, nullptr); }, L"\uE774", IconTone::Security);
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(fly.Items().GetAt(fly.Items().Size() - 1), box_value(lycan::Tr(L"main.virustotal_lookup_tip")));
        sep();
        add(lycan::Tr(L"main.add_files"), [this] { AddFromPickerAsync(false, std::wstring()); }, L"\uE710", IconTone::Accent);
        add(lycan::Tr(L"main.new_folder"), [this] { NewFolderAsync(std::wstring()); }, L"\uE8F4", IconTone::Accent);
        if (current_ && current_->Caps().canModify && ClipboardHasFiles()) add(lycan::Tr(L"main.paste"), [this] { PasteFilesAsync(std::wstring(), true); }, L"\uE77F", IconTone::Accent);
        sep();
        add(lycan::Tr(L"main.archive_information"), [this] { ShowArchiveInfoAsync(); }, L"\uE946");
        add(lycan::Tr(L"main.properties"), [this] { SHObjectProperties(Hwnd(), SHOP_FILEPATH, currentPath_.c_str(), nullptr); });  // Windows' own Properties window
        add(lycan::Tr(L"main.show_explorer"), [this] {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(currentPath_.c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        });
        add(lycan::Tr(L"main.copy_file_path"), [this] {
            Windows::ApplicationModel::DataTransfer::DataPackage dp;
            dp.SetText(hstring{currentPath_.wstring()});
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        }, L"\uE8C8");
        if (!IsVolumePath(currentPath_))  // a split archive is several files: deleting one of them would leave the rest behind
        {
            sep();
            add(lycan::Tr(L"main.delete_archive"), [this] { DeleteCurrentArchiveAsync(); }, L"\uE74D", IconTone::Danger);
        }
    }

    void MainWindow::OnTreeContextRequested(UIElement const& sender, Input::ContextRequestedEventArgs const& args)
    {
        if (!current_ || !tree_) return;
        uint32_t node = lycan::kNoNode;
        for (auto d = args.OriginalSource().try_as<DependencyObject>(); d; d = Microsoft::UI::Xaml::Media::VisualTreeHelper::GetParent(d))
        {
            if (auto tvi = d.try_as<TreeViewItem>())
            {
                if (auto tn = FolderTree().NodeFromContainer(tvi)) node = NodeIdOf(tn);
                break;
            }
        }
        if (node == lycan::kNoNode || node >= tree_->NodeCount()) return;
        args.Handled(true);
        MenuFlyout fly;
        if (node == tree_->Root()) AddArchiveMenuItems(fly);
        else
        {
            const std::wstring parent = tree_->ParentPath(node);
            AddNodeMenuItems(fly, {node}, parent.empty() ? std::wstring() : parent + L"/");
        }
        Windows::Foundation::Point pt{};
        if (args.TryGetPosition(sender, pt))
        {
            Primitives::FlyoutShowOptions opt;
            opt.Position(pt);
            fly.ShowAt(sender, opt);
        }
        else fly.ShowAt(sender.as<FrameworkElement>());
    }

    // "Delete the archive": to the Recycle Bin, then back to the home page. The question can be switched off (settings,
    // or the check box in the question itself).
    fire_and_forget MainWindow::DeleteCurrentArchiveAsync()
    {
        auto strong = get_strong();
        if (!current_) co_return;
        const fs::path file = currentPath_;
        const bool recycle = CanRecycle(file);
        if (settings_.confirmDeleteArchive)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.delete_archive_title"), L"\uE74D");
            StackPanel sp;
            sp.Spacing(10);
            sp.MaxWidth(440);
            TextBlock t1;
            t1.Text(hstring{recycle ? lycan::Tr(L"main.move_archive_recycle_bin_you_can")
                                    : lycan::Tr(L"main.drive_archive_has_no_recycle_bin")});
            t1.TextWrapping(TextWrapping::Wrap);
            if (!recycle) t1.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sp.Children().Append(t1);
            TextBlock t2;
            t2.Text(hstring{file.wstring()});
            t2.Opacity(0.8);
            t2.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t2);
            CheckBox again;
            again.Content(box_value(hstring{lycan::Tr(L"main.delete_without_asking_from_now_on")}));
            sp.Children().Append(again);
            dlg.Content(sp);
            dlg.PrimaryButtonText(recycle ? lycan::Tr(L"main.move_recycle_bin") : lycan::Tr(L"main.delete_permanently"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Close);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
            if (again.IsChecked().Value())
            {
                settings_.confirmDeleteArchive = false;
                loadingSettings_ = true;
                ConfirmDeleteToggle().IsOn(false);
                loadingSettings_ = false;
                SaveSettings();
            }
        }
        if (!current_ || currentPath_ != file) co_return;  // something else was opened while the question was open
        int deleted = 0;
        UnlockArchiveFile();
        const int failed = ShellDelete(Hwnd(), {file}, recycle, deleted);
        if (failed != 0) LockArchiveFile(currentPath_);
        if (failed == 0) ShowHome();
        else ShowMessage(lycan::Tr(L"main.archive_could_not_be_deleted"), lycan::Tr(L"main.file_may_be_use_by_another"));
    }

    // ---------------------------------------------------------------- Changing the archive (the design notes "Changing an archive")
    //
    // add / delete / rename / new folder run as one task of the queue (TaskKind::Update): the archive is rewritten in a
    // temporary file next to it and swapped in (update.h). While it runs the window keeps showing the old content; when it is
    // done the archive is opened again and the view comes back to where it was.

    namespace
    {
        constexpr uint64_t kUpdateWarnBytes = 64ull << 20;  // a solid block that is unpacked and packed again beyond this: ask first

        // A name for one item (no folders in it): what Windows can extract, no ".", no "..".
        bool ValidEntryName(std::wstring const& n)
        {
            if (n.empty() || n.size() > 255 || n == L"." || n == L"..") return false;
            for (wchar_t c : n)
                if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) return false;
            return true;
        }

        FileStamp StampOfFile(fs::path const& p)
        {
            FileStamp s;
            std::error_code ec;
            const auto t = fs::last_write_time(p, ec);
            if (ec) return s;
            s.time = t.time_since_epoch().count();
            s.size = fs::file_size(p, ec);
            s.ok = !ec;
            return s;
        }

        std::wstring TrimName(std::wstring s)
        {
            const size_t b = s.find_first_not_of(L" \t"), e = s.find_last_not_of(L" \t");
            return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
        }

        bool PathIsUnder(std::wstring const& path, std::wstring const& folder)
        {
            return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == L'/';
        }
    }

    std::wstring MainWindow::CurrentFolderPath() const
    {
        return tree_ && curNode_ < tree_->NodeCount() ? tree_->Node(curNode_).path : std::wstring();
    }

    // Can the shown archive be changed right now? With explain: says why not.
    bool MainWindow::CanModifyNow(bool explain)
    {
        if (!current_ || !tree_) return false;
        if (updateTask_ != 0)
        {
            if (explain) ShowStatusNote(lycan::Tr(L"main.archive_being_changed_please_wait"));
            return false;
        }
        if (!current_->Caps().canModify)
        {
            if (explain)
                ShowMessage(lycan::Tr(L"main.archive_cannot_be_changed_directly"),
                            hstring{lycan::Tf(L"main.cannot_be_changed_place_only_zip", current_->FormatName())});
            return false;
        }
        const DWORD attr = GetFileAttributesW(currentPath_.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        {
            if (explain) ShowMessage(lycan::Tr(L"main.archive_read_only"), lycan::Tr(L"main.file_set_read_only_cannot_be"));
            return false;
        }
        return true;
    }

    lycan::UpdateOptions MainWindow::MakeUpdateOptions(lycan::UpdateMode mode)
    {
        lycan::UpdateOptions uo;
        uo.mode = mode;
        uo.compress.password = password_;
        uo.compress.tempDir = lycan_app::ScratchOverride();
        uo.compress.skipThumbsDb = settings_.skipThumbsDb;
        uo.keepBackup = settings_.keepBackupOnModify;
        uo.nameCodepage = currentCp_;
        return uo;
    }

    // The window sits still while the archive is replaced; afterwards it opens the new file, in the same folder and view
    fire_and_forget MainWindow::RunUpdateAsync(std::vector<lycan::UpdateOp> ops, TaskMeta meta, lycan::UpdateMode mode)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true)) co_return;
        if (current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;
        if (!CanModifyNow(false)) co_return;  // the archive changed while the password was asked
        const fs::path archive = currentPath_;
        lycan::diag::Info(L"update of {}: {} operation(s)", lycan::diag::Name(archive), ops.size());
        const lycan::UpdateOptions uo = MakeUpdateOptions(mode);

        // what it costs (a solid 7z block that loses an item is packed again); a bad operation shows up here before anything runs
        uint64_t recompress = 0;
        std::wstring error;
        SetBusy(true);
        co_await resume_background();
        try { recompress = lycan::EstimateUpdate(archive, ops, uo).recompressBytes; }
        catch (lycan::Error const& e) { error = e.message().Text(); }
        catch (std::exception const& e) { error = FromUtf8(e.what()); }
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!error.empty())
        {
            ShowMessage(lycan::Tr(L"main.archive_could_not_be_changed"), hstring{error});
            co_return;
        }
        if (!current_ || currentPath_ != archive || updateTask_ != 0) co_return;
        if (recompress >= kUpdateWarnBytes)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.will_take_while"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.about_solid_archive_has_be", FormatSize(recompress))});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"main.resume"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Primary);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
            if (!current_ || currentPath_ != archive || updateTask_ != 0) co_return;
        }

        // the previews show items by index: they would go on showing the old archive
        lycan_app::CloseImagePreview(this);
        lycan_app::CloseAudioPreview(this);
        meta.archive = archive;
        meta.isUpdate = true;
        // the view comes back as it was
        meta.keep.valid = true;
        meta.keep.flat = flat_;
        meta.keep.mode = viewMode_;
        meta.keep.sort = sortKey_;
        meta.keep.asc = sortAsc_;
        meta.keep.tree = treeWanted_;
        meta.keep.query = query_;
        {
            std::lock_guard lk(metaM_);
            UnlockArchiveFile();  // the archive is replaced by a new file: nobody may hold the old one (taken again when it is shown again)
            const uint64_t id = queue_.EnqueueUpdate(archive, std::move(ops), uo);
            updateTask_ = id;
            taskMeta_[id] = std::move(meta);
        }
    }

    void MainWindow::FinishUpdateTask(lycan::TaskInfo const& info, TaskMeta& meta)
    {
        if (updateTask_ == info.id) updateTask_ = 0;
        if (current_ && archiveLock_ == INVALID_HANDLE_VALUE) LockArchiveFile(currentPath_);  // nothing was swapped in (failed, cancelled, no change)
        const bool changed = info.state == lycan::TaskState::Completed || (info.state == lycan::TaskState::Failed && info.update.changed);
        if (info.state == lycan::TaskState::Cancelled) return;
        if (!changed)
        {
            if (info.update.errors.empty() && info.error.empty())
            {
                if (info.update.unchanged || info.update.notAdded)
                    ShowMessage(lycan::Tr(L"main.nothing_update"), lycan::Tr(L"main.files_archive_up_date_none_files"));
                return;
            }
            ShowMessage(lycan::Tr(L"main.archive_could_not_be_changed"), hstring{info.error.empty() ? info.update.errors[0].Text() : info.error.Text()});
            return;
        }
        if (!info.update.errors.empty())
            ShowMessage(lycan::Tr(L"main.some_files_were_not_added"), hstring{lycan::Tf(L"main.archive_was_changed_but_file_s", info.update.errors.size(), info.update.errors[0].Text())});
        if (!current_ || currentPath_ != meta.archive) return;  // this window moved on to another archive

        // the summary shows in the status area once the new archive is on screen
        std::wstring note;
        auto part = [&](uint32_t n, const wchar_t* text) { if (n) note += (note.empty() ? std::wstring{} : std::wstring{lycan::Tr(L"fmt.list_sep")}) + lycan::Tf(text, n); };
        part(info.update.added + info.update.replaced, L"main.status_added");
        part(info.update.deleted, L"main.deleted");
        part(info.update.renamed, L"main.renamed");
        part(info.update.dirsCreated, L"main.new_folder_s");
        part(info.update.unchanged, L"main.unchanged");
        openNote_ = note.empty() ? std::wstring() : lycan::Tf(L"main.archive_changed", note);
        if (info.update.storedFiles) openNote_ += lycan::Tf(L"main.already_compressed_file_s_stored", info.update.storedFiles);
        for (auto& e : edits_)
            if (e.archive == meta.archive) e.archiveStamp = StampOfFile(e.archive);  // our own change is not "somebody else's"
        restoreFor_ = meta.archive;
        restoreFolder_ = meta.restoreFolder;
        restoreSelect_ = meta.selectPaths;
        keptView_ = meta.keep;
        OpenArchiveAsync(currentPath_, currentCp_, password_);
    }

    // Adding to an archive that has items of the same name: what is done with the files of the same name
    Windows::Foundation::IAsyncOperation<int32_t> MainWindow::ChooseAddModeAsync(int clashes)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.items_with_same_name"), L"");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(460);
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.archive_already_has_items_with", clashes)});
        tb.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(tb);
        RadioButtons rb;
        struct Opt { const wchar_t* title; const wchar_t* text; };
        const Opt opts[] = {{L"main.replace_add", L"main.files_same_name_always_replaced"},
                            {L"main.update_add", L"main.file_replaced_only_if_one_on"},
                            {L"main.update_existing_only", L"main.only_files_archive_newer_on_disk"}};
        for (auto const& o : opts)
        {
            StackPanel line;
            TextBlock t;
            t.Text(lycan::Tr(o.title));
            line.Children().Append(t);
            line.Children().Append(Caption(lycan::Tr(o.text), 0.7));
            RadioButton r;
            r.Content(line);
            rb.Items().Append(r);
        }
        rb.SelectedIndex(settings_.addMode == lycan::AddMode::Replace ? 0 : settings_.addMode == lycan::AddMode::UpdateAdd ? 1 : settings_.addMode == lycan::AddMode::Freshen ? 2 : 0);
        sp.Children().Append(rb);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.add"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return -1;
        const int idx = std::clamp(rb.SelectedIndex(), 0, 2);
        settings_.addMode = static_cast<lycan::AddMode>(idx);
        SaveSettings();
        co_return idx;
    }

    // Making an archive whose file is there already
    Windows::Foundation::IAsyncOperation<int32_t> MainWindow::ChooseExistingOutputAsync(fs::path output, bool canUpdate)
    {
        auto strong = get_strong();
        PlaySfx(L"conflict");
        auto dlg = MakeDialog(lycan::Tr(L"main.file_already_exists"), L"\uE8C8");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(480);
        {
            StackPanel where;
            where.Spacing(2);
            TextBlock n;
            n.Text(hstring{output.filename().wstring()});
            n.FontSize(16);
            n.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            n.TextTrimming(TextTrimming::CharacterEllipsis);
            where.Children().Append(n);
            where.Children().Append(Caption(output.parent_path().wstring(), 0.65));
            sp.Children().Append(where);
        }
        // the choices, in this order (the index of the radio button -> what it means): 0 = add to it, 1 = a new name, 2 = replace it
        std::vector<int> kinds;
        RadioButtons rb;
        auto addOption = [&](int kind, const wchar_t* title, std::wstring const& text) {
            StackPanel line;
            TextBlock t;
            t.Text(title);
            line.Children().Append(t);
            line.Children().Append(Caption(text, 0.7));
            RadioButton r;
            r.Content(line);
            rb.Items().Append(r);
            kinds.push_back(kind);
        };
        if (canUpdate) addOption(3, lycan::Tr(L"main.add_to_this_archive"), lycan::Tr(L"main.add_these_files_what_not"));
        addOption(1, lycan::Tr(L"main.save_as_new_file"), lycan::Tf(L"main.saved_as", UniquePath(output).filename().wstring()));
        addOption(2, lycan::Tr(L"main.replace"), lycan::Tr(L"main.old_archive_replaced_by_one"));
        int sel = 0;
        const int wanted = settings_.existingOutput == lycan::ExistingOutput::Update ? 3 : settings_.existingOutput == lycan::ExistingOutput::Overwrite ? 2 : 1;
        for (size_t i = 0; i < kinds.size(); ++i) if (kinds[i] == wanted) sel = static_cast<int>(i);
        rb.SelectedIndex(sel);
        sp.Children().Append(rb);
        ComboBox modes;
        for (auto const* t : {L"main.replace_add_files_same_name_always", L"main.update_add_replace_only_if_newer", L"main.update_existing_only_only_files",
                              L"main.synchronize_make_archive_match"})
            modes.Items().Append(box_value(hstring{lycan::Tr(t)}));
        modes.SelectedIndex(static_cast<int>(settings_.addMode));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(modes, lycan::Tr(L"main.how_treat_files_same_name"));
        modes.HorizontalAlignment(HorizontalAlignment::Stretch);
        modes.Margin(ThicknessHelper::FromLengths(28, 0, 0, 0));
        auto showModes = [&] { modes.Visibility(canUpdate && rb.SelectedIndex() >= 0 && kinds[static_cast<size_t>(rb.SelectedIndex())] == 3 ? Visibility::Visible : Visibility::Collapsed); };
        showModes();
        rb.SelectionChanged([&](auto&&, auto&&) { showModes(); });
        sp.Children().Append(modes);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return 0;
        const int kind = kinds[static_cast<size_t>(std::clamp(rb.SelectedIndex(), 0, static_cast<int>(kinds.size()) - 1))];
        settings_.existingOutput = kind == 3 ? lycan::ExistingOutput::Update : kind == 2 ? lycan::ExistingOutput::Overwrite : lycan::ExistingOutput::Rename;
        if (kind == 3) settings_.addMode = static_cast<lycan::AddMode>(std::clamp(modes.SelectedIndex(), 0, 3));
        SaveSettings();
        co_return kind == 3 ? 3 + std::clamp(modes.SelectedIndex(), 0, 3) : kind;
    }

    // Synchronize deletes things: what goes, before it is done
    Windows::Foundation::IAsyncOperation<bool> MainWindow::ConfirmSyncAsync(fs::path archive, std::vector<lycan::UpdateOp> ops, lycan::UpdateOptions uo)
    {
        auto strong = get_strong();
        lycan::UpdateReport rep;
        std::wstring error;
        SetBusy(true);
        co_await resume_background();
        try { rep = lycan::PreviewUpdate(archive, ops, uo); }
        catch (lycan::Error const& e) { error = e.message().Text(); }
        catch (std::exception const& e) { error = FromUtf8(e.what()); }
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!error.empty())
        {
            ShowMessage(lycan::Tr(L"main.cannot_synchronize"), hstring{error});
            co_return false;
        }
        if (rep.deleted == 0) co_return true;   // nothing goes out: no question
        auto dlg = MakeDialog(lycan::Tr(L"main.synchronizing_deletes_items_from"), L"\uE7BA");
        StackPanel sp;
        sp.Spacing(10);
        sp.MinWidth(460);
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.items_archive_no_longer_exist", rep.deleted, rep.added, rep.replaced, rep.unchanged)});
        tb.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(tb);
        std::wstring names;
        for (size_t i = 0; i < rep.deletedPaths.size() && i < 30; ++i) names += rep.deletedPaths[i] + L"\n";
        if (rep.deletedPaths.size() > 30) names += lycan::Tf(L"main.more", rep.deletedPaths.size() - 30);
        TextBox list;
        list.Text(hstring{names});
        list.IsReadOnly(true);
        list.AcceptsReturn(true);
        list.TextWrapping(TextWrapping::NoWrap);
        list.MaxHeight(220);
        ScrollViewer::SetVerticalScrollBarVisibility(list, ScrollBarVisibility::Auto);
        sp.Children().Append(list);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.synchronize"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        co_return co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary;
    }

    // Files and folders (from Explorer or a picker) into the folder `into` of the archive ("" = top level)
    fire_and_forget MainWindow::AddPathsToArchive(std::vector<fs::path> paths, std::wstring into)
    {
        auto strong = get_strong();
        if (paths.empty() || !CanModifyNow(true)) co_return;
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        int clashes = 0;
        for (auto const& p : paths)
        {
            std::wstring name = p.has_filename() ? p.filename().wstring() : p.parent_path().filename().wstring();
            if (name.empty()) continue;
            const std::wstring target = into.empty() ? name : into + L"/" + name;
            if (tree_->Find(target) != lycan::kNoNode) ++clashes;
            meta.selectPaths.push_back(target);
        }
        lycan::UpdateMode mode = lycan::UpdateMode::AddReplace;
        if (clashes)
        {
            const int32_t chosen = co_await ChooseAddModeAsync(clashes);
            if (chosen < 0) co_return;
            mode = static_cast<lycan::UpdateMode>(chosen);
            if (!CanModifyNow(false)) co_return;
        }
        RunUpdateAsync({lycan::UpdateOp::Add(into, std::move(paths))}, std::move(meta), mode);
    }

    fire_and_forget MainWindow::AddFromPickerAsync(bool folder, std::wstring into)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true)) co_return;
        std::vector<fs::path> add;
        if (const wchar_t* t = _wgetenv(L"LYCAN_TEST_ADD_PATHS"))  // tests: no picker, '|' separates the paths
        {
            std::wstring all = t;
            size_t pos = 0;
            while (pos <= all.size())
            {
                size_t end = all.find(L'|', pos);
                if (end == std::wstring::npos) end = all.size();
                if (end > pos) add.emplace_back(all.substr(pos, end - pos));
                pos = end + 1;
            }
        }
        else if (folder)
        {
            const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeSourceFolder, {}, lycan::Tr(L"main.choose_folder_add")))};
            if (!dir.empty()) add.push_back(dir);
        }
        else
        {
            auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeSourceFiles, true, false);
            for (auto const& f : files) add.emplace_back(std::wstring(f));
        }
        if (add.empty()) co_return;
        AddPathsToArchive(std::move(add), std::move(into));
    }

    void MainWindow::OnEditorPathLostFocus(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        std::wstring p = TrimName(std::wstring{EditorPathBox().Text()});
        if (p.size() > 260) p.clear();
        if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);  // pasted with its quotes
        if (p == settings_.editorPath) return;
        settings_.editorPath = p;
        SaveSettings();
    }

    void MainWindow::OnAddFilesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { AddFromPickerAsync(false, CurrentFolderPath()); }
    void MainWindow::OnAddFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { AddFromPickerAsync(true, CurrentFolderPath()); }
    void MainWindow::OnNewFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { NewFolderAsync(CurrentFolderPath()); }

    // One name for a folder or an item: the question stays open until the name is acceptable
    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskEntryNameAsync(hstring title, hstring initial)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(title, L"");
        TextBox box;
        box.Text(initial);
        box.MaxLength(255);
        Automation::AutomationProperties::SetName(box, lycan::Tr(L"main.name"));
        TextBlock bad;
        bad.Text(lycan::Tr(L"main.name_cannot_be_empty_contain"));
        bad.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})});
        bad.TextWrapping(TextWrapping::Wrap);
        bad.Visibility(Visibility::Collapsed);
        StackPanel sp;
        sp.Spacing(8);
        sp.MinWidth(360);
        sp.Children().Append(box);
        sp.Children().Append(bad);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        dlg.PrimaryButtonClick([box, bad](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
            if (!ValidEntryName(TrimName(std::wstring{box.Text()}))) { bad.Visibility(Visibility::Visible); a.Cancel(true); }
        });
        auto r = co_await ShowDialogAsync(dlg);
        co_return r == ContentDialogResult::Primary ? hstring{TrimName(std::wstring{box.Text()})} : hstring{};
    }

    fire_and_forget MainWindow::NewFolderAsync(std::wstring into)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true)) co_return;
        std::wstring name{co_await AskEntryNameAsync(lycan::Tr(L"main.new_folder_name"), lycan::Tr(L"main.new_folder_name"))};
        if (name.empty() || !CanModifyNow(false)) co_return;
        const std::wstring path = into.empty() ? name : into + L"/" + name;
        if (tree_->Find(path) != lycan::kNoNode)
        {
            ShowMessage(lycan::Tr(L"main.name_already_exists"), hstring{lycan::Tf(L"main.there_already_item_called_here", name)});
            co_return;
        }
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        meta.selectPaths.push_back(path);
        RunUpdateAsync({lycan::UpdateOp::MakeDir(path)}, std::move(meta));
    }

    fire_and_forget MainWindow::RenameNodeAsync(uint32_t node)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true) || node >= tree_->NodeCount() || node == tree_->Root()) co_return;
        const std::wstring oldPath = tree_->Node(node).path, oldName = tree_->Node(node).name, parent = tree_->ParentPath(node);
        std::wstring name{co_await AskEntryNameAsync(lycan::Tr(L"main.rename"), hstring{oldName})};
        if (name.empty() || name == oldName || !CanModifyNow(false)) co_return;
        const std::wstring path = parent.empty() ? name : parent + L"/" + name;
        if (tree_->Find(path) != lycan::kNoNode)
        {
            ShowMessage(lycan::Tr(L"main.name_already_exists"), hstring{lycan::Tf(L"main.there_already_item_called_here", name)});
            co_return;
        }
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        if (meta.restoreFolder == oldPath || PathIsUnder(meta.restoreFolder, oldPath)) meta.restoreFolder = path + meta.restoreFolder.substr(oldPath.size());  // the folder shown is the one renamed
        meta.selectPaths.push_back(path);
        RunUpdateAsync({lycan::UpdateOp::Rename(oldPath, path)}, std::move(meta));
    }

    fire_and_forget MainWindow::DeleteNodesAsync(std::vector<uint32_t> nodes)
    {
        auto strong = get_strong();
        if (nodes.empty() || !CanModifyNow(true)) co_return;
        // a folder takes what is below it: leave out what is selected inside a selected folder
        std::vector<std::wstring> paths;
        for (uint32_t n : nodes)
        {
            if (n == tree_->Root() || n >= tree_->NodeCount()) continue;
            bool covered = false;
            for (uint32_t m : nodes)
                if (m != n && tree_->Node(m).isDir && PathIsUnder(tree_->Node(n).path, tree_->Node(m).path)) covered = true;
            if (!covered) paths.push_back(tree_->Node(n).path);
        }
        if (paths.empty()) co_return;
        const std::wstring firstName = tree_->Node(nodes[0]).name;
        if (settings_.confirmDeleteEntries)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.delete_from_archive"), L"");
            StackPanel sp;
            sp.Spacing(10);
            sp.MaxWidth(440);
            TextBlock t1;
            t1.Text(hstring{paths.size() == 1 ? lycan::Tf(L"main.delete_one_from_archive", firstName) : lycan::Tf(L"main.delete_these_items_from_archive", paths.size())});
            t1.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t1);
            TextBlock t2;
            t2.Text(settings_.keepBackupOnModify ? lycan::Tr(L"main.items_taken_out_archive_itself_not")
                                                 : lycan::Tr(L"main.items_taken_out_archive_itself_not_moved_recycle_bin_cannot_be"));
            t2.Opacity(0.8);
            t2.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t2);
            CheckBox again;
            again.Content(box_value(hstring{lycan::Tr(L"main.delete_without_asking_from_now_on")}));
            sp.Children().Append(again);
            dlg.Content(sp);
            dlg.PrimaryButtonText(lycan::Tr(L"main.delete"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Close);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
            if (again.IsChecked().Value())
            {
                settings_.confirmDeleteEntries = false;
                loadingSettings_ = true;
                ConfirmDeleteEntriesToggle().IsOn(false);
                loadingSettings_ = false;
                SaveSettings();
            }
            if (!CanModifyNow(false) || !tree_) co_return;
        }
        std::vector<lycan::UpdateOp> ops;
        for (auto const& p : paths) ops.push_back(lycan::UpdateOp::Delete(p));
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        RunUpdateAsync(std::move(ops), std::move(meta));
    }

    // ---------------------------------------------------------------- Editing a file of the archive
    //
    // "Edit": the file is extracted to a temporary folder, opened in a program (Notepad for text, the program that is
    // associated with the type for everything else, or the editor chosen in the settings) and watched. When it has been
    // saved and is quiet again, the user is asked whether to put it back into the archive (a Replace operation of
    // RunUpdateAsync). The program may be one that is shared by many windows (Notepad, VS Code), so its process is not what
    // is watched but the file: its last write time and size.

    fire_and_forget MainWindow::EditEntryAsync(uint32_t node)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true) || node >= tree_->NodeCount()) co_return;
        auto const& n = tree_->Node(node);
        if (n.isDir || n.entryIndex == lycan::kNoEntry) co_return;
        const uint32_t entryIndex = n.entryIndex;
        const std::wstring name = n.name, entryPath = n.path;
        auto safe = lycan::SanitizeEntryPath(name);
        if (safe.rejected)
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), hstring{lycan::Tf(L"main.file_s_name_unsafe_was_rejected", safe.reason.Text())});
            co_return;
        }
        // one session per item: editing it again goes back to the file that is being edited
        for (auto const& e : edits_)
            if (e.archive == currentPath_ && e.entryPath == entryPath && fs::exists(e.file))
            {
                ShellExecuteW(nullptr, L"open", e.file.c_str(), nullptr, e.file.parent_path().c_str(), SW_SHOWNORMAL);
                co_return;
            }
        if (!co_await EnsurePasswordAsync() || !CanModifyNow(false)) co_return;

        static std::atomic<uint32_t> seq{0};
        if (lycan_app::OpenTempRoot().empty())
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), lycan::Tr(L"main.temporary_folder_not_found"));
            co_return;
        }
        const fs::path dir = lycan_app::OpenTempRoot() /
                             (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++seq));
        lycan::ExtractOptions eo;
        eo.overwrite = lycan::OverwriteMode::Overwrite;
        eo.stripPrefix = tree_->ParentPath(node).empty() ? std::wstring() : tree_->ParentPath(node) + L"/";
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };

        TaskMeta meta;
        meta.archive = currentPath_;
        meta.openFile = dir / safe.relative;
        meta.editEntry = entryPath;
        std::lock_guard lk(metaM_);
        const uint64_t id = queue_.EnqueueExtract(currentPath_, dir, eo, MakeOpenOptions(), {entryIndex});
        taskMeta_[id] = std::move(meta);
    }

    // The extraction is done: start the program and the watch
    void MainWindow::StartEditSession(TaskMeta const& meta)
    {
        if (!fs::exists(meta.openFile)) return;
        EditSession s;
        static uint64_t nextId = 1;
        s.id = nextId++;
        s.archive = meta.archive;
        s.entryPath = meta.editEntry;
        s.file = meta.openFile;
        s.archiveStamp = StampOfFile(meta.archive);
        s.baseline = s.lastSeen = StampOfFile(meta.openFile);
        edits_.push_back(s);

        if (!_wgetenv(L"LYCAN_TEST_NO_OPEN"))
        {
            std::wstring program = TrimName(settings_.editorPath);
            std::error_code ec;
            if (!program.empty() && !fs::exists(program, ec) && program.find_first_of(L"\\/") != std::wstring::npos) program.clear();  // not there any more
            const std::wstring workDir = s.file.parent_path().wstring();
            if (program.empty() && lycan::IsTextEditName(s.file.filename().wstring())) program = L"notepad.exe";
            bool started = false;
            if (!program.empty())
            {
                const std::wstring arg = L"\"" + s.file.wstring() + L"\"";
                started = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", program.c_str(), arg.c_str(), workDir.c_str(), SW_SHOWNORMAL)) > 32;
            }
            if (!started)
            {
                started = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, s.file.c_str(), nullptr, workDir.c_str(), SW_SHOWNORMAL)) > 32;
                if (!started) started = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"openas", s.file.c_str(), nullptr, workDir.c_str(), SW_SHOWNORMAL)) > 32;
            }
        }
        ShowStatusNote(lycan::Tf(L"main.editing_after_you_save_you_asked", s.file.filename().wstring()));
        if (!editTimer_)
        {
            editTimer_ = dispatcher_.CreateTimer();
            editTimer_.Interval(std::chrono::milliseconds(1000));
            editTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->PollEdits(); });
        }
        editTimer_.Start();
    }

    // Once a second: a file that was saved and has not changed for a second is ready to go back
    void MainWindow::PollEdits()
    {
        for (size_t i = 0; i < edits_.size();)
        {
            EditSession& e = edits_[i];
            if (e.prompting) { ++i; continue; }
            const FileStamp now = StampOfFile(e.file);
            if (!now.ok)
            {
                // an editor that saves by writing a new file and renaming it leaves a gap; a file that stays away is gone
                if (++e.missing > 600) { edits_.erase(edits_.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
                ++i;
                continue;
            }
            e.missing = 0;
            if (now == e.baseline) e.stable = 0;
            else if (now == e.lastSeen) { if (++e.stable >= 1) PromptWriteBack(e.id); }
            else e.stable = 0;
            e.lastSeen = now;
            ++i;
        }
        if (edits_.empty() && editTimer_) editTimer_.Stop();
    }

    bool MainWindow::HasUnsavedEdits() const
    {
        for (auto const& e : edits_)
        {
            const FileStamp now = StampOfFile(e.file);
            if (now.ok && !(now == e.baseline)) return true;
        }
        return false;
    }

    fire_and_forget MainWindow::PromptWriteBack(uint64_t id)
    {
        auto strong = get_strong();
        auto find = [this, id]() -> EditSession* {
            for (auto& e : edits_) if (e.id == id) return &e;
            return nullptr;
        };
        EditSession* s = find();
        if (!s || s->prompting) co_return;
        // not now: another question is open, a change is running, or the window shows something else (it asks later)
        if (!current_ || currentPath_ != s->archive || updateTask_ != 0 || dialogOpen_ || view_ != View::Archive || !tree_) co_return;
        s->prompting = true;
        const std::wstring name = s->file.filename().wstring();
        const std::wstring entryPath = s->entryPath;
        const fs::path file = s->file;

        // somebody else changed the archive while the file was being edited: putting it back could undo their change
        if (!(StampOfFile(s->archive) == s->archiveStamp))
        {
            s->prompting = false;
            s->baseline = StampOfFile(file);
            ShowMessage(lycan::Tr(L"main.archive_was_changed"), hstring{lycan::Tf(L"main.while_was_being_edited_another", name, file.wstring())});
            co_return;
        }
        const uint32_t node = tree_->Find(entryPath);
        if (node == lycan::kNoNode || tree_->Node(node).isDir)
        {
            s->prompting = false;
            s->baseline = StampOfFile(file);
            ShowMessage(lycan::Tr(L"main.file_no_longer_archive"), hstring{lycan::Tf(L"main.no_longer_archive_deleted_renamed", name, file.wstring())});
            co_return;
        }
        if (settings_.confirmWriteBack)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.put_back_into_archive"), L"");
            StackPanel sp;
            sp.Spacing(10);
            sp.MaxWidth(440);
            TextBlock t1;
            t1.Text(hstring{lycan::Tf(L"main.was_changed_put_back_into_archive", name)});
            t1.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t1);
            CheckBox again;
            again.Content(box_value(hstring{lycan::Tr(L"main.put_back_without_asking_from_now")}));
            sp.Children().Append(again);
            dlg.Content(sp);
            dlg.PrimaryButtonText(lycan::Tr(L"main.put_back"));
            dlg.CloseButtonText(lycan::Tr(L"main.not_now"));
            dlg.DefaultButton(ContentDialogButton::Primary);
            const auto r = co_await ShowDialogAsync(dlg);
            s = find();
            if (!s) co_return;
            if (r != ContentDialogResult::Primary)
            {
                s->baseline = StampOfFile(file);  // asked again only when it changes again
                s->prompting = false;
                co_return;
            }
            if (again.IsChecked().Value())
            {
                settings_.confirmWriteBack = false;
                loadingSettings_ = true;
                ConfirmWriteBackToggle().IsOn(false);
                loadingSettings_ = false;
                SaveSettings();
            }
        }
        s = find();
        if (!s) co_return;
        s->baseline = StampOfFile(file);  // this is the content that goes in
        s->prompting = false;
        if (!current_ || currentPath_ != s->archive || tree_->Find(entryPath) == lycan::kNoNode) co_return;
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        meta.selectPaths.push_back(entryPath);
        RunUpdateAsync({lycan::UpdateOp::Replace(entryPath, file)}, std::move(meta));
    }

    // Closing the window while a saved file has not gone back: say so
    fire_and_forget MainWindow::AskCloseWithEditsAsync()
    {
        auto strong = get_strong();
        if (askingClose_ || dialogOpen_) co_return;
        askingClose_ = true;
        auto dlg = MakeDialog(lycan::Tr(L"main.changes_not_yet_put_back_into"), L"");
        TextBlock tb;
        tb.Text(lycan::Tr(L"main.file_you_edited_has_not_been"));
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.close_without_putting_back"));
        dlg.CloseButtonText(lycan::Tr(L"main.back"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const auto r = co_await ShowDialogAsync(dlg);
        askingClose_ = false;
        if (r == ContentDialogResult::Primary)
        {
            closeConfirmed_ = true;
            Close();
        }
    }

    // Search / list everything result -> jump to the folder of that item and select it
    void MainWindow::ShowNodeInFolder(uint32_t node)
    {
        if (!tree_ || node >= tree_->NodeCount()) return;
        flat_ = false;
        syncingUi_ = true;
        FlatToggle().IsChecked(false);
        syncingUi_ = false;
        NavigateTo(tree_->Node(node).parent, true);
        for (auto const& it : entries_)
        {
            auto row = it.as<LycanZip::EntryRow>();
            if (row.NodeId() == node)
            {
                ActiveList().SelectedItems().Clear();
                ActiveList().SelectedItems().Append(row);
                ActiveList().ScrollIntoView(row);
                break;
            }
        }
    }

    void MainWindow::ShowProperties(std::vector<uint32_t> const& nodes)
    {
        if (!tree_ || nodes.size() != 1) return;
        auto const& n = tree_->Node(nodes[0]);
        std::wstring parent = tree_->ParentPath(nodes[0]);
        std::wstring t = lycan::Tf(L"main.path", n.path);
        if (n.isDir)
        {
            t += lycan::Tr(L"main.type_folder");
            t += lycan::Tf(L"main.contains_total", n.fileCount, FormatSize(n.totalSize));
        }
        else
        {
            t += lycan::Tr(L"main.type_file");
            t += lycan::Tf(L"main.info_size", FormatSize(n.size), n.size);
            std::wstring packed = lycan::Tf(L"main.info_packed", FormatSize(n.packedSize));
            if (n.size > 0 && n.packedSize > 0)
            {
                packed += lycan::Tf(L"fmt.percent_paren", std::lround(100.0 * static_cast<double>(n.packedSize) / static_cast<double>(n.size)));
            }
            t += packed + L"\n";
        }
        if (n.mtimeFileTime) t += lycan::Tf(L"main.info_modified", FormatTime(n.mtimeFileTime));
        if (n.isEncrypted) t += lycan::Tr(L"main.info_encrypted");
        if (!n.isDir && current_ && n.entryIndex != lycan::kNoEntry && n.entryIndex < current_->Entries().size() && current_->Entries()[n.entryIndex].hasCrc)
        {
            wchar_t crc[16];
            swprintf_s(crc, L"%08x", current_->Entries()[n.entryIndex].crc);
            t += lycan::Tf(L"main.crc_32_stored_archive", crc);  // no need to unpack for this one; the other hash values are in the menu
        }
        // what the archive says besides the columns of the list (only what the format has)
        if (current_ && n.entryIndex != lycan::kNoEntry && n.entryIndex < current_->Entries().size())
        {
            const auto tech = current_->EntryTechInfo(n.entryIndex);
            std::wstring extra;
            if (tech.ctimeFileTime) extra += lycan::Tf(L"main.info_created", FormatTime(tech.ctimeFileTime));
            if (tech.atimeFileTime) extra += lycan::Tf(L"main.info_accessed", FormatTime(tech.atimeFileTime));
            if (!tech.method.empty()) extra += lycan::Tf(L"main.info_method", tech.method);
            if (tech.hasUnixMode)
            {
                std::wstring bits;
                for (int i = 0; i < 9; ++i) bits += (tech.unixMode & (0400u >> i)) ? L"rwxrwxrwx"[i] : L'-';
                wchar_t octal[16];
                swprintf_s(octal, L"%04o", tech.unixMode);
                extra += lycan::Tf(L"main.info_permissions", bits + L"  (" + octal + L")");
            }
            if (!tech.hostOs.empty()) extra += lycan::Tf(L"main.info_host_os", tech.hostOs);
            if (!tech.characteristics.empty()) extra += lycan::Tf(L"main.info_characteristics", tech.characteristics);
            if (!tech.linkTarget.empty()) extra += lycan::Tf(L"main.info_link", tech.linkTarget);
            if (tech.hasBlock) extra += lycan::Tf(L"main.info_block", tech.block);
            if (tech.hasOffset) extra += lycan::Tf(L"main.info_offset", tech.offset);
            if (!tech.comment.empty()) extra += lycan::Tf(L"main.info_comment", tech.comment);
            if (!extra.empty()) t += L"\n" + std::wstring{lycan::Tr(L"main.info_technical")} + extra;
        }
        ShowMessage(hstring{n.name}, hstring{t});
    }

    // ---------------------------------------------------------------- Extract / test

    std::vector<uint32_t> MainWindow::SelectedNodes()
    {
        std::vector<uint32_t> out;
        if (!entries_) return out;
        for (auto const& it : ActiveList().SelectedItems()) out.push_back(it.as<LycanZip::EntryRow>().NodeId());
        return out;
    }

    // Selected items -> Entry indices of the archive (a folder with its content)
    std::vector<uint32_t> MainWindow::SelectedIndices()
    {
        if (!tree_) return {};
        return tree_->EntryIndices(SelectedNodes());
    }

    // The prefix of the folder being viewed: when items selected here are extracted, this upper path is not kept
    std::wstring MainWindow::StripPrefixForView() const
    {
        if (!tree_ || curNode_ == tree_->Root()) return {};
        return tree_->Node(curNode_).path + L"/";
    }

    fire_and_forget MainWindow::StartExtract(fs::path dest, bool selectionOnly, std::optional<lycan::ExtractMode> modeArg)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        // take the selection before the dialog (password) appears, so a later change of the selection does not affect this run
        std::vector<uint32_t> indices;
        std::wstring strip;
        if (selectionOnly)
        {
            indices = SelectedIndices();
            if (indices.empty()) co_return;
            strip = StripPrefixForView();
        }
        StartExtractCore(std::move(dest), std::move(indices), std::move(strip), selectionOnly, modeArg);
    }

    // The folders / files of the menus (list or folder tree): straight into the destination, folder names on top
    fire_and_forget MainWindow::ExtractNodes(std::vector<uint32_t> nodes, std::wstring strip, bool askFolder)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || nodes.empty()) co_return;
        fs::path dest = currentPath_.parent_path();
        if (askFolder)
        {
            dest = fs::path(std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, currentPath_.parent_path(),
                                                                             lycan::Tr(L"main.choose_folder_extract"))));
            if (dest.empty()) co_return;
            RememberRecentExtract(dest);
        }
        auto indices = tree_->EntryIndices(nodes);
        if (indices.empty()) co_return;
        StartExtractCore(std::move(dest), std::move(indices), std::move(strip), true, std::nullopt);
    }

    fire_and_forget MainWindow::StartExtractCore(fs::path dest, std::vector<uint32_t> indices, std::wstring strip, bool selectionOnly,
                                                 std::optional<lycan::ExtractMode> modeArg)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        if (!co_await EnsurePasswordAsync()) co_return;

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        // a selection goes straight into the destination (no wrapping folder); a whole archive follows the mode
        const lycan::ExtractMode mode = selectionOnly ? lycan::ExtractMode::NoFolder : modeArg.value_or(settings_.extractMode);
        eo.smart = mode == lycan::ExtractMode::Auto;
        if (mode == lycan::ExtractMode::Folder) dest = dest / lycan::ArchiveBaseName(currentPath_);
        eo.stripPrefix = strip;
        eo.overwrite = lycan::ToOverwriteMode(settings_.conflict);
        if (eo.overwrite == lycan::OverwriteMode::Ask) { auto rule = std::make_shared<std::atomic<int>>(0); eo.onConflict = [this, rule](lycan::ConflictInfo const& ci) { return AskConflict(ci, rule); }; }
        ApplyLargeGuard(eo);
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };

        TaskMeta meta;
        meta.archive = currentPath_;
        meta.wholeArchive = !selectionOnly;
        {
            std::lock_guard lk(metaM_);
            uint64_t id = queue_.EnqueueExtract(currentPath_, dest, eo, MakeOpenOptions(), indices);
            taskMeta_[id] = std::move(meta);
        }
    }

    fire_and_forget MainWindow::OnExtractHereClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        StartExtract(currentPath_.parent_path());
    }

    void MainWindow::OnExtractHereSplitClick(Microsoft::UI::Xaml::Controls::SplitButton const&, Microsoft::UI::Xaml::Controls::SplitButtonClickEventArgs const&)
    {
        OnExtractHereClick(nullptr, nullptr);
    }

    // The button that ends the compact window: it appears when nothing is running any more ("Done" when everything worked, "Close" when
    // something failed); Enter (it takes the focus) and Esc press it
    void MainWindow::UpdateCompactDone()
    {
        bool show = false, allOk = true;
        if (compact_ && !closing_ && busyOps_ == 0 && !dialogOpen_ && !quickRunning_ && pendingPaths_.empty())
        {
            bool any = false;
            show = true;
            for (auto const& t : queue_.Snapshot())
            {
                if (!t.Finished()) { show = false; break; }
                any = true;
                const bool ok = t.state == lycan::TaskState::Completed && (t.kind != lycan::TaskKind::Extract || t.extract.ok()) &&
                                (t.kind != lycan::TaskKind::Compress || t.compress.ok()) && (t.kind != lycan::TaskKind::Update || t.update.ok());
                if (!ok && !(t.kind == lycan::TaskKind::Test && t.state == lycan::TaskState::Cancelled)) allOk = false;
            }
            show = show && any;
        }
        const bool was = CompactDoneButton().Visibility() == Visibility::Visible;
        if (show)
        {
            const hstring label = allOk ? lycan::Tr(L"main.done") : lycan::Tr(L"main.close");
            CompactDoneButton().Content(box_value(label));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(CompactDoneButton(), label);
        }
        CompactDoneButton().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        if (show && !was) CompactDoneButton().Focus(FocusState::Programmatic);
    }

    void MainWindow::OnCompactDoneClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!compact_ || CompactDoneButton().Visibility() != Visibility::Visible) return;  // (Esc while the button is not there: nothing)
        FadeOutAndClose();
    }

    void MainWindow::UpdateCompactOptions()
    {
        CompactOptions().Visibility(compact_ && !compactHost_ ? Visibility::Visible : Visibility::Collapsed);
        if (!compact_ || compactHost_) return;
        auto setBox = [](CheckBox const& box, const wchar_t* text, const wchar_t* name, bool on, bool visible) {
            box.Content(box_value(hstring{text}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, hstring{name});
            box.IsChecked(on);
            box.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        };
        const bool was = loadingSettings_;
        loadingSettings_ = true;  // setting IsChecked from code must not write back
        if (compactExtract_)
        {
            setBox(OptFirst(), lycan::Tr(L"main.open_folder"), lycan::Tr(L"main.open_folder_when_done"), settings_.openFolderAfterExtract, !compactTest_);
            setBox(OptSecond(), L"", L"", false, false);
        }
        else
        {
            setBox(OptFirst(), lycan::Tr(L"main.show_explorer"), lycan::Tr(L"main.after_completion_show_explorer"), settings_.revealAfterCompress, true);
            setBox(OptSecond(), lycan::Tr(L"main.test_integrity"), lycan::Tr(L"main.after_completion_test_integrity"), settings_.testAfterCompress, true);
        }
        setBox(OptClose(), lycan::Tr(L"main.close_window"), lycan::Tr(L"main.after_completion_close_window"), settings_.closeCompactWhenDone, true);
        loadingSettings_ = was;
    }

    void MainWindow::OnCompactOptionClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        const bool first = OptFirst().IsChecked().Value(), second = OptSecond().IsChecked().Value();
        if (compactExtract_)
            settings_.openFolderAfterExtract = first;
        else
        {
            settings_.revealAfterCompress = first;
            settings_.testAfterCompress = second;
        }
        settings_.closeCompactWhenDone = OptClose().IsChecked().Value();
        LoadSettingsToUi();  // keep the settings page in step
        SaveSettings();
        if (settings_.closeCompactWhenDone) CheckCompactAutoClose();  // it was kept open only because of the option
    }

    void MainWindow::ExtractHereWith(lycan::ExtractMode mode)
    {
        if (current_) StartExtract(currentPath_.parent_path(), false, mode);
    }

    void MainWindow::UpdateExtractButton()
    {
        using lycan::ExtractMode;
        // the folder name in the label is shortened so that the toolbar does not grow with long archive names
        std::wstring base = current_ ? lycan::ArchiveBaseName(currentPath_) : std::wstring(lycan::Tr(L"main.file_name_short"));
        if (base.size() > 18) base = base.substr(0, 17) + L"…";
        auto label = [&](ExtractMode m, bool asMenuItem) -> std::wstring {
            switch (m) {
                case ExtractMode::NoFolder: return lycan::Tr(L"main.extract_here_no_folder");
                case ExtractMode::Folder: return lycan::Tf(L"main.extract_folder", base);
                default: return asMenuItem ? lycan::Tr(L"main.extract_here_automatic") : lycan::Tr(L"main.extract_here");
            }
        };
        const ExtractMode main = settings_.extractMode;
        const std::wstring text = label(main, false);
        ExtractHereText().Text(hstring{text});
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(ExtractHereButton(), hstring{text});
        std::wstring tip = main == ExtractMode::NoFolder ? lycan::Tr(L"main.extract_here_straight_into_folder")
                         : main == ExtractMode::Folder   ? lycan::Tr(L"main.extract_here_always_into_folder")
                                                         : lycan::Tr(L"main.extract_here_ctrl_e_creates_name");
        ToolTipService::SetToolTip(ExtractHereButton(), box_value(hstring{tip}));
        MenuFlyout flyout;
        for (ExtractMode m : {ExtractMode::Auto, ExtractMode::NoFolder, ExtractMode::Folder}) {
            if (m == main) continue;
            MenuFlyoutItem it;
            it.Text(hstring{label(m, true)});
            it.Click([weak = get_weak(), m](auto&&, auto&&) { if (auto s = weak.get()) s->ExtractHereWith(m); });
            flyout.Items().Append(it);
        }
        ExtractHereButton().Flyout(flyout);
        BuildExtractToMenu();
    }

    void MainWindow::OnExtractToSplitClick(SplitButton const&, SplitButtonClickEventArgs const&) { OnExtractToClick(nullptr, nullptr); }

    // The drop-down of "Extract to...": the folders that were chosen before, and the folders people extract to most (extracts there at once)
    void MainWindow::BuildExtractToMenu()
    {
        MenuFlyout flyout;
        auto weak = get_weak();
        flyout.Opening([weak](IInspectable const& sender, IInspectable const&) {
            auto s = weak.get();
            if (!s) return;
            auto menu = sender.as<MenuFlyout>();
            menu.Items().Clear();
            auto add = [&](std::wstring const& dir, std::wstring const& label) {
                MenuFlyoutItem it;
                std::wstring shown = label;
                if (shown.size() > 56) shown = shown.substr(0, 26) + L"…" + shown.substr(shown.size() - 29);   // a long path: cut in the middle
                it.Text(hstring{shown});
                it.Icon(MenuIcon(L"\uE8B7"));
                ToolTipService::SetToolTip(it, box_value(hstring{dir}));
                it.Click([weak, dir](auto&&, auto&&) {
                    if (auto self = weak.get()) { if (self->current_) { self->RememberRecentExtract(fs::path{dir}); self->StartExtract(fs::path{dir}); } }
                });
                menu.Items().Append(it);
            };
            for (auto const& d : s->settings_.recentExtract) add(d, d);
            if (!s->settings_.recentExtract.empty()) menu.Items().Append(MenuFlyoutSeparator{});
            struct Known { const KNOWNFOLDERID* id; const wchar_t* label; };
            for (auto const& k : {Known{&FOLDERID_Desktop, L"main.desktop"}, Known{&FOLDERID_Documents, L"main.documents"}, Known{&FOLDERID_Downloads, L"main.downloads"}})
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(*k.id, 0, nullptr, &path)) && path)
                {
                    add(path, std::wstring{lycan::Tr(k.label)});
                    CoTaskMemFree(path);
                }
            }
        });
        ExtractToButton().Flyout(flyout);
    }

    fire_and_forget MainWindow::OnExtractToClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, currentPath_.parent_path(),
                                                                            lycan::Tr(L"main.choose_folder_extract")))};
        if (!dir.empty()) { RememberRecentExtract(dir); StartExtract(dir); }
    }

    // Selection bar / context menu: extract only the selected items
    fire_and_forget MainWindow::OnExtractSelHereClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        StartExtract(currentPath_.parent_path(), true);
    }

    fire_and_forget MainWindow::OnExtractSelToClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_ || SelectedNodes().empty()) co_return;
        const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, currentPath_.parent_path(),
                                                                            lycan::Tr(L"main.choose_folder_extract")))};
        if (!dir.empty()) { RememberRecentExtract(dir); StartExtract(dir, true); }
    }

    // "Scan for malware": a task; what it found (or why it could not) is told when it is done
    fire_and_forget MainWindow::OnScanClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        if (current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;  // without the password the encrypted files are skipped; ask first
        queue_.EnqueueScan(currentPath_, MakeOpenOptions());
    }

    void MainWindow::FinishScanTask(lycan::TaskInfo const& info)
    {
        if (info.state == lycan::TaskState::Cancelled) return;
        auto const& r = info.scan;
        if (r.amsiHr != 0)
        {
            ShowMessage(lycan::Tr(L"main.scan_could_not_run"), hstring{lycan_app::ScanProblemText(r.amsiHr) + L"\n\n" +
                                                      lycan::Tr(L"main.scan_hands_files_antivirus_product")});
            return;
        }
        if (!r.Found()) return;  // the card says what was scanned
        std::wstring list;
        size_t shown = 0;
        for (auto const& t : r.threats)
        {
            if (shown++ >= 15) break;
            list += L"\n• " + t.path;
            if (t.blocked) list += lycan::Tr(L"main.blocked_by_system_policy");
            else if (!t.name.empty())
            {
                list += L"\n    " + t.name;
                if (!t.severity.empty() || !t.category.empty())
                    list += lycan::Tf(L"fmt.paren", t.severity + (t.severity.empty() || t.category.empty() ? std::wstring() : std::wstring{lycan::Tr(L"fmt.comma")}) + t.category);
            }
        }
        if (r.threats.size() > shown) list += L"\n" + lycan::Tf(L"main.more", r.threats.size() - shown);
        bool unmatched = false;
        for (auto const& t : r.threats) if (!t.blocked && t.name.empty()) unmatched = true;
        if (unmatched && !r.threatNames.empty())
        {
            std::wstring all;
            for (auto const& n : r.threatNames) all += (all.empty() ? std::wstring{} : std::wstring{lycan::Tr(L"fmt.list_sep")}) + n;
            list += L"\n\n" + lycan::Tf(L"main.threat_names_logged_by_windows", all);
        }
        ShowMessage(lycan::Tr(L"main.suspicious_files_found"),
                    hstring{lycan::Tf(L"main.installed_antivirus_product_thinks", r.threats.size()) + list + L"\n\n" +
                            lycan::Tr(L"main.do_not_open_these_files_antivirus") + L"\n\n" + lycan::Tr(L"scan.affected_item_note")});
    }

    fire_and_forget MainWindow::OnTestClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        if (!co_await EnsurePasswordAsync()) co_return;
        queue_.EnqueueTest(currentPath_, MakeOpenOptions());
    }

    // ---------------------------------------------------------------- Conflict question

    // Automatic rules (the index in the drop-down of the dialog; 0 = ask every time). When it cannot be decided (for example the modified time is missing) it is always "keep both", so no data is lost.
    static const wchar_t* const kAutoRuleNames[] = {
        L"main.ask_every_time_no_automatic_choice", L"main.keep_newer", L"main.keep_older", L"main.keep_larger", L"main.keep_smaller", L"main.skip_if_contents_identical"};

    static lycan::ConflictAction ApplyAutoRule(int rule, lycan::ConflictInfo const& ci)
    {
        using A = lycan::ConflictAction;
        auto pick = [](bool known, uint64_t existing, uint64_t incoming, bool preferHigher) {
            if (!known) return A::Rename;
            if (existing == incoming) return A::Skip;  // tie: keep the existing one
            return (incoming > existing) == preferHigher ? A::Overwrite : A::Skip;
        };
        const bool timeKnown = ci.existingMTime && ci.newMTime;
        switch (rule)
        {
        case 1: return pick(timeKnown, ci.existingMTime, ci.newMTime, true);
        case 2: return pick(timeKnown, ci.existingMTime, ci.newMTime, false);
        case 3: return pick(true, ci.existingSize, ci.newSize, true);
        case 4: return pick(true, ci.existingSize, ci.newSize, false);
        case 5: return (timeKnown && ci.existingSize == ci.newSize && ci.existingMTime == ci.newMTime) ? A::Skip : A::Rename;
        }
        return A::Rename;
    }

    // The check of a suspiciously big archive: on unless Settings > Extract says no (the tests make the limits small: LYCAN_TEST_LARGE_BYTES)
    void MainWindow::ApplyLargeGuard(lycan::ExtractOptions& eo)
    {
        if (!settings_.bombCheck) return;
        eo.onLarge = [this](lycan::LargeExtractInfo const& i) { return AskLarge(i); };
        wchar_t buf[24];
        if (GetEnvironmentVariableW(L"LYCAN_TEST_LARGE_BYTES", buf, static_cast<DWORD>(std::size(buf))) > 0)
        {
            eo.largeBytes = _wcstoui64(buf, nullptr, 10);
            eo.largeRatio = 50;
        }
    }

    bool MainWindow::AskLarge(lycan::LargeExtractInfo const& info)
    {
        auto req = std::make_shared<LargeRequest>();
        req->info = info;
        Post([this, req] { ShowLargeDialog(req); });
        std::unique_lock lk(req->m);
        while (!req->done)
        {
            if (closing_) return false;
            req->cv.wait_for(lk, 200ms);
        }
        return req->answer;
    }

    fire_and_forget MainWindow::ShowLargeDialog(std::shared_ptr<LargeRequest> req)
    {
        auto strong = get_strong();
        PlaySfx(L"conflict");
        auto const& li = req->info;
        auto dlg = MakeDialog(lycan::Tr(L"main.archive_unpacks_something_very_big"), L"\uE7BA");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(460);
        const uint64_t ratio = li.packedBytes ? li.declaredBytes / li.packedBytes : 0;
        TextBlock tb;
        tb.Text(hstring{ratio ? lycan::Tf(L"main.archive_only_unpacks_about_times", FormatSize(li.packedBytes), FormatSize(li.declaredBytes), ratio)
                              : lycan::Tf(L"main.unpacks_about", FormatSize(li.declaredBytes))});
        tb.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        tb.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(tb);
        TextBlock note;
        note.Text(lycan::Tr(L"main.far_more_than_normal_archive_may"));
        note.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(note);
        CheckBox never;
        never.Content(box_value(lycan::Tr(L"main.do_not_check_again_can_be")));
        sp.Children().Append(never);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.continue_extracting"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const bool go = co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary;
        if (go && never.IsChecked().Value())
        {
            settings_.bombCheck = false;
            SaveSettings();
            LoadSettingsToUi();
        }
        req->Finish(go);
    }

    lycan::ConflictAnswer MainWindow::AskConflict(lycan::ConflictInfo const& info, std::shared_ptr<std::atomic<int>> const& rule)
    {
        if (int r = rule->load()) return {ApplyAutoRule(r, info), false};
        auto req = std::make_shared<ConflictRequest>();
        req->info = info;
        req->rule = rule;
        Post([this, req] { ShowConflictDialog(req); });

        std::unique_lock lk(req->m);
        while (!req->done)
        {
            if (closing_) return {lycan::ConflictAction::Cancel, false};
            req->cv.wait_for(lk, 200ms);
        }
        return req->answer;
    }

    fire_and_forget MainWindow::ShowConflictDialog(std::shared_ptr<ConflictRequest> req)
    {
        auto strong = get_strong();
        auto const& ci = req->info;
        PlaySfx(L"conflict");

        auto dlg = MakeDialog(lycan::Tr(L"main.conflict_title"), L"\uE8C8");
        StackPanel sp;
        sp.Spacing(14);
        sp.MinWidth(440);

        const std::wstring fileName = ci.existing.filename().wstring();
        const FileCategory cat = CategoryOfName(fileName, false, false);
        {
            StackPanel where;
            where.Spacing(2);
            TextBlock n;
            n.Text(hstring{fileName});
            n.FontSize(16);
            n.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            n.TextTrimming(TextTrimming::CharacterEllipsis);
            where.Children().Append(n);
            TextBlock loc = Caption(ci.existing.parent_path().wstring(), 0.65);
            loc.IsTextSelectionEnabled(true);
            where.Children().Append(loc);
            sp.Children().Append(where);
        }

        // Two versions side by side: the existing file vs the version in the archive, marking the newer / larger one
        const bool known = ci.existingMTime && ci.newMTime;
        const bool newer = known && ci.newMTime > ci.existingMTime;
        const bool older = known && ci.newMTime < ci.existingMTime;
        auto makeSide = [&](const wchar_t* label, uint64_t size, uint64_t mtime, std::wstring const& badge, bool accentBadge) {
            StackPanel side;
            side.Spacing(6);
            side.Children().Append(Caption(label, 0.7));
            StackPanel line;
            line.Orientation(Orientation::Horizontal);
            line.Spacing(8);
            line.Children().Append(MakeGlyph(GlyphForCategory(cat), 24, BrushForCategory(cat)));
            StackPanel info;
            TextBlock sz;
            sz.Text(hstring{FormatSize(size)});
            sz.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            info.Children().Append(sz);
            info.Children().Append(Caption(FormatTime(mtime), 0.75));
            line.Children().Append(info);
            side.Children().Append(line);
            if (!badge.empty())
            {
                Border pill;
                pill.CornerRadius(CornerRadiusHelper::FromUniformRadius(10));
                pill.Padding(ThicknessHelper::FromLengths(8, 1, 8, 1));
                pill.HorizontalAlignment(HorizontalAlignment::Left);
                auto c = accentBadge ? AccentColorNow() : Windows::UI::Color{255, 160, 170, 190};
                pill.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x30, c.R, c.G, c.B}});
                TextBlock bt;
                bt.Text(hstring{badge});
                bt.FontSize(12);
                bt.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{c});
                pill.Child(bt);
                side.Children().Append(pill);
            }
            return MakeCardBox(side, 14, 12);
        };
        auto sizeNote = [&](bool forArchive) -> std::wstring {
            if (ci.newSize == ci.existingSize) return lycan::Tr(L"main.same_size");
            bool bigger = forArchive ? ci.newSize > ci.existingSize : ci.existingSize > ci.newSize;
            return bigger ? lycan::Tr(L"main.larger") : lycan::Tr(L"main.smaller");
        };
        Grid cmp;
        cmp.ColumnSpacing(12);
        for (int i = 0; i < 2; ++i) { ColumnDefinition cd; cd.Width(GridLength{1, GridUnitType::Star}); cmp.ColumnDefinitions().Append(cd); }
        auto left = makeSide(lycan::Tr(L"main.existing_file"), ci.existingSize, ci.existingMTime, older ? lycan::Tr(L"main.newer") : (known && !newer ? L"" : L""), true);
        auto right = makeSide(lycan::Tr(L"main.version_archive"), ci.newSize, ci.newMTime, newer ? lycan::Tr(L"main.newer") : older ? lycan::Tr(L"main.older") : (known ? lycan::Tr(L"main.same_time") : L""), newer);
        Grid::SetColumn(right, 1);
        cmp.Children().Append(left);
        cmp.Children().Append(right);
        sp.Children().Append(cmp);
        sp.Children().Append(Caption(lycan::Tf(L"main.conflict_archive_version", sizeNote(true)), 0.65));

        RadioButtons options;
        options.Header(box_value(lycan::Tr(L"main.what_would_you_like_do")));
        options.Items().Append(TitledOption(lycan::Tr(L"main.overwrite_existing_file"), lycan::Tr(L"main.replace_with_version_archive")));
        options.Items().Append(TitledOption(lycan::Tr(L"main.keep_both"), lycan::Tr(L"main.new_file_renamed_automatically")));
        options.Items().Append(TitledOption(lycan::Tr(L"main.skip_file"), lycan::Tr(L"main.keep_existing_file_do_not_extract")));
        options.SelectedIndex(1);  // the default is "keep both": no data is lost
        sp.Children().Append(options);

        CheckBox all;
        all.Content(box_value(lycan::Tr(L"main.apply_same_choice_all_later")));
        sp.Children().Append(all);

        // Automatic choice: decided file by file by the rule (newer / older / larger / smaller / skip when the same); once chosen, no conflict of this extraction is asked again
        ComboBox autoRule;
        autoRule.Header(box_value(lycan::Tr(L"main.choose_automatically")));
        autoRule.HorizontalAlignment(HorizontalAlignment::Stretch);
        Automation::AutomationProperties::SetName(autoRule, lycan::Tr(L"main.automatic_choice"));
        for (auto n : kAutoRuleNames) autoRule.Items().Append(box_value(lycan::Tr(n)));
        autoRule.SelectedIndex(0);
        autoRule.SelectionChanged([options, all, autoRule](auto&&, auto&&) {
            const bool manual = autoRule.SelectedIndex() <= 0;
            options.IsEnabled(manual);
            all.IsEnabled(manual);
        });
        sp.Children().Append(autoRule);

        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel_extraction"));
        dlg.DefaultButton(ContentDialogButton::Primary);

        auto result = co_await ShowDialogAsync(dlg);
        if (result != ContentDialogResult::Primary)
        {
            req->Finish({lycan::ConflictAction::Cancel, false});
            co_return;
        }
        if (int r = autoRule.SelectedIndex(); r > 0)
        {
            if (req->rule) req->rule->store(r);
            req->Finish({ApplyAutoRule(r, ci), false});
            co_return;
        }
        lycan::ConflictAction a = options.SelectedIndex() == 0 ? lycan::ConflictAction::Overwrite
                                  : options.SelectedIndex() == 2 ? lycan::ConflictAction::Skip
                                                                 : lycan::ConflictAction::Rename;
        req->Finish({a, all.IsChecked().Value()});
    }

    // ---------------------------------------------------------------- Creating an archive

    fire_and_forget MainWindow::OnNewFromFilesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeSourceFiles, true, false);
        std::vector<fs::path> paths;
        for (auto const& f : files) paths.emplace_back(std::wstring(f));
        if (!paths.empty()) StartCompress(std::move(paths));
    }

    fire_and_forget MainWindow::OnNewFromFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeSourceFolder, {}, lycan::Tr(L"main.choose_folder_compress")))};
        if (!dir.empty()) StartCompress({dir});
    }

    // ---------------------------------------------------------------- "New archive" page

    namespace
    {
        struct ValueItem { const wchar_t* text; int64_t value; };
        constexpr const wchar_t* kFormatNames[] = {L"ZIP", L"7z", L"TAR.GZ", L"TAR.XZ", L"TAR.BZ2", L"TAR", L"ISO"};
        constexpr const wchar_t* kProfileNames[] = {L"main.balanced_recommended", L"main.compatibility_first", L"main.smallest_size", L"main.fastest"};
        constexpr const wchar_t* kProfileShort[] = {L"main.balanced", L"main.compatibility_first", L"main.smallest_size", L"main.fastest"};

        ComboBoxItem MakeItem(hstring const& text, Windows::Foundation::IInspectable const& tag)
        {
            ComboBoxItem it;
            it.Content(box_value(text));
            it.Tag(tag);
            return it;
        }

        void FillValues(ComboBox const& c, std::initializer_list<ValueItem> items)
        {
            c.Items().Clear();
            for (auto const& v : items) c.Items().Append(MakeItem(v.text, box_value<int64_t>(v.value)));
            c.SelectedIndex(0);
        }

        int64_t ComboValue(ComboBox const& c, int64_t dflt)
        {
            auto it = c.SelectedItem().try_as<ComboBoxItem>();
            return it ? unbox_value_or<int64_t>(it.Tag(), dflt) : dflt;
        }

        // Select the given value; if the list does not have it (for example a preset stored a custom size) add an item
        void SelectValue(ComboBox const& c, int64_t v, std::wstring const& customText)
        {
            for (uint32_t i = 0; i < c.Items().Size(); ++i)
            {
                auto it = c.Items().GetAt(i).try_as<ComboBoxItem>();
                if (it && unbox_value_or<int64_t>(it.Tag(), INT64_MIN) == v) { c.SelectedIndex(static_cast<int32_t>(i)); return; }
            }
            c.Items().Append(MakeItem(hstring{customText}, box_value<int64_t>(v)));
            c.SelectedIndex(static_cast<int32_t>(c.Items().Size()) - 1);
        }

        std::wstring EffectiveMethod(lycan::CompressPreset const& p)
        {
            if (!p.method.empty()) return p.method;
            return p.type == lycan::ArchiveType::SevenZip ? L"LZMA2" : p.type == lycan::ArchiveType::Zip ? L"Deflate" : L"";
        }

        // Zero the fields that are not used: a disabled control may still hold an old value, which must not affect the result or the judgment of "modified"
        void NormalizePreset(lycan::CompressPreset& p)
        {
            const bool zip = p.type == lycan::ArchiveType::Zip, sz = p.type == lycan::ArchiveType::SevenZip;
            if (!sz) { p.solidSize = lycan::kSolidAuto; p.encryptFileNames = false; }
            if (!zip) p.zipEncryption.clear();
            if (p.type == lycan::ArchiveType::Iso) { p.profile = lycan::Profile::Balanced; p.level = -1; p.threads = 0; p.pathMode = lycan::PathMode::Relative; p.customParams.clear(); }
            if (!zip && !sz) { p.method.clear(); p.volumeSize = 0; p.dictSize = 0; p.wordSize = 0; p.storeCompressed = true; }
            else
            {
                auto m = EffectiveMethod(p);
                if (!lycan::MethodHasDict(m)) p.dictSize = 0;
                if (!lycan::MethodHasWordSize(m)) p.wordSize = 0;
            }
        }

        std::wstring TrimW(std::wstring s)
        {
            while (!s.empty() && iswspace(s.back())) s.pop_back();
            size_t i = 0;
            while (i < s.size() && iswspace(s[i])) ++i;
            return s.substr(i);
        }

        constexpr int64_t kVolCustom = -2;  // the value of the "Custom..." item

        // A size typed by the user: "50 MB", "1.5gb", "700" (no unit = MB). At least 64 KB (the lower limit of the Core), at most 1 TB.
        std::optional<uint64_t> ParseSizeInput(std::wstring s)
        {
            s = TrimW(s);
            size_t i = 0;
            bool dot = false;
            while (i < s.size() && (iswdigit(s[i]) || (!dot && s[i] == L'.'))) { dot |= s[i] == L'.'; ++i; }
            if (i == 0) return std::nullopt;
            double num = 0;
            try { num = std::stod(s.substr(0, i)); } catch (...) { return std::nullopt; }
            std::wstring unit = TrimW(s.substr(i));
            for (auto& ch : unit) ch = static_cast<wchar_t>(towlower(ch));
            double mult = 1048576.0;
            if (unit == L"b") mult = 1;
            else if (unit == L"k" || unit == L"kb") mult = 1024.0;
            else if (unit == L"m" || unit == L"mb" || unit.empty()) mult = 1048576.0;
            else if (unit == L"g" || unit == L"gb") mult = 1073741824.0;
            else return std::nullopt;
            const double bytes = num * mult;
            if (!(bytes >= 65536.0) || bytes > 1099511627776.0) return std::nullopt;
            return static_cast<uint64_t>(std::llround(bytes));
        }

        std::wstring FormatSizeInput(uint64_t b)
        {
            if (b % (1ull << 30) == 0) return std::to_wstring(b >> 30) + L" GB";
            if (b % (1ull << 20) == 0) return std::to_wstring(b >> 20) + L" MB";
            if (b % 1024 == 0) return std::to_wstring(b >> 10) + L" KB";
            return std::to_wstring(b) + L" B";
        }

        std::wstring DescribePreset(lycan::CompressPreset const& p, bool hasPassword)
        {
            std::vector<std::wstring> parts;
            parts.push_back(kFormatNames[IndexOf(kFormatOrder, p.type)]);
            if (!p.method.empty()) parts.push_back(p.method);
            if (p.level == 0) parts.push_back(lycan::Tr(L"main.store_only"));
            else if (p.level > 0) parts.push_back(lycan::Tf(L"main.level", p.level));
            else parts.push_back(lycan::Tr(kProfileShort[IndexOf(kProfileOrder, p.profile)]));
            if (p.type == lycan::ArchiveType::SevenZip)
            {
                if (p.solidSize == lycan::kSolidAll) parts.push_back(lycan::Tr(L"main.single_solid_block"));
                else if (p.solidSize == lycan::kSolidOff) parts.push_back(lycan::Tr(L"main.non_solid"));
            }
            if (hasPassword) parts.push_back(p.encryptFileNames ? lycan::Tr(L"main.password_protected_incl_file_names") : lycan::Tr(L"main.password_protected"));
            if (p.volumeSize) parts.push_back(lycan::Tf(L"main.split", FormatSize(p.volumeSize)));
            if (p.pathMode == lycan::PathMode::Full) parts.push_back(lycan::Tr(L"main.full_path"));
            std::wstring out;
            for (size_t i = 0; i < parts.size(); ++i) out += (i ? L" · " : L"") + parts[i];
            return out;
        }
    }

    void MainWindow::InitCompressPage()
    {
        auto weak = get_weak();
        // Static option lists
        for (auto n : kFormatNames) CFormatCombo().Items().Append(box_value(hstring{n}));
        for (auto n : kProfileNames) CProfileCombo().Items().Append(box_value(hstring{lycan::Tr(n)}));
        FillValues(CLevelCombo(), {{lycan::Tr(L"main.by_mode_default"), -1}, {lycan::Tr(L"main.store_only_no_compression"), 0}, {lycan::Tr(L"main.1_fastest"), 1}, {lycan::Tr(L"main.3_fast"), 3}, {lycan::Tr(L"main.5_normal"), 5}, {lycan::Tr(L"main.7_maximum"), 7}, {lycan::Tr(L"main.9_ultra"), 9}});
        FillValues(CDictCombo(), {{lycan::Tr(L"main.auto"), 0}, {L"64 KB", 64ll << 10}, {L"256 KB", 256ll << 10}, {L"1 MB", 1ll << 20}, {L"4 MB", 4ll << 20},
                                  {L"16 MB", 16ll << 20}, {L"32 MB", 32ll << 20}, {L"64 MB", 64ll << 20}, {L"128 MB", 128ll << 20},
                                  {L"256 MB", 256ll << 20}, {L"512 MB", 512ll << 20}, {L"1 GB", 1ll << 30}});
        FillValues(CSolidCombo(), {{lycan::Tr(L"main.auto"), lycan::kSolidAuto}, {lycan::Tr(L"main.non_solid_each_file_separately"), lycan::kSolidOff}, {L"1 MB", 1ll << 20}, {L"16 MB", 16ll << 20},
                                   {L"64 MB", 64ll << 20}, {L"256 MB", 256ll << 20}, {L"1 GB", 1ll << 30}, {L"4 GB", 4ll << 30},
                                   {lycan::Tr(L"main.all_single_block_smallest"), lycan::kSolidAll}});
        FillValues(CThreadsCombo(), {{lycan::Tr(L"main.auto"), 0}, {L"1", 1}, {L"2", 2}, {L"4", 4}, {L"8", 8}, {L"16", 16}, {L"32", 32}});
        FillValues(CZipEncCombo(), {{lycan::Tr(L"main.by_mode_aes_256"), 0}, {L"AES-256", 1}, {lycan::Tr(L"main.zipcrypto_compatible_with_old"), 2}});
        FillValues(CPathModeCombo(), {{lycan::Tr(L"main.relative_path"), 0}, {lycan::Tr(L"main.full_path_without_drive_letter"), 1}});
        FillValues(CVolCombo(), {{lycan::Tr(L"main.do_not_split"), 0}, {lycan::Tr(L"main.25_mb_e_mail_attachment"), 25ll << 20}, {L"100 MB", 100ll << 20}, {L"700 MB (CD)", 700ll << 20},
                                 {L"1 GB", 1ll << 30}, {L"2 GB", 2ll << 30}, {lycan::Tr(L"main.4_gb_fat32_limit"), 4ll << 30},
                                 {lycan::Tr(L"main.custom"), kVolCustom}});
        RebuildMethodItems(lycan::ArchiveType::Zip);
        RebuildWordItems(false);

        auto changed = [weak](auto&&, auto&&) { if (auto s = weak.get()) s->RefreshCompressUi(); };
        for (auto c : {CProfileCombo(), CLevelCombo(), CDictCombo(), CWordCombo(), CSolidCombo(), CThreadsCombo(), CZipEncCombo(), CPathModeCombo(), CVolCombo()})
            c.SelectionChanged(changed);
        CFormatCombo().SelectionChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->cLoading_ = true;
            s->RebuildMethodItems(kFormatOrder[std::clamp(s->CFormatCombo().SelectedIndex(), 0, 6)]);
            s->RebuildWordItems(false);
            s->cLoading_ = false;
            s->RefreshCompressUi();
        });
        CMethodCombo().SelectionChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->cLoading_ = true;
            auto it = s->CMethodCombo().SelectedItem().try_as<ComboBoxItem>();
            s->RebuildWordItems(it && _wcsicmp(unbox_value_or<hstring>(it.Tag(), L"").c_str(), L"PPMd") == 0);
            s->cLoading_ = false;
            s->RefreshCompressUi();
        });
        CShowPw().IsChecked(true);
        CPwText().TextChanged(changed);
        CPwBox().PasswordChanged(changed);
        CPwConfirm().PasswordChanged(changed);
        CVolCustom().TextChanged(changed);
        CShowPw().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->ApplyPasswordMode(); });
        CParamsBox().TextChanged(changed);
        CEncNames().Toggled(changed);
        CStoreCompressed().Toggled(changed);
        CTestAfter().Toggled([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->settings_.testAfterCompress = s->CTestAfter().IsOn();  // remembered (the same setting as the compact window and the settings page)
            s->SaveSettings();
            s->LoadSettingsToUi();
        });
        CSumsArchive().Toggled(changed);   // part of the template (the settings page has the defaults for the right-click compress without a template)
        CSumsFiles().Toggled(changed);
        CNameBox().TextChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->cNameEdited_ = true;
            s->RefreshCompressUi();
        });
        CPresetCombo().SelectionChanged([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            auto it = s->CPresetCombo().SelectedItem().try_as<ComboBoxItem>();
            if (!it) return;
            std::wstring name{unbox_value_or<hstring>(it.Tag(), L"")};
            for (auto const* list : {&s->cBuiltin_, &s->cUser_})
                for (auto const& p : *list)
                    if (p.name == name) { s->cBase_ = p; s->LoadPresetToUi(p); return; }
        });
        CChangeDirButton().Click([weak](auto&&, auto&&) -> fire_and_forget {
            auto s = weak.get();
            if (!s) co_return;
            const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(s->AppWindow().Id(), lycan_app::kPurposeCompressOut, s->cOutDir_, lycan::Tr(L"main.choose_output_folder_archive")))};
            if (!dir.empty()) { s->cOutDir_ = dir; s->RefreshCompressUi(); }
        });
        CAddFiles().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->AddSourcesFromPicker(false); });
        CAddFolder().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->AddSourcesFromPicker(true); });
        CBackButton().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->LeaveCompressPage(); });
        CCancelButton().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->LeaveCompressPage(); });
        CStartButton().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->OnCompressStart(); });
        CResetAdvanced().Click([weak](auto&&, auto&&) { if (auto s = weak.get()) s->LoadPresetToUi(s->cBase_); });
        CAdvanced().Expanding([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->settings_.compressAdvancedOpen = true;
            s->SaveSettings();
        });
        CAdvanced().Collapsed([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s || s->cLoading_) return;
            s->settings_.compressAdvancedOpen = false;
            s->SaveSettings();
        });

        // Preset menu
        MenuFlyout menu;
        auto add = [&](const wchar_t* text, auto&& handler) {
            MenuFlyoutItem item;
            item.Text(text);
            item.Click([weak, handler](auto&&, auto&&) { if (auto s = weak.get()) handler(*s); });
            menu.Items().Append(item);
            return item;
        };
        add(lycan::Tr(L"main.menu_save_as_new_template"), [](MainWindow& s) { s.OnPresetSaveNew(); });
        cMenuUpdate_ = add(lycan::Tr(L"main.update_template_with_current"), [](MainWindow& s) { s.OnPresetUpdate(); });
        cMenuRename_ = add(lycan::Tr(L"main.menu_rename_template"), [](MainWindow& s) { s.OnPresetRename(); });
        cMenuDelete_ = add(lycan::Tr(L"main.delete_template"), [](MainWindow& s) { s.OnPresetDelete(); });
        menu.Items().Append(MenuFlyoutSeparator{});
        add(lycan::Tr(L"main.restore_template_s_settings"), [](MainWindow& s) { s.LoadPresetToUi(s.cBase_); });
        menu.Items().Append(MenuFlyoutSeparator{});
        cMenuInMenu_ = ToggleMenuFlyoutItem{};
        cMenuInMenu_.Text(lycan::Tr(L"main.show_right_click_menu_compress"));
        cMenuInMenu_.Click([weak](auto&&, auto&&) {
            auto s = weak.get();
            if (!s) return;
            s->SetPresetInMenu(s->cBase_.name, s->cBase_.builtin, s->cMenuInMenu_.IsChecked());
        });
        menu.Items().Append(cMenuInMenu_);
        CPresetMenu().Flyout(menu);
    }

    std::wstring MainWindow::CompressPassword()
    {
        return CShowPw().IsChecked().Value() ? std::wstring{CPwText().Text()} : std::wstring{CPwBox().Password()};
    }

    void MainWindow::SetCompressPassword(std::wstring const& pw)
    {
        const bool was = cLoading_;
        cLoading_ = true;
        CPwText().Text(hstring{pw});
        CPwBox().Password(hstring{pw});
        CPwConfirm().Password(L"");
        cLoading_ = was;
    }

    // Toggle "show password": carries what was typed over to the other kind of box
    void MainWindow::ApplyPasswordMode()
    {
        const bool show = CShowPw().IsChecked().Value();
        const bool was = cLoading_;
        cLoading_ = true;
        if (show) CPwText().Text(CPwBox().Password());
        else { CPwBox().Password(CPwText().Text()); CPwConfirm().Password(L""); }
        cLoading_ = was;
        RefreshCompressUi();
    }

    void MainWindow::RebuildMethodItems(lycan::ArchiveType type)
    {
        auto c = CMethodCombo();
        c.Items().Clear();
        auto methods = lycan::MethodsFor(type);
        if (methods.empty()) c.Items().Append(MakeItem(lycan::Tr(L"main.fixed_format"), box_value(hstring{})));
        else
        {
            c.Items().Append(MakeItem(lycan::Tr(L"main.auto"), box_value(hstring{})));
            for (auto const& m : methods) c.Items().Append(MakeItem(hstring{m}, box_value(hstring{m})));
        }
        c.SelectedIndex(0);
    }

    // For PPMd the "word size" is the model order (2-32), for other methods it is the word length
    void MainWindow::RebuildWordItems(bool ppmd)
    {
        auto c = CWordCombo();
        const int64_t keep = ComboValue(c, 0);
        c.Items().Clear();
        c.Items().Append(MakeItem(lycan::Tr(L"main.auto"), box_value<int64_t>(0)));
        bool found = keep == 0;
        if (ppmd) for (int64_t v : {4, 6, 8, 12, 16, 24, 32}) { c.Items().Append(MakeItem(hstring{std::to_wstring(v)}, box_value<int64_t>(v))); found |= v == keep; }
        else for (int64_t v : {8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256}) { c.Items().Append(MakeItem(hstring{std::to_wstring(v)}, box_value<int64_t>(v))); found |= v == keep; }
        if (found) SelectValue(c, keep, std::to_wstring(keep));
        else c.SelectedIndex(0);
    }

    lycan::CompressPreset MainWindow::ReadCompressUi()
    {
        lycan::CompressPreset p;
        p.type = kFormatOrder[std::clamp(CFormatCombo().SelectedIndex(), 0, 6)];
        p.profile = kProfileOrder[std::clamp(CProfileCombo().SelectedIndex(), 0, 3)];
        p.level = static_cast<int>(ComboValue(CLevelCombo(), -1));
        if (auto it = CMethodCombo().SelectedItem().try_as<ComboBoxItem>()) p.method = std::wstring{unbox_value_or<hstring>(it.Tag(), L"")};
        p.dictSize = static_cast<uint64_t>(std::max<int64_t>(0, ComboValue(CDictCombo(), 0)));
        p.wordSize = static_cast<unsigned>(std::max<int64_t>(0, ComboValue(CWordCombo(), 0)));
        p.solidSize = ComboValue(CSolidCombo(), lycan::kSolidAuto);
        const int ze = static_cast<int>(ComboValue(CZipEncCombo(), 0));
        p.zipEncryption = ze == 1 ? L"AES256" : ze == 2 ? L"ZipCrypto" : L"";
        p.encryptFileNames = CEncNames().IsOn();
        p.storeCompressed = CStoreCompressed().IsOn();
        p.sumsArchive = CSumsArchive().IsOn();
        p.sumsFiles = CSumsFiles().IsOn();
        const int64_t vol = ComboValue(CVolCombo(), 0);
        cVolInvalid_ = false;
        if (vol == kVolCustom)
        {
            auto bytes = ParseSizeInput(std::wstring{CVolCustom().Text()});
            cVolInvalid_ = !bytes;
            p.volumeSize = bytes.value_or(0);
        }
        else p.volumeSize = static_cast<uint64_t>(std::max<int64_t>(0, vol));
        p.threads = static_cast<unsigned>(std::max<int64_t>(0, ComboValue(CThreadsCombo(), 0)));
        p.pathMode = ComboValue(CPathModeCombo(), 0) == 1 ? lycan::PathMode::Full : lycan::PathMode::Relative;
        p.customParams = TrimW(std::wstring{CParamsBox().Text()});
        NormalizePreset(p);
        return p;
    }

    void MainWindow::LoadPresetToUi(lycan::CompressPreset const& p)
    {
        cLoading_ = true;
        CFormatCombo().SelectedIndex(IndexOf(kFormatOrder, p.type));
        CProfileCombo().SelectedIndex(IndexOf(kProfileOrder, p.profile));
        RebuildMethodItems(p.type);
        for (uint32_t i = 0; i < CMethodCombo().Items().Size(); ++i)
        {
            auto it = CMethodCombo().Items().GetAt(i).try_as<ComboBoxItem>();
            if (it && _wcsicmp(unbox_value_or<hstring>(it.Tag(), L"").c_str(), p.method.c_str()) == 0) { CMethodCombo().SelectedIndex(static_cast<int32_t>(i)); break; }
        }
        RebuildWordItems(_wcsicmp(EffectiveMethod(p).c_str(), L"PPMd") == 0);
        SelectValue(CLevelCombo(), p.level, lycan::Tf(L"main.level", p.level));
        SelectValue(CDictCombo(), static_cast<int64_t>(p.dictSize), FormatSize(p.dictSize));
        SelectValue(CWordCombo(), p.wordSize, std::to_wstring(p.wordSize));
        SelectValue(CSolidCombo(), p.solidSize, FormatSize(static_cast<uint64_t>(p.solidSize)));
        SelectValue(CThreadsCombo(), p.threads, std::to_wstring(p.threads));
        CZipEncCombo().SelectedIndex(p.zipEncryption == L"AES256" ? 1 : p.zipEncryption == L"ZipCrypto" ? 2 : 0);
        CEncNames().IsOn(p.encryptFileNames);
        CStoreCompressed().IsOn(p.storeCompressed);
        {
            bool found = false;
            for (uint32_t i = 0; i < CVolCombo().Items().Size() && !found; ++i)
            {
                auto it = CVolCombo().Items().GetAt(i).try_as<ComboBoxItem>();
                if (it && unbox_value_or<int64_t>(it.Tag(), INT64_MIN) == static_cast<int64_t>(p.volumeSize)) { CVolCombo().SelectedIndex(static_cast<int32_t>(i)); found = true; }
            }
            if (!found)  // the preset stores a size that is not in the list: select "Custom..." and fill it in
            {
                SelectValue(CVolCombo(), kVolCustom, lycan::Tr(L"main.custom"));
                CVolCustom().Text(hstring{FormatSizeInput(p.volumeSize)});
            }
        }
        CPathModeCombo().SelectedIndex(p.pathMode == lycan::PathMode::Full ? 1 : 0);
        CParamsBox().Text(hstring{p.customParams});
        CTestAfter().IsOn(settings_.testAfterCompress);
        CSumsArchive().IsOn(p.sumsArchive);
        CSumsFiles().IsOn(p.sumsFiles);
        cLoading_ = false;
        cBaseNorm_ = ReadCompressUi();
        RefreshCompressUi();
    }

    void MainWindow::FillPresetCombo(std::wstring const& select)
    {
        const bool was = cLoading_;
        cLoading_ = true;
        CPresetCombo().Items().Clear();
        int sel = 0, i = 0;
        for (auto const* list : {&cBuiltin_, &cUser_})
            for (auto const& p : *list)
            {
                // The names of built-in presets are data (Tag identifies them), only the displayed text is translated; a name the user chose is shown as it is
                CPresetCombo().Items().Append(MakeItem(hstring{list == &cBuiltin_ ? std::wstring(lycan::Tr(lycan::BuiltinPresetNameId(p.name))) : p.name}, box_value(hstring{p.name})));
                if (p.name == select) sel = i;
                ++i;
            }
        CPresetCombo().SelectedIndex(sel);
        cLoading_ = was;
    }

    // Control state, output file name, summary and the "modified" hint; called whenever any setting changes
    void MainWindow::RefreshCompressUi()
    {
        if (cLoading_ || !cPageActive_) return;
        auto p = ReadCompressUi();
        const bool zip = p.type == lycan::ArchiveType::Zip, sz = p.type == lycan::ArchiveType::SevenZip, arch = zip || sz;
        const bool iso = p.type == lycan::ArchiveType::Iso;
        const auto method = EffectiveMethod(p);
        // an ISO image holds the files as they are: nothing of the compression is used
        CIsoCard().Visibility(iso ? Visibility::Visible : Visibility::Collapsed);
        for (auto c : {CProfileCombo(), CLevelCombo(), CThreadsCombo(), CPathModeCombo()}) c.IsEnabled(!iso);
        CParamsBox().IsEnabled(!iso);
        CPwText().IsEnabled(arch);
        CPwBox().IsEnabled(arch);
        CPwConfirm().IsEnabled(arch);
        CShowPw().IsEnabled(arch);
        CVolCombo().IsEnabled(arch);
        const bool customVol = ComboValue(CVolCombo(), 0) == kVolCustom;
        CVolCustom().Visibility(customVol ? Visibility::Visible : Visibility::Collapsed);
        CVolCustom().IsEnabled(arch);
        CMethodCombo().IsEnabled(arch);
        CDictCombo().IsEnabled(arch && lycan::MethodHasDict(method));
        CWordCombo().IsEnabled(arch && lycan::MethodHasWordSize(method));
        CSolidCombo().IsEnabled(sz);
        CZipEncCombo().IsEnabled(zip);
        CEncNames().IsEnabled(sz);
        CStoreCompressed().IsEnabled(arch);
        if (!arch && !CompressPassword().empty()) SetCompressPassword(L"");

        const bool showPw = CShowPw().IsChecked().Value();
        CPwText().Visibility(showPw ? Visibility::Visible : Visibility::Collapsed);
        CPwBox().Visibility(showPw ? Visibility::Collapsed : Visibility::Visible);
        const std::wstring pw = CompressPassword();
        const bool hasPw = !pw.empty() && arch;
        // when the password is visible there is no need to type it again; when hidden, ask for confirmation
        CPwConfirm().Visibility(hasPw && !showPw ? Visibility::Visible : Visibility::Collapsed);

        // Reasons that block the start (shown in the password area or the status bar)
        std::wstring pwNote, blocker, warning;
        if (hasPw && !showPw && std::wstring{CPwConfirm().Password()} != pw) { pwNote = lycan::Tr(L"main.two_passwords_do_not_match"); blocker = pwNote; }
        else if (zip && std::any_of(pw.begin(), pw.end(), [](wchar_t c) { return c > 0x7F; }))
        { pwNote = lycan::Tr(L"main.zip_passwords_can_only_use_letters"); blocker = pwNote; }
        else if (hasPw && zip && (p.zipEncryption == L"ZipCrypto" || (p.zipEncryption.empty() && p.profile == lycan::Profile::Compatible)))
            pwNote = lycan::Tr(L"main.zipcrypto_weakly_encrypted");
        else if (iso) pwNote = lycan::Tr(L"main.iso_images_not_compressed_do_not");
        else if (!arch) pwNote = lycan::Tr(L"main.tar_family_does_not_support");
        else if (sz && p.encryptFileNames && pw.empty()) pwNote = lycan::Tr(L"main.no_password_entered_yet_so_file");
        CPwNote().Text(hstring{pwNote});
        CPwNote().Visibility(pwNote.empty() ? Visibility::Collapsed : Visibility::Visible);

        // Output
        std::wstring name = TrimW(std::wstring{CNameBox().Text()});
        const bool badName = name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || name.back() == L'.';
        const int fi = IndexOf(kFormatOrder, p.type);
        COutDirText().Text(hstring{lycan::Tf(L"main.save", cOutDir_.wstring())});
        COutNameText().Text(hstring{badName ? std::wstring{lycan::Tr(L"main.invalid_file_name_chars")}
                                            : lycan::Tf(L"main.output_file", name + kFormatExt[fi]) + (p.volumeSize ? lycan::Tr(L"main.split_into_001_002") : L"")});
        if (badName && blocker.empty()) blocker = lycan::Tr(L"main.invalid_file_name");
        if (cSources_.empty() && blocker.empty()) blocker = lycan::Tr(L"main.please_add_files_folders_compress");
        if (arch && customVol && cVolInvalid_ && blocker.empty()) blocker = lycan::Tr(L"main.invalid_split_size_enter_size_like");

        // Preset state
        const bool modified = !(p.SameSettings(cBaseNorm_));
        CPresetModified().Visibility(modified ? Visibility::Visible : Visibility::Collapsed);
        cMenuUpdate_.IsEnabled(!cBase_.builtin && modified);
        cMenuRename_.IsEnabled(!cBase_.builtin);
        cMenuDelete_.IsEnabled(!cBase_.builtin);
        if (cMenuInMenu_) cMenuInMenu_.IsChecked(PresetInMenu(cBase_.name, cBase_.builtin));

        if (p.customParams.empty() == false) warning = lycan::Tr(L"main.custom_parameters_set_if_they_do");
        std::wstring status = DescribePreset(p, hasPw) + lycan::Tf(L"fmt.n_sources", cSources_.size());
        if (!blocker.empty()) status += L"\n⚠ " + blocker;
        else if (!warning.empty()) status += L"\n" + warning;
        CStatusText().Text(hstring{status});
        CStartButton().IsEnabled(blocker.empty());
    }

    std::wstring MainWindow::DefaultArchiveBaseName() const
    {
        if (cSources_.empty()) return L"Archive";
        std::wstring baseName = cSources_.size() == 1 ? cSources_[0].stem().wstring() : cSources_[0].parent_path().filename().wstring();
        std::error_code ec;
        if (cSources_.size() == 1 && fs::is_directory(cSources_[0], ec)) baseName = cSources_[0].filename().wstring();
        return baseName.empty() ? L"Archive" : baseName;
    }

    void MainWindow::RebuildSources()
    {
        auto list = CSourcesList();
        list.Children().Clear();
        CSourcesTitle().Text(hstring{lycan::Tf(L"main.sources", cSources_.size())});
        constexpr size_t kMaxShown = 100;
        auto weak = get_weak();
        for (size_t i = 0; i < cSources_.size() && i < kMaxShown; ++i)
        {
            const fs::path p = cSources_[i];
            std::error_code ec;
            const bool dir = fs::is_directory(p, ec);
            Grid row;
            row.ColumnSpacing(8);
            ColumnDefinition c0, c1, c2;
            c0.Width(GridLength{0, GridUnitType::Auto});
            c1.Width(GridLength{1, GridUnitType::Star});
            c2.Width(GridLength{0, GridUnitType::Auto});
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            row.ColumnDefinitions().Append(c2);
            auto glyph = MakeGlyph(dir ? L"" : L"", 18);
            row.Children().Append(glyph);
            StackPanel names;
            TextBlock n;
            n.Text(hstring{p.filename().empty() ? p.wstring() : p.filename().wstring()});
            n.TextTrimming(TextTrimming::CharacterEllipsis);
            names.Children().Append(n);
            auto sub = Caption(p.parent_path().wstring(), 0.55);
            sub.TextTrimming(TextTrimming::CharacterEllipsis);
            sub.TextWrapping(TextWrapping::NoWrap);
            names.Children().Append(sub);
            Grid::SetColumn(names, 1);
            row.Children().Append(names);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(row, box_value(hstring{p.wstring()}));
            Button rm;
            rm.Content(MakeGlyph(L"", 11));
            rm.Padding(ThicknessHelper::FromUniformLength(6));
            rm.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
            rm.BorderThickness(ThicknessHelper::FromUniformLength(0));
            Automation::AutomationProperties::SetName(rm, lycan::Tr(L"main.remove"));
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(rm, box_value(lycan::Tr(L"main.remove")));
            rm.Click([weak, p](auto&&, auto&&) {
                auto s = weak.get();
                if (!s) return;
                s->cSources_.erase(std::remove(s->cSources_.begin(), s->cSources_.end(), p), s->cSources_.end());
                s->OnSourcesChanged();
            });
            Grid::SetColumn(rm, 2);
            row.Children().Append(rm);
            list.Children().Append(row);
        }
        if (cSources_.size() > kMaxShown) list.Children().Append(Caption(lycan::Tf(L"main.and_n_more_sources", cSources_.size() - kMaxShown), 0.65));
        CSourcesInfo().Text(lycan::Tr(L"main.calculating"));
        ScanSources(cSources_, CSourcesInfo(), dispatcher_);
    }

    void MainWindow::OnSourcesChanged()
    {
        RebuildSources();
        if (!cNameEdited_)
        {
            cLoading_ = true;
            CNameBox().Text(hstring{DefaultArchiveBaseName()});
            cLoading_ = false;
        }
        RefreshCompressUi();
    }

    void MainWindow::UpdateCompressLayout()
    {
        const bool wide = RootGrid().ActualWidth() >= 900;
        auto card = CSourcesCard();
        CSourcesNarrowHost().Visibility(wide ? Visibility::Collapsed : Visibility::Visible);  // an empty container still takes one gap
        auto from = wide ? CSourcesNarrowHost() : CSide();
        auto to = wide ? CSide() : CSourcesNarrowHost();
        uint32_t idx;
        if (from.Children().IndexOf(card, idx))
        {
            from.Children().RemoveAt(idx);
            to.Children().Append(card);
        }
        CSideCol().Width(wide ? GridLength{360, GridUnitType::Pixel} : GridLength{0, GridUnitType::Pixel});
    }

    fire_and_forget MainWindow::AddSourcesFromPicker(bool folder)
    {
        auto strong = get_strong();
        std::vector<fs::path> add;
        if (folder)
        {
            const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeSourceFolder, {}, lycan::Tr(L"main.choose_folder_add")))};
            if (!dir.empty()) add.push_back(dir);
        }
        else
        {
            auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeSourceFiles, true, false);
            for (auto const& f : files) add.emplace_back(std::wstring(f));
        }
        for (auto& p : add)
            if (std::find(cSources_.begin(), cSources_.end(), p) == cSources_.end()) cSources_.push_back(std::move(p));
        OnSourcesChanged();
    }

    // Ctrl+V: files and folders copied in Explorer go into the sources of the new-archive page, or into the archive that is
    // shown (as a drop would). While a text box has the focus the paste is the text box's own, never taken over.
    bool MainWindow::TryPasteFiles()
    {
        using Windows::ApplicationModel::DataTransfer::StandardDataFormats;
        const bool compress = view_ == View::Compress;
        const bool archive = view_ == View::Archive && current_ && tree_ && !compact_ && current_->Caps().canModify;
        if (dialogOpen_ || (!compress && !archive) || FocusInTextInput()) return false;
        try
        {
            auto view = Windows::ApplicationModel::DataTransfer::Clipboard::GetContent();
            if (!view.Contains(StandardDataFormats::StorageItems())) return false;
        }
        catch (...) { return false; }
        PasteFilesAsync();
        return true;
    }

    bool MainWindow::ClipboardHasFiles()
    {
        try { return Windows::ApplicationModel::DataTransfer::Clipboard::GetContent().Contains(Windows::ApplicationModel::DataTransfer::StandardDataFormats::StorageItems()); }
        catch (...) { return false; }
    }

    fire_and_forget MainWindow::PasteFilesAsync(std::wstring into, bool targeted)
    {
        auto strong = get_strong();
        std::vector<fs::path> paths;
        try
        {
            auto items = co_await Windows::ApplicationModel::DataTransfer::Clipboard::GetContent().GetStorageItemsAsync();
            for (auto const& it : items)
                if (!it.Path().empty()) paths.emplace_back(std::wstring(it.Path()));
        }
        catch (...) {}
        if (paths.empty()) co_return;
        PlaySfx(L"drop");
        if (view_ == View::Compress)
        {
            for (auto& p : paths)
                if (std::find(cSources_.begin(), cSources_.end(), p) == cSources_.end()) cSources_.push_back(std::move(p));
            OnSourcesChanged();
        }
        else if (view_ == View::Archive && current_ && tree_ && !compact_ && current_->Caps().canModify)
            AddPathsToArchive(std::move(paths), targeted ? into : CurrentFolderPath());
    }

    // Entered from the right click "Add to archive", drag and drop, and "New archive" on the start page; when the page is open already new sources are merged into the list
    void MainWindow::StartCompress(std::vector<fs::path> inputs)
    {
        if (inputs.empty()) return;
        if (cPageActive_)
        {
            for (auto& p : inputs)
                if (std::find(cSources_.begin(), cSources_.end(), p) == cSources_.end()) cSources_.push_back(std::move(p));
            if (view_ != View::Compress) ShowView(View::Compress);
            OnSourcesChanged();
            return;
        }
        const bool wasCompact = compact_;
        ExitCompact();
        cFromCompact_ = cFromCompact_ || wasCompact;
        cReturnView_ = view_ == View::Settings ? viewBeforeSettings_ : view_;
        if (cReturnView_ == View::Compress) cReturnView_ = View::Home;

        cSources_ = std::move(inputs);
        cOutDir_ = cSources_[0].parent_path();
        cNameEdited_ = false;
        cBuiltin_ = lycan::BuiltinPresets();
        cUser_ = lycan::LoadPresets(lycan::DefaultPresetsPath());

        std::wstring want = settings_.lastPreset;
        auto exists = [&](std::wstring const& n) {
            for (auto const* l : {&cBuiltin_, &cUser_}) for (auto const& p : *l) if (p.name == n) return true;
            return false;
        };
        if (want.empty() || !exists(want)) want = settings_.defaultFormat == lycan::ArchiveType::SevenZip ? L"General 7z" : L"General ZIP";
        for (auto const* l : {&cBuiltin_, &cUser_}) for (auto const& p : *l) if (p.name == want) cBase_ = p;

        SetCompressPassword(L"");
        CShowPw().IsChecked(true);  // show the password by default: a hidden password box cannot type Chinese and other characters
        cPageActive_ = true;
        FillPresetCombo(want);
        LoadPresetToUi(cBase_);
        cLoading_ = true;
        CAdvanced().IsExpanded(settings_.compressAdvancedOpen);
        CNameBox().Text(hstring{DefaultArchiveBaseName()});
        cLoading_ = false;
        RebuildSources();
        ShowView(View::Compress);
        UpdateCompressLayout();
        RefreshCompressUi();
    }

    void MainWindow::LeaveCompressPage()
    {
        cPageActive_ = false;
        SetCompressPassword(L"");
        if (cFromCompact_)
        {
            cFromCompact_ = false;
            ShowView(cReturnView_ == View::Archive && !current_ ? View::Home : cReturnView_);
            EnterCompact();
            CheckCompactAutoClose();
            return;
        }
        ShowView(cReturnView_ == View::Archive && !current_ ? View::Home : cReturnView_);
    }

    void MainWindow::OnCompressStart() { StartCompressAsync(); }

    fire_and_forget MainWindow::StartCompressAsync()
    {
        auto strong = get_strong();
        if (cSources_.empty() || !CStartButton().IsEnabled()) co_return;
        auto p = ReadCompressUi();
        std::wstring name = TrimW(std::wstring{CNameBox().Text()});
        const int fi = IndexOf(kFormatOrder, p.type);
        fs::path output = cOutDir_ / (name + kFormatExt[fi]);

        lycan::CompressOptions co;
        co.tempDir = lycan_app::ScratchOverride();
        lycan::ApplyPreset(p, co);
        co.skipThumbsDb = settings_.skipThumbsDb;
        co.password = CompressPassword();
        if (p.type != lycan::ArchiveType::Zip && p.type != lycan::ArchiveType::SevenZip) co.password.clear();
        co.encryptFileNames = p.encryptFileNames && !co.password.empty();
        co.isoLabel = TrimW(std::wstring{CIsoLabel().Text()});
        co.isoKeepTimes = CIsoKeepTimes().IsOn();

        // the file exists already: a new archive under another name, replace it, or add to it (the update modes)
        {
            std::error_code ec;
            if (fs::exists(output, ec))
            {
                const bool canUpdate = p.type == lycan::ArchiveType::Zip || p.type == lycan::ArchiveType::SevenZip || p.type == lycan::ArchiveType::Tar;
                const std::vector<fs::path> sources = cSources_;
                const int32_t choice = co_await ChooseExistingOutputAsync(output, canUpdate);
                if (choice == 0) co_return;
                if (choice == 1) output = UniquePath(output);
                else if (choice == 2) co.overwrite = true;
                else
                {
                    lycan::UpdateOptions uo;
                    uo.compress = co;
                    uo.mode = static_cast<lycan::UpdateMode>(choice - 3);
                    uo.keepBackup = settings_.keepBackupOnModify;
                    std::vector<lycan::UpdateOp> ops = {lycan::UpdateOp::Add(L"", sources)};
                    if (uo.mode == lycan::UpdateMode::Synchronize && !co_await ConfirmSyncAsync(output, ops, uo)) co_return;
                    {
                        std::lock_guard lk(metaM_);
                        queue_.EnqueueUpdate(output, std::move(ops), uo);
                    }
                    settings_.lastPreset = cBase_.name;
                    SaveSettings();
                    LeaveCompressPage();
                    co_return;
                }
            }
        }

        {
            std::lock_guard lk(metaM_);
            uint64_t id = queue_.EnqueueCompress(output, cSources_, co);
            CompressMeta m;
            m.password = co.password;
            m.sources = cSources_;
            m.testAfter = CTestAfter().IsOn();
            m.sumsArchive = CSumsArchive().IsOn();
            m.sumsFiles = CSumsFiles().IsOn();
            compressMeta_[id] = std::move(m);
        }
        settings_.testAfterCompress = CTestAfter().IsOn();
        settings_.lastPreset = cBase_.name;
        SaveSettings();
        LeaveCompressPage();
    }

    // After a successful compression with nothing skipped, move the sources to the recycle bin (when the output file is inside a source folder, that source is not touched)
    void MainWindow::FinishCompressTask(lycan::TaskInfo const& info)
    {
        if (info.state == lycan::TaskState::Completed && info.compress.ok() && settings_.revealAfterCompress && !info.compress.outputs.empty())
        {
            // show the new file selected in Explorer
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(info.compress.outputs.front().c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        }
        if (info.state != lycan::TaskState::Completed || !info.compress.ok() || info.compress.outputs.empty()) return;
        auto row = taskById_.find(info.id);
        auto self = row != taskById_.end() ? get_self<implementation::TaskRow>(row->second) : nullptr;
        bool haveSources = false, testAfter = false, sumsArchive = false, sumsFiles = false;
        std::vector<fs::path> sumSources;
        {
            std::lock_guard lk(metaM_);
            auto it = compressMeta_.find(info.id);
            if (it != compressMeta_.end())
            {
                it->second.output = info.compress.outputs.front();
                haveSources = !it->second.sources.empty();
                testAfter = it->second.testAfter;
                sumsArchive = it->second.sumsArchive;
                sumsFiles = it->second.sumsFiles;
                if (sumsArchive || sumsFiles) sumSources = it->second.sources;
            }
        }
        if (self)
        {
            self->SetTestable(true);
            // nothing is offered for deleting when something was skipped: the archive is not a complete copy
            const bool complete = info.compress.skipped.empty();
            self->SetDeletable(haveSources && complete, hstring{lycan::Tr(L"main.delete_originals")});
            if (haveSources && !complete) self->SetNote(lycan::Tr(L"main.some_items_were_skipped_so"));
        }
        if (testAfter) StartFollowupTest(info.id);
        if ((sumsArchive || sumsFiles) && !sumSources.empty())
            WriteSumsAfterCompressAsync(info.id, info.compress.outputs, std::move(sumSources), sumsArchive, sumsFiles);
    }

    // The checksum files next to a new archive (settings "write a checksum file ..."): read after the archive is complete, on a thread of their own
    fire_and_forget MainWindow::WriteSumsAfterCompressAsync(uint64_t cardId, std::vector<fs::path> outputs, std::vector<fs::path> sources, bool archiveSum, bool filesSum)
    {
        auto strong = get_strong();
        OpGuard guard{this};   // the compact window stays until the files are written
        lycan::SumWriteResult res;
        bool failed = false;
        co_await resume_background();
        try { res = lycan::WriteArchiveSums(outputs.front(), sources, archiveSum, filesSum, {}, outputs); }
        catch (...) { failed = true; }
        co_await resume_ui{dispatcher_};
        auto row = taskById_.find(cardId);
        if (row == taskById_.end()) co_return;
        auto self = get_self<implementation::TaskRow>(row->second);
        std::wstring note;
        if (failed) note = lycan::Tr(L"sums.card_note_failed");
        else
        {
            std::wstring names;
            for (auto const& w : res.written) names += (names.empty() ? L"" : L", ") + w.filename().wstring();
            if (!names.empty()) note = lycan::Tf(L"sums.card_note_written", names);
        }
        if (!note.empty()) self->SetNote(hstring{note});
    }

    // The integrity test of a finished compression runs as its own queue task, but is shown inside the card of the
    // compression: the card shows the test's progress and returns to its finished state (with the buttons) afterwards.
    void MainWindow::StartFollowupTest(uint64_t cardId)
    {
        auto row = taskById_.find(cardId);
        if (row == taskById_.end() || testOf_.count(cardId)) return;
        fs::path output;
        std::wstring password;
        {
            std::lock_guard lk(metaM_);
            auto it = compressMeta_.find(cardId);
            if (it == compressMeta_.end() || it->second.output.empty()) return;
            output = it->second.output;
            password = it->second.password;
        }
        lycan::OpenOptions oo;
        oo.tempDir = lycan_app::ScratchOverride();
        if (!password.empty()) oo.password = [password](std::wstring& out) { out = password; return true; };
        uint64_t testId;
        {
            std::lock_guard lk(metaM_);
            testId = queue_.EnqueueTest(output, oo);
            testFor_[testId] = cardId;
            testOf_[cardId] = testId;
        }
        get_self<implementation::TaskRow>(row->second)->BeginTest();
    }

    void MainWindow::OnFollowupTestUpdate(uint64_t cardId, lycan::TaskInfo const& info)
    {
        auto it = taskById_.find(cardId);
        if (it == taskById_.end()) { testFor_.erase(info.id); testOf_.erase(cardId); return; }
        auto* row = get_self<implementation::TaskRow>(it->second);
        if (!info.Finished())
        {
            row->Apply(info);
            NotifyStateChanged(false);
            UpdateTaskbar(nullptr);
            return;
        }
        testFor_.erase(info.id);
        testOf_.erase(cardId);
        std::wstring note;
        int state = 0;
        if (info.state == lycan::TaskState::Completed)
        {
            state = 1;
            note = lycan::Tr(L"main.integrity_test_passed");
            if (!info.extract.warnings.empty()) note += lycan::Tf(L"main.data_after_end_compressed_stream", info.extract.warnings.size());
        }
        else if (info.state == lycan::TaskState::Failed)
        {
            state = 2;
            note = lycan::Tf(L"main.integrity_test_failed", (info.extract.errors.empty() ? info.error : info.extract.errors.front()).Text());
        }
        else note = lycan::Tr(L"main.integrity_test_cancelled");
        row->EndTest(hstring{note});
        if (state)
        {
            std::lock_guard lk(metaM_);
            auto m = compressMeta_.find(cardId);
            if (m != compressMeta_.end()) m->second.testState = state;
        }
        if (state == 2) PlaySfx(L"error"); else if (state == 1) PlaySfx(L"done");
        UpdateTaskbar(&info);
        NotifyStateChanged(true);
        UpdateTaskSummary();
        CheckCompactAutoClose();
    }

    static uint64_t TagId(Windows::Foundation::IInspectable const& sender);
    void MainWindow::OnTaskTestClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { StartFollowupTest(TagId(s)); }
    void MainWindow::OnTaskDeleteClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { DeleteOriginalsAsync(TagId(s)); }

    // "Delete the originals" (after a compression) / "delete the archive" (after an extraction): always an explicit
    // click and a warning that says what will happen; the deletion itself is done by the shell.
    fire_and_forget MainWindow::DeleteOriginalsAsync(uint64_t cardId)
    {
        auto strong = get_strong();
        std::vector<fs::path> items;
        fs::path output;
        int testState = 0;
        bool isCompress = false;
        {
            std::lock_guard lk(metaM_);
            auto c = compressMeta_.find(cardId);
            if (c != compressMeta_.end())
            {
                items = c->second.sources;
                output = c->second.output;
                testState = c->second.testState;
                isCompress = true;
            }
            else
            {
                auto m = taskMeta_.find(cardId);
                if (m != taskMeta_.end() && m->second.deletable) items = {m->second.archive};
            }
        }
        // only what still exists; a source that contains the new archive stays (it is not a separate original)
        std::vector<fs::path> todo;
        bool keptOne = false;
        for (auto const& p : items)
        {
            std::error_code ec;
            if (!fs::exists(p, ec)) continue;
            if (isCompress && !output.empty())
            {
                auto rel = output.lexically_relative(p);
                if (!rel.empty() && *rel.begin() != L"..") { keptOne = true; continue; }
            }
            todo.push_back(p);
        }
        auto rowIt = taskById_.find(cardId);
        auto* row = rowIt != taskById_.end() ? get_self<implementation::TaskRow>(rowIt->second) : nullptr;
        if (todo.empty())
        {
            ShowMessage(lycan::Tr(L"main.nothing_delete"), lycan::Tr(L"main.originals_no_longer_exist_they"));
            if (row) row->SetDeletable(false, {});
            co_return;
        }
        if (isCompress)
        {
            std::error_code ec;
            if (output.empty() || !fs::exists(output, ec) || fs::file_size(output, ec) == 0)
            {
                ShowMessage(lycan::Tr(L"main.new_archive_was_not_found"), lycan::Tr(L"main.new_archive_gone_so_originals_will"));
                co_return;
            }
        }
        bool recycle = true;
        for (auto const& p : todo) recycle = recycle && CanRecycle(p);

        auto dlg = MakeDialog(isCompress ? lycan::Tr(L"main.delete_originals_title") : lycan::Tr(L"main.delete_original_archive_title"), L"\uE74D");
        StackPanel sp;
        sp.Spacing(10);
        sp.MaxWidth(440);
        auto add = [&](std::wstring const& text, bool strong_) {
            TextBlock t;
            t.Text(hstring{text});
            t.TextWrapping(TextWrapping::Wrap);
            if (strong_) t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            sp.Children().Append(t);
        };
        if (recycle)
            add(lycan::Tf(L"main.move_recycle_bin_you_can_restore", todo.size()), false);
        else
            add(lycan::Tf(L"main.drive_cannot_use_recycle_bin", todo.size()), true);
        std::wstring list;
        for (size_t i = 0; i < todo.size() && i < 5; ++i) list += L"•  " + todo[i].filename().wstring() + L"\n";
        if (todo.size() > 5) list += lycan::Tf(L"main.more", todo.size() - 5);
        {
            TextBlock t;
            t.Text(hstring{list});
            t.Opacity(0.8);
            t.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t);
        }
        if (isCompress)
        {
            if (testState == 1) add(lycan::Tr(L"main.new_archive_passed_integrity_test"), false);
            else if (testState == 2) add(lycan::Tr(L"main.integrity_test_new_archive_failed"), true);
            else add(lycan::Tr(L"main.new_archive_has_not_been_tested"), true);
            if (keptOne) add(lycan::Tr(L"main.new_archive_inside_one_source"), false);
        }
        dlg.Content(sp);
        dlg.PrimaryButtonText(recycle ? lycan::Tr(L"main.move_recycle_bin") : lycan::Tr(L"main.delete_permanently"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;

        int deleted = 0;
        const bool shown = current_ && !todo.empty() && currentPath_ == todo.front();
        if (shown) UnlockArchiveFile();
        const int failed = ShellDelete(Hwnd(), todo, recycle, deleted);
        if (shown && failed != 0 && current_) LockArchiveFile(currentPath_);
        if (row)
        {
            if (failed == 0)
            {
                row->SetNote(hstring{recycle ? lycan::Tf(L"main.moved_recycle_bin", deleted) : lycan::Tf(L"main.status_deleted_items", deleted)});
                row->SetDeletable(false, {});
            }
            else
                row->SetNote(hstring{lycan::Tf(L"main.not_deleted", failed)});
        }
        if (!isCompress)
        {
            std::lock_guard lk(metaM_);
            auto m = taskMeta_.find(cardId);
            if (m != taskMeta_.end() && failed == 0) m->second.deletable = false;
        }
        if (!isCompress && failed == 0 && current_ && !todo.empty() && currentPath_ == todo.front()) ShowHome();
    }

    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskNameAsync(hstring title, hstring initial)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(title, L"");
        TextBox box;
        box.Text(initial);
        box.MaxLength(40);
        box.SelectAll();
        Automation::AutomationProperties::SetName(box, lycan::Tr(L"main.template_name"));
        StackPanel sp;
        sp.Spacing(8);
        sp.MinWidth(360);
        sp.Children().Append(box);
        sp.Children().Append(Caption(lycan::Tr(L"main.up_40_characters_no_allowed"), 0.65));
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        auto r = co_await ShowDialogAsync(dlg);
        co_return r == ContentDialogResult::Primary ? hstring{TrimW(std::wstring{box.Text()})} : hstring{};
    }

    bool MainWindow::PresetNameTaken(std::wstring const& name) const
    {
        for (auto const* l : {&cBuiltin_, &cUser_}) for (auto const& p : *l) if (p.name == name) return true;
        return false;
    }

    fire_and_forget MainWindow::OnPresetSaveNew()
    {
        auto strong = get_strong();
        std::wstring name{co_await AskNameAsync(lycan::Tr(L"main.save_as_new_template"), lycan::Tr(L"main.my_template"))};
        if (name.empty()) co_return;
        if (!lycan::IsValidPresetName(name)) { ShowMessage(lycan::Tr(L"main.invalid_name"), lycan::Tr(L"main.name_cannot_be_empty_contain_line")); co_return; }
        if (PresetNameTaken(name)) { ShowMessage(lycan::Tr(L"main.name_already_exists"), lycan::Tr(L"main.template_with_name_already_exists")); co_return; }
        auto cur = ReadCompressUi();
        cur.name = name;
        cUser_.push_back(cur);
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_ = cur;
        FillPresetCombo(name);
        cBaseNorm_ = cur;
        RefreshCompressUi();
    }

    void MainWindow::OnPresetUpdate()
    {
        if (cBase_.builtin) return;
        auto cur = ReadCompressUi();
        cur.name = cBase_.name;
        cur.inMenu = cBase_.inMenu;  // not part of what the page shows
        for (auto& u : cUser_) if (u.name == cur.name) u = cur;
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_ = cur;
        cBaseNorm_ = cur;
        RefreshCompressUi();
    }

    fire_and_forget MainWindow::OnPresetRename()
    {
        auto strong = get_strong();
        if (cBase_.builtin) co_return;
        std::wstring oldName = cBase_.name;
        std::wstring name{co_await AskNameAsync(lycan::Tr(L"main.rename_template"), hstring{oldName})};
        if (name.empty() || name == oldName) co_return;
        if (!lycan::IsValidPresetName(name)) { ShowMessage(lycan::Tr(L"main.invalid_name"), lycan::Tr(L"main.name_cannot_be_empty_contain_line")); co_return; }
        if (PresetNameTaken(name)) { ShowMessage(lycan::Tr(L"main.name_already_exists"), lycan::Tr(L"main.template_with_name_already_exists")); co_return; }
        for (auto& u : cUser_) if (u.name == oldName) u.name = name;
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_.name = name;
        FillPresetCombo(name);
        RefreshCompressUi();
    }

    fire_and_forget MainWindow::OnPresetDelete()
    {
        auto strong = get_strong();
        if (cBase_.builtin) co_return;
        auto dlg = MakeDialog(lycan::Tr(L"main.delete_template_title"), L"");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.delete_template_cannot_be_undone", cBase_.name)});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.delete"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        const std::wstring gone = cBase_.name;
        cUser_.erase(std::remove_if(cUser_.begin(), cUser_.end(), [&](auto const& u) { return u.name == gone; }), cUser_.end());
        lycan::SavePresets(lycan::DefaultPresetsPath(), cUser_);
        cBase_ = cBuiltin_.front();
        FillPresetCombo(cBase_.name);
        LoadPresetToUi(cBase_);
    }

    // ---------------------------------------------------------------- Task panel

    void MainWindow::OnTaskUpdate(lycan::TaskInfo const& info)
    {
        if (info.kind == lycan::TaskKind::Hash)  // the dialog shows it (no card, no notification)
        {
            auto h = hashHandlers_.find(info.id);
            if (h != hashHandlers_.end())
            {
                auto fn = h->second;  // it may restart the task and so change the map
                fn(info);
            }
            return;
        }
        if (info.kind == lycan::TaskKind::Test)
        {
            auto ft = testFor_.find(info.id);
            if (ft != testFor_.end())
            {
                OnFollowupTestUpdate(ft->second, info);  // shown inside the card of the finished compression
                return;
            }
        }
        LycanZip::TaskRow row{nullptr};
        auto it = taskById_.find(info.id);
        if (it == taskById_.end())
        {
            row = make<implementation::TaskRow>(info.id);
            taskById_.insert_or_assign(info.id, row);
            taskRows_.InsertAt(0, row);  // newest on top
            PlaySfx(L"start");
        }
        else row = it->second;
        get_self<implementation::TaskRow>(row)->Apply(info);
        NotifyStateChanged(info.Finished() || info.state == lycan::TaskState::Queued);  // new task / finished: update at once; progress only once per second
        const bool panelWasHidden = TaskPanel().Visibility() != Visibility::Visible;
        TaskPanel().Visibility(Visibility::Visible);
        if (panelWasHidden) FadeIn(TaskPanel(), 16, 240);  // the task card enters
        if (info.Finished() && fxDoneTasks_.insert(info.id).second)
        {
            if (info.state == lycan::TaskState::Failed) PlaySfx(L"error");
            else if (info.state == lycan::TaskState::Completed) { PlaySfx(L"done"); PulseTaskPanel(); }
            if (info.state == lycan::TaskState::Completed || info.state == lycan::TaskState::Failed) GlowMoon(row, info.state == lycan::TaskState::Failed);
            UpdateTaskbar(&info);
        }
        else UpdateTaskbar(nullptr);
        UpdateTaskSummary();
        if (compact_) UpdateCompactDone();

        if (info.Finished() && info.state == lycan::TaskState::Failed && info.tempSpace) HandleTempSpaceTask(info);  // no room for the scratch file: ask where else
        if (info.Finished() && info.kind == lycan::TaskKind::Extract)
        {
            TaskMeta* meta = nullptr;
            {
                std::lock_guard lk(metaM_);
                auto m = taskMeta_.find(info.id);
                if (m != taskMeta_.end() && !m->second.handled) { m->second.handled = true; meta = &m->second; }
            }
            if (meta) FinishExtractTask(info, *meta);
        }
        if (info.Finished() && info.kind == lycan::TaskKind::Compress) FinishCompressTask(info);
        if (info.Finished() && info.kind == lycan::TaskKind::Scan && scanShown_.insert(info.id).second) FinishScanTask(info);
        if (info.Finished() && info.kind == lycan::TaskKind::Update)
        {
            TaskMeta meta;
            bool have = false;
            {
                std::lock_guard lk(metaM_);
                auto m = taskMeta_.find(info.id);
                if (m != taskMeta_.end() && !m->second.handled) { m->second.handled = true; meta = m->second; have = true; }
            }
            if (have) FinishUpdateTask(info, meta);
        }
        CheckCompactAutoClose();
    }

    // What is done after an extraction task ends: handling a wrong password, opening the folder when done, deleting the original archive
    void MainWindow::FinishExtractTask(lycan::TaskInfo const& info, TaskMeta& meta)
    {
        if (info.state == lycan::TaskState::Failed && info.extract.wrongPassword)
        {
            // password rejected: forget it, the next operation asks again and verifies
            if (current_ && currentPath_ == meta.archive) { password_.clear(); passwordVerified_ = false; }
            ShowMessage(lycan::Tr(L"main.wrong_password"), lycan::Tr(L"main.archive_could_not_be_opened_with"));
            return;
        }
        if (!meta.openFile.empty())
        {
            // temporary extraction done -> open with the default program. If there is no associated program, open the "Choose an app" dialog instead
            if (info.state == lycan::TaskState::Completed && info.extract.ok())
            {
                if (!meta.editEntry.empty()) { StartEditSession(meta); return; }  // "edit": the program is started and the file watched
                if (_wgetenv(L"LYCAN_TEST_NO_OPEN")) return;  // for tests: do not really start the associated program
                // working directory = the file's folder: programs often load their files by relative path
                const std::wstring workDir = meta.openFile.parent_path().wstring();
                SHELLEXECUTEINFOW sei{sizeof(sei)};
                sei.fMask = SEE_MASK_NOCLOSEPROCESS;  // remember the process: its temp folder must outlive it (see OpenTempDir.h)
                sei.lpVerb = nullptr;  // the default verb, as in Explorer (fonts have only "preview", no "open")
                sei.lpFile = meta.openFile.c_str();
                sei.lpDirectory = workDir.c_str();
                sei.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExW(&sei)) lycan_app::TrackLaunch(lycan_app::OpenTempOwnerDir(meta.openFile), sei.hProcess);
                else
                {
                    ShellExecuteW(nullptr, L"openas", meta.openFile.c_str(), nullptr, workDir.c_str(), SW_SHOWNORMAL);
                    lycan_app::TrackLaunch(lycan_app::OpenTempOwnerDir(meta.openFile), nullptr);
                }
            }
            return;
        }
        if (info.state != lycan::TaskState::Completed || !info.extract.ok()) return;

        auto row = taskById_.find(info.id);
        if (settings_.openFolderAfterExtract && !info.extract.outputDir.empty())
            ShellExecuteW(nullptr, L"open", info.extract.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        // The original archive can be deleted with a button on the card (never automatically). Only offered when the
        // whole archive was extracted without skipping or rejecting anything, and not for volumes (one part only).
        if (meta.wholeArchive && info.extract.skipped.empty() && info.extract.rejected.empty() && !IsVolumePath(meta.archive))
        {
            meta.deletable = true;
            if (row != taskById_.end()) get_self<implementation::TaskRow>(row->second)->SetDeletable(true, hstring{lycan::Tr(L"main.delete_original_archive")});
        }
    }

    // Taskbar progress and the notification when done. finished not empty = this update is because a task just ended.
    // The progress is the total of all running tasks; only when the whole batch (all tasks) ends a summary notification is sent, so there is no pop-up for each one.
    void MainWindow::UpdateTaskbar(lycan::TaskInfo const* finished)
    {
        using PS = lycan_app::ProgressState;
        if (finished && finished->state != lycan::TaskState::Cancelled)
        {
            const bool ok = finished->state == lycan::TaskState::Completed &&
                            (finished->kind != lycan::TaskKind::Extract || finished->extract.ok()) &&
                            (finished->kind != lycan::TaskKind::Compress || finished->compress.ok()) &&
                            (finished->kind != lycan::TaskKind::Update || finished->update.ok());
            ++(ok ? batchOk_ : batchBad_);
            batchTitle_ = finished->title;
            batchKind_ = finished->kind;
            batchError_ = ok ? L"" : !finished->error.empty() ? finished->error.Text()
                          : finished->extract.wrongPassword ? lycan::Tr(L"main.wrong_password") : lycan::Tr(L"main.some_items_could_not_be_processed");
        }

        int active = 0, paused = 0;
        unsigned long long done = 0, total = 0;
        for (auto const& t : queue_.Snapshot())
        {
            if (t.Finished()) continue;
            ++active;
            if (t.state == lycan::TaskState::Paused) ++paused;
            if (t.total > 0) { done += t.done; total += t.total; }
        }

        if (active > 0)
        {
            const PS now = paused == active ? PS::Paused : total == 0 ? PS::Indeterminate : PS::Normal;
            if (now != lastTaskbarState_) { lastTaskbarState_ = now; lycan::diag::Info(L"taskbar progress: {} ({} task(s) running)", static_cast<int>(now), active); }
            notifier_.SetProgress(now, done, total);
            SetTitleProgress(total > 0 ? static_cast<int>(std::min<unsigned long long>(done * 100 / total, 100)) : -1);
            return;
        }
        SetTitleProgress(-1);
        lastTaskbarState_ = PS::None;

        if (batchOk_ + batchBad_ == 0)
        {
            if (notifier_.State() != PS::Error) notifier_.SetProgress(PS::None);  // cancelled: clear the progress bar
            return;
        }

        // the whole batch ended
        const int ok = batchOk_, bad = batchBad_;
        const bool single = ok + bad == 1;
        const wchar_t* kind = batchKind_ == lycan::TaskKind::Compress ? lycan::Tr(L"main.compression") : batchKind_ == lycan::TaskKind::Test ? lycan::Tr(L"main.integrity_test") : batchKind_ == lycan::TaskKind::Update ? lycan::Tr(L"main.change_archive") : batchKind_ == lycan::TaskKind::Scan ? lycan::Tr(L"main.scan_malware") : lycan::Tr(L"main.extraction");
        std::wstring title, text;
        if (single)
        {
            title = bad ? lycan::Tf(L"main.op_failed", kind) : lycan::Tf(L"main.complete", kind);
            text = batchTitle_ + (bad ? lycan::Tf(L"fmt.colon_value", batchError_) : std::wstring());
        }
        else
        {
            title = lycan::Tr(L"app.tasks_finished");
            text = lycan::Tf(L"main.succeeded", ok) + (bad ? lycan::Tf(L"main.suffix_n_failed", bad) : std::wstring());
        }
        batchOk_ = batchBad_ = 0;

        notifier_.SetProgress(bad ? PS::Error : PS::None);
        const bool foreground = notifier_.AppIsForeground();
        if (!foreground) notifier_.FlashUntilForeground();
        const bool inBackground = sBackgroundActive && sBackgroundActive();  // the App sends one notification for everything
        if (settings_.notifications && !closing_ && !inBackground && (bad > 0 || !foreground)) notifier_.Notify(title, text, bad > 0);
    }

    // Summary next to the panel title: how many running / all done / with failures
    void MainWindow::UpdateTaskSummary()
    {
        int running = 0, failed = 0, total = 0;
        for (auto const& t : queue_.Snapshot())
        {
            ++total;
            if (!t.Finished()) ++running;
            else if (t.state == lycan::TaskState::Failed) ++failed;
        }
        std::wstring text;
        if (running) text = lycan::Tf(L"main.progress", running);
        else if (failed) text = lycan::Tf(L"main.n_failed", failed);
        else if (total) text = lycan::Tr(L"main.all_done");
        TaskSummary().Text(hstring{text});
        const auto bgVis = running > 0 ? Visibility::Visible : Visibility::Collapsed;  // only while something runs
        BackgroundButton().Visibility(bgVis);
        CompactBackgroundButton().Visibility(bgVis);
    }

    void MainWindow::OnTaskCollapseClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        bool collapse = TaskScroll().Visibility() == Visibility::Visible;
        TaskScroll().Visibility(collapse ? Visibility::Collapsed : Visibility::Visible);
        TaskCollapseGlyph().Glyph(collapse ? L"\uE70E" : L"\uE70D");  // expanded = arrow down; collapsed = arrow up
    }

    void MainWindow::OnClearFinishedClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        for (int i = static_cast<int>(taskRows_.Size()) - 1; i >= 0; --i)
        {
            auto row = taskRows_.GetAt(static_cast<uint32_t>(i)).as<LycanZip::TaskRow>();
            lycan::TaskInfo info;
            if (queue_.Get(row.Id(), info) && info.Finished())
            {
                taskById_.erase(row.Id());
                { std::lock_guard lk(metaM_); taskMeta_.erase(row.Id()); }
                taskRows_.RemoveAt(static_cast<uint32_t>(i));
            }
        }
        if (taskRows_.Size() == 0) TaskPanel().Visibility(Visibility::Collapsed);
        UpdateTaskSummary();
    }

    static uint64_t TagId(Windows::Foundation::IInspectable const& sender)
    {
        return unbox_value<uint64_t>(sender.as<FrameworkElement>().Tag());
    }

    void MainWindow::OnTaskPauseClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { queue_.Pause(ActiveTaskFor(TagId(s))); }
    void MainWindow::OnTaskResumeClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { queue_.Resume(ActiveTaskFor(TagId(s))); }
    void MainWindow::OnTaskCancelClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { queue_.Cancel(ActiveTaskFor(TagId(s))); }

    void MainWindow::OnTaskOpenFolderClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&)
    {
        auto path = unbox_value<hstring>(s.as<FrameworkElement>().Tag());
        if (!path.empty()) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    // ---------------------------------------------------------------- Hash values

    // Which algorithm's checksum file "Save as a checksum file" writes (the strongest of the chosen ones) and its extension
    static uint32_t HashSumAlgo(uint32_t algos)
    {
        for (uint32_t a : {lycan::kHashSha256, lycan::kHashSha512, lycan::kHashSha384, lycan::kHashSha1, lycan::kHashMd5, lycan::kHashCrc32})
            if (algos & a) return a;
        return lycan::kHashSha256;
    }

    static std::wstring HashSumExt(uint32_t algo)
    {
        switch (algo)
        {
        case lycan::kHashSha256: return L"sha256";
        case lycan::kHashSha512: return L"sha512";
        case lycan::kHashSha384: return L"sha384";
        case lycan::kHashSha1: return L"sha1";
        case lycan::kHashMd5: return L"md5";
        default: return L"txt";  // CRC-32: "CRC32 (name) = value" lines are not a format of their own
        }
    }

    // Opens the VirusTotal page of a SHA-256 value in the browser of the user (nothing is uploaded: the address holds the hash and that is all).
    // Tests: LYCAN_TEST_URL_FILE=<file> appends the address to the file instead of opening it.
    static void OpenVirusTotal(std::string const& sha256Hex)
    {
        const std::wstring url = lycan::VirusTotalUrl(sha256Hex);
        if (url.empty()) return;
        if (const wchar_t* f = _wgetenv(L"LYCAN_TEST_URL_FILE"))
        {
            std::ofstream(f, std::ios::app) << lycan::ToUtf8(url) << "\n";
            return;
        }
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // The dialog of one request: the algorithms, the progress, a value per file and algorithm (selectable, with a copy button),
    // a box to compare with a pasted value, and "save as a checksum file" / "copy all".
    struct MainWindow::HashDialogState
    {
        HashRequest req;
        uint32_t algos = lycan::kHashDefault;
        uint64_t id = 0;
        bool running = false, have = false;
        bool vtDone = false;               // "Look up on VirusTotal": the page was opened (only once)
        lycan::HashReport report;
        ContentDialog dlg{nullptr};
        TextBox compare{nullptr};
        TextBlock compareResult{nullptr}, status{nullptr};
        ProgressBar bar{nullptr};
        StackPanel results{nullptr};
        ScrollViewer scroll{nullptr};
        std::vector<std::pair<uint32_t, CheckBox>> boxes;
        // a checksum file next to the file: what it says about this file
        int sideKind = 0;                  // 0 = there is none, 1 = it has a value for the file, 2 = it has none for this file, 3 = it could not be read
        uint32_t sideAlgo = 0, sideForced = 0;
        std::string sideHex;
        TextBlock sideResult{nullptr};
        std::function<void()> start, render, evaluate, checkSide;
    };

    fire_and_forget MainWindow::ShowHashDialogAsync(HashRequest req, bool closeWhenDone)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        const bool inArchive = !req.archive.empty();
        if (inArchive ? req.indices.empty() : req.files.empty()) co_return;
        if (inArchive && current_ && current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;

        using Microsoft::UI::Xaml::Media::SolidColorBrush;
        const Windows::UI::Color green{lycan_app::StatusColor(Windows::UI::Color{255, 0x6C, 0xCB, 0x5F})}, red{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})};
        auto st = std::make_shared<HashDialogState>();
        st->req = std::move(req);
        st->algos = st->req.virusTotal ? lycan::kHashSha256 : lycan::NormalizeHashAlgos(settings_.hashAlgos);   // (a lookup does not change what the user chose for the hash dialog)
        if (!st->req.sidecar.empty() && st->req.files.size() == 1)
        {
            const lycan::SumFile sf = lycan::ReadSumFile(st->req.sidecar);
            if (sf.error != lycan::SumFileError::None) st->sideKind = 3;
            else
            {
                st->sideKind = 2;
                const std::wstring want = fs::path(st->req.files[0]).filename().wstring();
                auto lastPart = [](std::wstring n) {
                    const size_t cut = n.find_last_of(L"/\\");
                    return cut == std::wstring::npos ? n : n.substr(cut + 1);
                };
                const lycan::SumEntry* hit = nullptr;
                for (auto const& e : sf.entries)
                    if (_wcsicmp(lastPart(e.name).c_str(), want.c_str()) == 0) { hit = &e; break; }
                if (!hit && sf.entries.size() == 1) hit = &sf.entries[0];   // a checksum file of one file, named after something else
                if (hit)
                {
                    st->sideKind = 1;
                    st->sideAlgo = hit->algo;
                    st->sideHex = hit->hex;
                    st->sideForced = hit->algo & ~st->algos;
                    st->algos |= hit->algo;   // the value has to be calculated to be compared
                }
            }
        }
        auto dlg = MakeDialog(lycan::Tr(st->req.virusTotal ? L"main.virustotal_lookup" : L"main.hash_values"), L"");
        st->dlg = dlg;

        StackPanel sp;
        sp.Spacing(10);
        sp.MinWidth(680);
        dlg.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(760.0));  // the default (548) cuts the values off
        {
            TextBlock what;
            what.Text(inArchive ? hstring{lycan::Tf(L"main.item_s_archive", st->req.archive.filename().wstring(), st->req.indices.size())}
                      : !st->req.root.empty() ? hstring{lycan::Tf(L"sums.folder_files", st->req.root.filename().wstring(), st->req.files.size())}
                      : st->req.files.size() == 1 ? hstring{st->req.files[0].wstring()}
                                                  : hstring{lycan::Tf(L"fmt.n_files", st->req.files.size())});
            what.TextTrimming(TextTrimming::CharacterEllipsis);
            what.Opacity(0.75);
            sp.Children().Append(what);
        }

        // the algorithms
        {
            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Spacing(2);
            for (uint32_t a : lycan::HashAlgoList(lycan::kHashAll))
            {
                CheckBox b;
                b.Content(box_value(hstring{lycan::HashAlgoName(a)}));
                b.MinWidth(0);
                b.IsChecked((st->algos & a) != 0);
                Automation::AutomationProperties::SetName(b, hstring{lycan::HashAlgoName(a)});
                b.Click([this, st](auto&&, auto&&) {
                    uint32_t mask = 0;
                    for (auto const& [algo, box] : st->boxes) if (box.IsChecked().Value()) mask |= algo;
                    if (mask == 0)  // at least one: keep the first one that was just switched off
                    {
                        mask = st->algos;
                        for (auto const& [algo, box] : st->boxes) box.IsChecked((mask & algo) != 0);
                    }
                    if (mask != st->algos) { st->algos = mask; st->start(); }
                });
                row.Children().Append(b);
                st->boxes.emplace_back(a, b);
            }
            sp.Children().Append(row);
        }

        // progress and the values
        st->bar = ProgressBar{};
        st->bar.Minimum(0);
        st->bar.Maximum(100);
        sp.Children().Append(st->bar);
        st->status = Caption(L"", 0.75);
        sp.Children().Append(st->status);
        st->sideResult = TextBlock{};
        st->sideResult.TextWrapping(TextWrapping::Wrap);
        st->sideResult.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        sp.Children().Append(st->sideResult);
        st->results = StackPanel{};
        st->results.Spacing(12);
        ScrollViewer sv;
        sv.Content(st->results);
        st->scroll = sv;
        sp.Children().Append(MakeCardBox(sv));

        // compare with a value that was pasted
        st->compare = TextBox{};
        st->compare.Header(box_value(lycan::Tr(L"main.paste_hash_value_compare")));
        st->compare.PlaceholderText(lycan::Tr(L"main.example_sha_256_written_on_website"));
        Automation::AutomationProperties::SetName(st->compare, lycan::Tr(L"main.hash_value_compare"));
        st->compare.TextChanged([st](auto&&, auto&&) { if (st->evaluate) st->evaluate(); });
        sp.Children().Append(st->compare);
        st->compareResult = TextBlock{};
        st->compareResult.TextWrapping(TextWrapping::Wrap);
        st->compareResult.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        sp.Children().Append(st->compareResult);
        sp.Children().Append(Caption(lycan::Tr(L"main.match_only_means_file_same_as"), 0.6));
        dlg.Content(sp);

        // the name of a file as it is shown: the filename; a file in an archive keeps its path; the files of a folder that was chosen are shown from that folder
        auto shownName = [st](std::wstring const& path) -> std::wstring {
            if (!st->req.archive.empty()) return path;
            if (!st->req.root.empty())
            {
                std::wstring rel = fs::path(path).lexically_relative(st->req.root.parent_path()).wstring();
                if (!rel.empty() && rel.rfind(L"..", 0) != 0) { std::replace(rel.begin(), rel.end(), L'\\', L'/'); return rel; }
            }
            return fs::path(path).filename().wstring();
        };

        // what the values look like
        st->render = [this, st, red, shownName]() {
            st->results.Children().Clear();
            // a few files show as they are; many scroll (with room at the right for the scroll bar)
            const bool many = st->report.files.size() > 3;
            st->scroll.VerticalScrollBarVisibility(many ? ScrollBarVisibility::Auto : ScrollBarVisibility::Disabled);
            st->scroll.MaxHeight(many ? 240.0 : std::numeric_limits<double>::infinity());
            st->results.Margin(ThicknessHelper::FromLengths(0, 0, many ? 18 : 0, 0));
            const auto list = lycan::HashAlgoList(st->report.algos);
            size_t shown = 0;
            for (auto const& f : st->report.files)
            {
                if (shown++ >= 200) break;
                StackPanel one;
                one.Spacing(3);
                TextBlock name;
                name.Text(hstring{shownName(f.path)});
                name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                name.TextTrimming(TextTrimming::CharacterEllipsis);
                Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(name, box_value(hstring{f.path}));
                one.Children().Append(name);
                if (!f.ok())
                {
                    TextBlock e;
                    e.Text(hstring{f.error.Text()});
                    e.TextWrapping(TextWrapping::Wrap);
                    e.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{red});
                    one.Children().Append(e);
                }
                else
                {
                    one.Children().Append(Caption(f.size < 1024 ? FormatSize(f.size) : lycan::Tf(L"fmt.size_bytes", FormatSize(f.size), f.size), 0.6));
                    for (size_t i = 0; i < list.size() && i < f.hex.size(); ++i)
                    {
                        Grid row;
                        row.ColumnSpacing(8);
                        ColumnDefinition c0, c1, c2;
                        c0.Width(GridLength{76, GridUnitType::Pixel});
                        c1.Width(GridLength{1, GridUnitType::Star});
                        c2.Width(GridLength{0, GridUnitType::Auto});
                        row.ColumnDefinitions().Append(c0);
                        row.ColumnDefinitions().Append(c1);
                        row.ColumnDefinitions().Append(c2);
                        TextBlock lbl;
                        lbl.Text(hstring{lycan::HashAlgoName(list[i])});
                        lbl.Opacity(0.7);
                        lbl.VerticalAlignment(VerticalAlignment::Center);
                        row.Children().Append(lbl);
                        TextBlock val;
                        val.Text(hstring{FromUtf8(f.hex[i])});
                        val.IsTextSelectionEnabled(true);
                        val.TextWrapping(TextWrapping::Wrap);
                        val.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{L"Consolas"});
                        val.VerticalAlignment(VerticalAlignment::Center);
                        Grid::SetColumn(val, 1);
                        row.Children().Append(val);
                        Button copy;
                        copy.Content(MakeGlyph(L"", 12));
                        copy.Padding(ThicknessHelper::FromUniformLength(5));
                        copy.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
                        copy.BorderThickness(ThicknessHelper::FromUniformLength(0));
                        Automation::AutomationProperties::SetName(copy, lycan::Tr(L"preview.copy"));
                        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(copy, box_value(lycan::Tr(L"preview.copy")));
                        copy.Click([hex = f.hex[i]](auto&&, auto&&) {
                            Windows::ApplicationModel::DataTransfer::DataPackage dp;
                            dp.SetText(hstring{FromUtf8(hex)});
                            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
                        });
                        Grid::SetColumn(copy, 2);
                        row.Children().Append(copy);
                        if (list[i] == lycan::kHashSha256)   // look the file up at VirusTotal
                        {
                            ColumnDefinition c3;
                            c3.Width(GridLength{0, GridUnitType::Auto});
                            row.ColumnDefinitions().Append(c3);
                            Button vt;
                            vt.Content(MakeGlyph(L"\uE774", 12));
                            vt.Padding(ThicknessHelper::FromUniformLength(5));
                            vt.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
                            vt.BorderThickness(ThicknessHelper::FromUniformLength(0));
                            Automation::AutomationProperties::SetName(vt, lycan::Tr(L"main.virustotal_lookup"));
                            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(vt, box_value(lycan::Tr(L"main.virustotal_lookup_tip")));
                            vt.Click([hex = f.hex[i]](auto&&, auto&&) { OpenVirusTotal(hex); });
                            Grid::SetColumn(vt, 3);
                            row.Children().Append(vt);
                        }
                        one.Children().Append(row);
                    }
                }
                st->results.Children().Append(one);
            }
            if (st->report.files.size() > 200)
                st->results.Children().Append(Caption(lycan::Tf(L"main.more_file_s_not_listed_save", st->report.files.size() - 200), 0.65));
        };

        // the pasted value against what was computed
        st->evaluate = [st, green, red, shownName]() {
            auto res = st->compareResult;
            res.Text(L"");
            const std::wstring text{st->compare.Text()};
            if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
            auto paint = [&](hstring const& t, Windows::UI::Color c) { res.Text(t); res.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{c}); };
            const std::string want = lycan::NormalizeHashText(text);
            if (want.empty()) { paint(lycan::Tr(L"main.not_hash_value_only_0_9"), red); return; }
            const uint32_t mask = lycan::HashAlgosForLength(want.size());
            if (!mask)
            {
                paint(hstring{lycan::Tf(L"main.length_digits_matches_no_algorithm", want.size())}, red);
                return;
            }
            std::wstring which;
            for (uint32_t a : lycan::HashAlgoList(mask)) which += (which.empty() ? L"" : L" / ") + std::wstring(lycan::HashAlgoName(a));
            if (!st->have) { paint(lycan::Tr(L"main.still_calculating"), lycan_app::StatusColor(Windows::UI::Color{255, 200, 200, 200})); return; }
            const auto list = lycan::HashAlgoList(st->report.algos);
            for (auto const& f : st->report.files)
            {
                if (!f.ok()) continue;
                for (size_t i = 0; i < list.size() && i < f.hex.size(); ++i)
                    if ((list[i] & mask) && f.hex[i] == want)
                    {
                        paint(hstring{lycan::Tf(L"main.match", shownName(f.path), lycan::HashAlgoName(list[i]))}, green);
                        return;
                    }
            }
            if ((mask & st->report.algos) == 0) paint(hstring{lycan::Tf(L"main.has_not_been_calculated_tick_above", which)}, lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90}));
            else paint(hstring{lycan::Tf(L"main.no_match_no_file_has", which)}, red);
        };

        // the value of the checksum file next to the file against what was calculated
        st->checkSide = [st, green, red]() {
            if (st->sideKind == 0) return;
            const std::wstring name = st->req.sidecar.filename().wstring();
            auto paint = [&](hstring const& t, Windows::UI::Color c) { st->sideResult.Text(t); st->sideResult.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{c}); };
            const Windows::UI::Color amber{lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90})};
            if (st->sideKind == 3) { paint(hstring{lycan::Tf(L"sums.side_unusable", name)}, red); return; }
            if (st->sideKind == 2) { paint(hstring{lycan::Tf(L"sums.side_no_entry", name)}, amber); return; }
            if (!st->have) { paint(hstring{lycan::Tf(L"sums.side_waiting", name)}, lycan_app::StatusColor(Windows::UI::Color{255, 200, 200, 200})); return; }
            if ((st->report.algos & st->sideAlgo) == 0) { paint(hstring{lycan::Tf(L"sums.side_untick", name, lycan::HashAlgoName(st->sideAlgo))}, amber); return; }
            if (st->report.files.empty() || !st->report.files[0].ok()) { st->sideResult.Text(L""); return; }
            const auto list = lycan::HashAlgoList(st->report.algos);
            for (size_t i = 0; i < list.size() && i < st->report.files[0].hex.size(); ++i)
                if (list[i] == st->sideAlgo)
                {
                    if (st->report.files[0].hex[i] == st->sideHex) paint(hstring{lycan::Tf(L"sums.side_match", name)}, green);
                    else paint(hstring{lycan::Tf(L"sums.side_differ", name, FromUtf8(st->sideHex))}, red);
                    return;
                }
        };

        // (re)start the computation
        st->start = [this, st]() {
            if (st->id) { queue_.Cancel(st->id); hashHandlers_.erase(st->id); }
            st->report = {};
            st->have = false;
            st->results.Children().Clear();
            if (!st->req.virusTotal)
            {
                settings_.hashAlgos = st->algos & ~st->sideForced;   // what the checksum file asked for is not the user's choice
                SaveSettings();
            }
            const uint32_t sumAlgo = HashSumAlgo(st->algos);
            st->dlg.PrimaryButtonText(hstring{lycan::Tf(L"main.save_as", HashSumExt(sumAlgo))});
            st->dlg.IsPrimaryButtonEnabled(false);
            st->dlg.IsSecondaryButtonEnabled(false);
            st->running = true;
            st->bar.Value(0);
            st->bar.Visibility(Visibility::Visible);
            st->status.Text(lycan::Tr(L"main.calculating"));
            if (st->checkSide) st->checkSide();
            const bool inArchive = !st->req.archive.empty();
            st->id = inArchive ? queue_.EnqueueHashEntries(st->req.archive, st->req.indices, st->algos, MakeOpenOptions())
                               : queue_.EnqueueHashFiles(st->req.files, st->algos);
            hashHandlers_[st->id] = [this, st](lycan::TaskInfo const& info) {
                if (info.id != st->id) return;
                if (!info.Finished())
                {
                    const double pct = info.total ? 100.0 * static_cast<double>(info.done) / static_cast<double>(info.total) : 0.0;
                    st->bar.Value(pct);
                    st->status.Text(hstring{lycan::Tf(L"main.calculating_progress", std::lround(pct), FormatSize(info.done), FormatSize(info.total))});
                    return;
                }
                st->running = false;
                st->bar.Visibility(Visibility::Collapsed);
                if (info.state == lycan::TaskState::Cancelled) return;
                if (info.hash.files.empty())  // nothing came out: the archive could not be opened, or nothing to hash
                {
                    st->status.Text(hstring{info.error.empty() ? std::wstring(lycan::Tr(L"main.there_nothing_calculate")) : info.error.Text()});
                    return;
                }
                st->report = info.hash;
                st->have = true;
                st->render();
                std::wstring s = lycan::Tf(L"main.done_file_s", st->report.files.size() - st->report.failed, FormatSize(st->report.bytes));
                if (st->report.failed) s += lycan::Tf(L"main.could_not_be_calculated", st->report.failed);
                st->status.Text(hstring{s});
                st->dlg.IsPrimaryButtonEnabled(st->report.failed < st->report.files.size());
                st->dlg.IsSecondaryButtonEnabled(st->report.failed < st->report.files.size());
                st->evaluate();
                if (st->checkSide) st->checkSide();
                if (st->req.virusTotal && !st->vtDone && st->report.failed == 0 && st->report.files.size() == 1)
                {
                    const auto list = lycan::HashAlgoList(st->report.algos);
                    for (size_t i = 0; i < list.size() && i < st->report.files[0].hex.size(); ++i)
                        if (list[i] == lycan::kHashSha256)
                        {
                            st->vtDone = true;
                            OpenVirusTotal(st->report.files[0].hex[i]);
                            st->dlg.Hide();   // done: the browser has the page (the hash stays in the dialog when something went wrong instead)
                        }
                }
            };
        };

        dlg.SecondaryButtonText(lycan::Tr(L"main.copy_all"));
        dlg.CloseButtonText(lycan::Tr(L"main.close"));
        dlg.DefaultButton(ContentDialogButton::Close);
        dlg.PrimaryButtonClick([this, st](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
            a.Cancel(true);  // the dialog stays open
            SaveHashSumsAsync(st);
        });
        dlg.SecondaryButtonClick([st](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
            a.Cancel(true);
            if (!st->have) return;
            const auto list = lycan::HashAlgoList(st->report.algos);
            const bool disk = st->req.archive.empty();
            const fs::path base = !st->req.root.empty() ? st->req.root.parent_path() : disk && !st->req.files.empty() ? st->req.files[0].parent_path() : fs::path{};
            const std::string text = list.size() == 1 ? lycan::HashReportText(st->report, list[0], base) : lycan::HashReportTextAll(st->report, base);
            Windows::ApplicationModel::DataTransfer::DataPackage dp;
            dp.SetText(hstring{FromUtf8(text)});
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
            st->status.Text(lycan::Tr(L"main.copied_to_clipboard_period"));
        });
        st->start();
        dialogWidthDip_ = 820;   // (the compact window's width while the dialog is open)
        co_await ShowDialogAsync(dlg);
        dialogWidthDip_ = 640;

        if (st->id)
        {
            if (st->running) queue_.Cancel(st->id);
            hashHandlers_.erase(st->id);
        }
        st->start = nullptr;  // they hold st: let go of each other
        st->render = nullptr;
        st->evaluate = nullptr;
        st->checkSide = nullptr;
        st->dlg = nullptr;
        if (closeWhenDone && !closing_ && !current_ && view_ == View::Home) Close();  // the window was only the host of this dialog
    }

    // "Save as a checksum file": the file of the strongest chosen algorithm, names relative to the folder it is saved in
    fire_and_forget MainWindow::SaveHashSumsAsync(std::shared_ptr<HashDialogState> st)
    {
        auto strong = get_strong();
        if (st->running || !st->have || st->report.files.empty()) co_return;
        const uint32_t algo = HashSumAlgo(st->report.algos);
        const std::wstring ext = HashSumExt(algo);
        const bool inArchive = !st->req.archive.empty();
        const fs::path first = inArchive ? st->req.archive : !st->req.root.empty() ? st->req.root : st->req.files[0];
        std::wstring stem = inArchive || st->req.files.size() == 1 || !st->req.root.empty() ? first.filename().wstring() : first.parent_path().filename().wstring();
        if (stem.empty()) stem = L"checksums";
        fs::path out;
        if (const wchar_t* t = _wgetenv(L"LYCAN_TEST_HASH_SAVE")) out = t;  // tests: no picker
        else out = fs::path{std::wstring(co_await lycan_app::PickSaveFileAsync(AppWindow().Id(), L"LycanZip.HashSums", first.parent_path(), stem + L"." + ext, {{ext + L" file", ext}},
                                                                               lycan::Tr(L"main.save_checksum_file")))};
        if (out.empty()) co_return;
        const std::string text = lycan::HashReportText(st->report, algo, inArchive ? fs::path{} : out.parent_path());
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << text;
        f.close();
        if (!f.good() || text.empty()) { ShowMessage(lycan::Tr(L"main.could_not_save"), hstring{lycan::Tf(L"main.could_not_write", out.wstring())}); co_return; }
        st->status.Text(hstring{lycan::Tf(L"main.saved", out.wstring())});
    }

    void MainWindow::HashNodes(std::vector<uint32_t> const& nodes)
    {
        if (!current_ || !tree_ || nodes.empty()) return;
        HashRequest req;
        req.archive = currentPath_;
        req.indices = tree_->EntryIndices(nodes);  // folders bring their files
        ShowHashDialogAsync(std::move(req));
    }

    // "Look up on VirusTotal" for one file of the archive: its SHA-256 is calculated from the archive (nothing is unpacked to disk), then the page opens
    void MainWindow::VirusTotalNode(uint32_t node)
    {
        if (!current_ || !tree_ || tree_->Node(node).isDir || tree_->Node(node).entryIndex == lycan::kNoEntry) return;
        HashRequest req;
        req.archive = currentPath_;
        req.indices = {tree_->Node(node).entryIndex};
        req.virusTotal = true;
        ShowHashDialogAsync(std::move(req));
    }

    // ... and for the archive file itself
    void MainWindow::OnVirusTotalArchiveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!current_) return;
        HashRequest req;
        req.files = {currentPath_};
        req.virusTotal = true;
        ShowHashDialogAsync(std::move(req));
    }

    void MainWindow::OnHashArchiveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!current_) return;
        HashRequest req;
        req.files = {currentPath_};
        req.sidecar = lycan::FindArchiveSidecars(currentPath_).archiveSum;
        ShowHashDialogAsync(std::move(req));
    }

    // ---------------------------------------------------------------- Verify checksum files

    // What the checks of a checksum file are shown in; the checks run on a thread of their own (they only read files)
    struct MainWindow::VerifyState
    {
        std::vector<fs::path> sums;
        fs::path archive;                     // empty = the files next to each checksum file; otherwise the files inside this archive
        lycan::ArchiveSumOptions match;       // archive: what the user said about the names, when they could not be matched
        std::atomic<bool> cancel{false}, closed{false};
        std::atomic<ULONGLONG> lastTick{0};
        uint64_t runId = 0;                   // UI thread only: an older run that ends late is ignored
        bool running = false;
        ContentDialog dlg{nullptr};
        ProgressBar bar{nullptr};
        TextBlock status{nullptr};
        StackPanel results{nullptr};
        StackPanel ask{nullptr};
        TextBox strip{nullptr}, add{nullptr};
    };

    static hstring SumFileErrorText(lycan::SumFileError e)
    {
        switch (e)
        {
        case lycan::SumFileError::Unreadable: return lycan::Tr(L"sums.error_unreadable");
        case lycan::SumFileError::TooBig: return lycan::Tr(L"sums.error_too_big");
        case lycan::SumFileError::NoLines: return lycan::Tr(L"sums.error_no_lines");
        default: return {};
        }
    }

    fire_and_forget MainWindow::RunVerifyAsync(std::shared_ptr<VerifyState> st)
    {
        auto strong = get_strong();
        const uint64_t run = ++st->runId;
        st->running = true;
        st->cancel = false;
        st->results.Children().Clear();
        st->ask.Visibility(Visibility::Collapsed);
        st->bar.Value(0);
        st->bar.Visibility(Visibility::Visible);
        st->status.Text(lycan::Tr(L"sums.checking"));
        const std::vector<fs::path> sums = st->sums;
        const fs::path archive = st->archive;
        const lycan::ArchiveSumOptions match = st->match;
        const lycan::OpenOptions oo = archive.empty() ? lycan::OpenOptions{} : MakeOpenOptions();
        const auto dispatcher = dispatcher_;

        std::vector<lycan::SumFile> files;
        std::vector<lycan::SumVerifyReport> reports;
        std::wstring failure;
        bool cancelled = false;
        co_await resume_background();
        try
        {
            std::unique_ptr<lycan::ArchiveSource> src;
            if (!archive.empty()) src = lycan::ArchiveSource::Open(archive, oo);
            for (auto const& file : sums)
            {
                if (st->cancel) { cancelled = true; break; }
                lycan::SumVerifyOptions vo;
                vo.password = oo.password;
                vo.progress = [st, dispatcher, run](uint64_t done, uint64_t total) {
                    if (st->cancel) return false;
                    const ULONGLONG now = GetTickCount64();
                    if (now - st->lastTick.load() > 120)
                    {
                        st->lastTick = now;
                        dispatcher.TryEnqueue([st, run, done, total] {
                            if (st->closed || run != st->runId) return;
                            const double pct = total ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : 0.0;
                            st->bar.Value(pct);
                            st->status.Text(hstring{lycan::Tf(L"sums.checking_progress", std::lround(pct), FormatSize(done), FormatSize(total))});
                        });
                    }
                    return true;
                };
                files.push_back(lycan::ReadSumFile(file));
                reports.push_back(src ? lycan::VerifySumAgainstArchive(files.back(), *src, vo, match) : lycan::VerifySumFile(files.back(), vo));
            }
        }
        catch (lycan::Error const& e)
        {
            if (e.cancelled()) cancelled = true;
            else failure = e.message().Text();
        }
        catch (std::exception const& e) { failure = FromUtf8(e.what()); }
        co_await resume_ui{dispatcher_};
        if (st->closed || run != st->runId) co_return;

        st->running = false;
        st->bar.Visibility(Visibility::Collapsed);
        if (cancelled) { st->status.Text(L""); co_return; }
        if (!failure.empty()) { st->status.Text(hstring{failure}); co_return; }

        using Microsoft::UI::Xaml::Media::SolidColorBrush;
        const Windows::UI::Color green{lycan_app::StatusColor(Windows::UI::Color{255, 0x6C, 0xCB, 0x5F})}, red{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})},
            amber{lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90})};
        auto colored = [](hstring const& text, Windows::UI::Color c, bool bold) {
            TextBlock t;
            t.Text(text);
            t.TextWrapping(TextWrapping::Wrap);
            t.Foreground(SolidColorBrush{c});
            if (bold) t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            return t;
        };
        size_t fine = 0;
        bool needAsk = false;
        for (size_t i = 0; i < reports.size(); ++i)
        {
            const lycan::SumVerifyReport& r = reports[i];
            StackPanel one;
            one.Spacing(4);
            TextBlock title;
            title.Text(hstring{sums[i].filename().wstring()});
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.TextTrimming(TextTrimming::CharacterEllipsis);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(title, box_value(hstring{sums[i].wstring()}));
            one.Children().Append(title);
            if (r.error != lycan::SumFileError::None)
            {
                one.Children().Append(colored(SumFileErrorText(r.error), red, false));
                st->results.Children().Append(one);
                continue;
            }
            if (r.allMatch())
            {
                ++fine;
                one.Children().Append(colored(hstring{lycan::Tf(L"sums.all_match", r.total)}, green, true));
                one.Children().Append(Caption(lycan::Tf(L"sums.bytes_read", FormatSize(r.bytes)), 0.6));
            }
            else
            {
                one.Children().Append(colored(hstring{lycan::Tf(L"sums.some_match", r.matched, r.total)}, red, true));
                size_t shown = 0;
                for (auto const& p : r.problems)
                {
                    if (shown++ >= 300) break;
                    Grid row;
                    row.ColumnSpacing(10);
                    ColumnDefinition c0, c1;
                    c0.Width(GridLength{1, GridUnitType::Star});
                    c1.Width(GridLength{0, GridUnitType::Auto});
                    row.ColumnDefinitions().Append(c0);
                    row.ColumnDefinitions().Append(c1);
                    TextBlock name;
                    name.Text(hstring{p.name});
                    name.TextTrimming(TextTrimming::CharacterEllipsis);
                    Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(name, box_value(hstring{p.name}));
                    row.Children().Append(name);
                    TextBlock what = p.status == lycan::SumStatus::Mismatch ? colored(lycan::Tr(L"sums.differs"), red, false)
                                     : p.status == lycan::SumStatus::Missing ? colored(lycan::Tr(L"sums.not_found"), amber, false)
                                                                             : colored(hstring{lycan::Tf(L"sums.could_not_read", p.error.Text())}, red, false);
                    Grid::SetColumn(what, 1);
                    row.Children().Append(what);
                    one.Children().Append(row);
                }
                if (r.problems.size() > 300) one.Children().Append(Caption(lycan::Tf(L"sums.more_not_listed", r.problems.size() - 300), 0.65));
            }
            if (!r.extra.empty())
            {
                one.Children().Append(colored(hstring{lycan::Tf(L"sums.extra_files", r.extra.size())}, amber, false));
                size_t shown = 0;
                for (auto const& e : r.extra)
                {
                    if (shown++ >= 20) break;
                    one.Children().Append(Caption(e, 0.65));
                }
                if (r.extra.size() > 20) one.Children().Append(Caption(lycan::Tf(L"sums.more_not_listed", r.extra.size() - 20), 0.65));
            }
            if (!archive.empty() && r.confident)
            {
                if (r.sumSkip) one.Children().Append(Caption(lycan::Tf(L"sums.names_skip_checksum", r.sumSkip), 0.6));
                if (r.entrySkip) one.Children().Append(Caption(lycan::Tf(L"sums.names_skip_archive", r.entrySkip), 0.6));
            }
            if (!archive.empty() && !r.confident) needAsk = true;
            if (files[i].badLines) one.Children().Append(Caption(lycan::Tf(L"sums.bad_lines", files[i].badLines), 0.6));
            st->results.Children().Append(one);
        }
        st->status.Text(reports.size() > 1 ? hstring{lycan::Tf(L"sums.summary_many", fine, reports.size())} : hstring{});
        if (needAsk) st->ask.Visibility(Visibility::Visible);
    }

    fire_and_forget MainWindow::ShowVerifyDialogAsync(std::vector<fs::path> sums, fs::path archive, bool closeWhenDone)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        if (sums.empty()) co_return;
        if (!archive.empty() && current_ && current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;

        auto st = std::make_shared<VerifyState>();
        st->sums = std::move(sums);
        st->archive = std::move(archive);
        auto dlg = MakeDialog(lycan::Tr(L"sums.verify_title"), L"");
        st->dlg = dlg;

        StackPanel sp;
        sp.Spacing(10);
        sp.MinWidth(680);
        dlg.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(760.0));
        {
            TextBlock what;
            what.Text(!st->archive.empty() ? hstring{lycan::Tf(L"sums.of_archive", st->archive.filename().wstring())}
                      : st->sums.size() == 1 ? hstring{lycan::Tf(L"sums.of_folder", st->sums[0].parent_path().wstring())}
                                             : hstring{lycan::Tf(L"sums.n_checksum_files", st->sums.size())});
            what.TextTrimming(TextTrimming::CharacterEllipsis);
            what.Opacity(0.75);
            sp.Children().Append(what);
        }
        st->bar = ProgressBar{};
        st->bar.Minimum(0);
        st->bar.Maximum(100);
        sp.Children().Append(st->bar);
        st->status = Caption(L"", 0.75);
        sp.Children().Append(st->status);
        st->results = StackPanel{};
        st->results.Spacing(14);
        ScrollViewer sv;
        sv.Content(st->results);
        sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        sv.MaxHeight(340);
        st->results.Margin(ThicknessHelper::FromLengths(0, 0, 18, 0));
        sp.Children().Append(MakeCardBox(sv));

        // the names of the checksum file are not the names in the archive: the user says how they differ
        st->ask = StackPanel{};
        st->ask.Spacing(8);
        st->ask.Visibility(Visibility::Collapsed);
        {
            TextBlock note;
            note.Text(lycan::Tr(L"sums.names_not_found"));
            note.TextWrapping(TextWrapping::Wrap);
            note.Opacity(0.85);
            st->ask.Children().Append(note);
            st->strip = TextBox{};
            st->strip.Header(box_value(lycan::Tr(L"sums.strip_prefix")));
            st->strip.PlaceholderText(lycan::Tr(L"sums.strip_example"));
            st->ask.Children().Append(st->strip);
            st->add = TextBox{};
            st->add.Header(box_value(lycan::Tr(L"sums.add_prefix")));
            st->add.PlaceholderText(lycan::Tr(L"sums.add_example"));
            st->ask.Children().Append(st->add);
            Button again;
            again.Content(box_value(lycan::Tr(L"sums.check_again")));
            again.Click([this, st](auto&&, auto&&) {
                if (st->running) return;
                st->match.manual = true;
                st->match.stripPrefix = std::wstring{st->strip.Text()};
                st->match.addPrefix = std::wstring{st->add.Text()};
                RunVerifyAsync(st);
            });
            st->ask.Children().Append(again);
        }
        sp.Children().Append(st->ask);
        sp.Children().Append(Caption(lycan::Tr(L"sums.match_only_means"), 0.6));
        dlg.Content(sp);
        dlg.CloseButtonText(lycan::Tr(L"main.close"));
        dlg.DefaultButton(ContentDialogButton::Close);

        RunVerifyAsync(st);
        dialogWidthDip_ = 820;
        co_await ShowDialogAsync(dlg);
        dialogWidthDip_ = 640;

        st->closed = true;
        st->cancel = true;
        st->dlg = nullptr;
        if (closeWhenDone && !closing_ && !current_ && view_ == View::Home) Close();  // the window was only the host of this dialog
    }

    // Explorer starts one process per selected file: the paths of one selection arrive one by one and are checked in one dialog
    void MainWindow::QueueVerify(std::vector<fs::path> paths, bool closeWhenDone)
    {
        for (auto const& p : paths)
            if (std::find(verifyPaths_.begin(), verifyPaths_.end(), p) == verifyPaths_.end()) verifyPaths_.push_back(p);
        verifyClose_ = verifyClose_ || closeWhenDone;
        if (!verifyTimer_)
        {
            verifyTimer_ = dispatcher_.CreateTimer();
            verifyTimer_.IsRepeating(false);
            verifyTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->FlushVerify(); });
        }
        verifyTimer_.Interval(500ms);
        verifyTimer_.Stop();
        verifyTimer_.Start();
    }

    void MainWindow::FlushVerify()
    {
        if (verifyTimer_) verifyTimer_.Stop();
        if (verifyPaths_.empty()) return;
        auto paths = std::move(verifyPaths_);
        verifyPaths_.clear();
        const bool close = verifyClose_;
        verifyClose_ = false;
        std::sort(paths.begin(), paths.end());  // the processes arrive in any order: a steady order for the list
        ShowVerifyDialogAsync(std::move(paths), {}, close);
    }

    // The files and folders of a request (right-click on anything, or the command line): folders bring every file in them. Listing a big
    // folder takes a moment, so it is done off the UI thread.
    fire_and_forget MainWindow::HashRequestAsync(std::vector<fs::path> paths, bool closeWhenDone)
    {
        auto strong = get_strong();
        HashRequest hr;
        bool anyFolder = false;
        co_await resume_background();
        for (auto const& p : paths)
        {
            std::error_code ec;
            if (fs::is_directory(p, ec)) anyFolder = true;
        }
        hr.files = anyFolder ? lycan::ExpandForHash(paths) : paths;
        if (paths.size() == 1 && anyFolder) hr.root = paths[0];
        else if (paths.size() == 1) hr.sidecar = lycan::FindArchiveSidecars(paths[0]).archiveSum;
        co_await resume_ui{dispatcher_};
        if (hr.files.empty())
        {
            ShowMessage(lycan::Tr(L"main.no_files"), lycan::Tr(L"sums.no_files_to_hash"));
            co_return;
        }
        ShowHashDialogAsync(std::move(hr), closeWhenDone);
    }

    // ---------------------------------------------------------------- "Calculate and save as name.sha256"

    // The calculation runs on a thread of its own; the window that asked for it stays out of sight unless it takes long (see HashSaveAsync)
    struct HashSaveState
    {
        std::atomic<bool> cancel{false}, finished{false};
        std::atomic<uint64_t> done{0}, total{0};
        lycan::SumWriteResult result;
        std::wstring error;
        bool cancelled = false;
    };

    static fire_and_forget HashSaveJob(std::shared_ptr<HashSaveState> st, std::vector<fs::path> paths, fs::path out, fs::path base)
    {
        co_await resume_background();
        try
        {
            if (const wchar_t* d = _wgetenv(L"LYCAN_TEST_HASH_DELAY"))   // tests: a slow calculation, to see the progress
                for (int ms = _wtoi(d); ms > 0 && !st->cancel; ms -= 50) Sleep(50);
            if (st->cancel) { st->cancelled = true; st->finished = true; co_return; }
            lycan::SumWriteOptions o;
            o.algo = lycan::kHashSha256;
            o.progress = [st](uint64_t d, uint64_t t) { st->done = d; st->total = t; return !st->cancel.load(); };
            st->result = lycan::WriteSumFile(out, paths, base, o);
        }
        catch (lycan::Error const& e)
        {
            if (e.cancelled()) st->cancelled = true;
            else st->error = e.message().Text();
        }
        catch (std::exception const& e) { st->error = FromUtf8(e.what()); }
        st->finished = true;
    }

    // The SHA-256 of the chosen files and folders into name.sha256: one item = its name (the file is next to it), several = the name of the folder they
    // are in (the file is in that folder). Nothing is shown when it is quick: a notification says it is done. A window comes when it takes long (progress,
    // a way to cancel), when the file is there already (replace it or keep both) and when something went wrong.
    fire_and_forget MainWindow::HashSaveAsync(std::vector<fs::path> paths, bool closeWhenDone)
    {
        auto strong = get_strong();
        OpGuard guard{this};
        if (paths.empty()) co_return;
        auto finishHost = [this, closeWhenDone]() {
            if (closeWhenDone && !closing_ && !current_ && view_ == View::Home) Close();  // the window was only the host of this
        };

        const fs::path dir = paths[0].parent_path();
        std::wstring stem = paths.size() == 1 ? paths[0].filename().wstring() : dir.filename().wstring();
        if (stem.empty() || stem.back() == L':') stem = L"checksums";
        fs::path out = dir / (stem + L".sha256");

        std::error_code ec;
        if (fs::exists(out, ec))
        {
            auto dlg = MakeDialog(lycan::Tr(L"sums.save_exists_title"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"sums.save_exists", out.filename().wstring())});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"sums.save_replace"));
            dlg.SecondaryButtonText(lycan::Tr(L"sums.save_keep_both"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Secondary);   // nothing is lost by default
            const auto r = co_await ShowDialogAsync(dlg);
            if (r == ContentDialogResult::Secondary) out = UniquePath(out);
            else if (r != ContentDialogResult::Primary) { finishHost(); co_return; }
        }

        auto st = std::make_shared<HashSaveState>();
        HashSaveJob(st, paths, out, dir);

        // quick: wait a moment without showing anything
        const auto start = std::chrono::steady_clock::now();
        while (!st->finished && std::chrono::steady_clock::now() - start < 1s)
        {
            co_await resume_after(50ms);
            co_await resume_ui{dispatcher_};
        }

        ContentDialog dlg{nullptr};
        if (!st->finished)
        {
            // it takes long: the window appears, with the progress and a way out
            using Microsoft::UI::Xaml::Media::SolidColorBrush;
            dlg = MakeDialog(lycan::Tr(L"sums.save_title"), L"");
            StackPanel sp;
            sp.Spacing(10);
            sp.MinWidth(420);
            TextBlock name;
            name.Text(hstring{out.filename().wstring()});
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            name.Opacity(0.75);
            sp.Children().Append(name);
            ProgressBar bar;
            bar.Minimum(0);
            bar.Maximum(100);
            sp.Children().Append(bar);
            TextBlock status = Caption(lycan::Tr(L"main.calculating"), 0.75);
            status.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(status);
            dlg.Content(sp);
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.PrimaryButtonText(lycan::Tr(L"main.show_explorer"));
            dlg.IsPrimaryButtonEnabled(false);
            dlg.PrimaryButtonClick([out](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
                a.Cancel(true);  // the dialog stays open
                if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(out.c_str()))
                {
                    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                    ILFree(pidl);
                }
            });
            const auto q = dispatcher_;
            const Windows::UI::Color green{lycan_app::StatusColor(Windows::UI::Color{255, 0x6C, 0xCB, 0x5F})}, red{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})};
            // follows the calculation until it ends; then shows what came out (the dialog stays until it is closed)
            [](std::shared_ptr<HashSaveState> st, Microsoft::UI::Dispatching::DispatcherQueue q, ContentDialog dlg, ProgressBar bar, TextBlock status, fs::path out,
               Windows::UI::Color green, Windows::UI::Color red) -> fire_and_forget {
                while (!st->finished)
                {
                    co_await resume_after(100ms);
                    co_await resume_ui{q};
                    const uint64_t t = st->total, d = st->done;
                    const double pct = t ? 100.0 * static_cast<double>(d) / static_cast<double>(t) : 0.0;
                    bar.Value(pct);
                    status.Text(hstring{lycan::Tf(L"main.calculating_progress", std::lround(pct), FormatSize(d), FormatSize(t))});
                }
                co_await resume_ui{q};
                bar.Visibility(Visibility::Collapsed);
                if (st->cancelled) { dlg.Hide(); co_return; }
                std::wstring text;
                if (!st->error.empty()) text = st->error;
                else
                {
                    text = lycan::Tf(L"sums.save_done", out.wstring());
                    if (st->result.skipped) text += lycan::Tf(L"sums.save_done_skipped", st->result.skipped);
                    dlg.IsPrimaryButtonEnabled(true);
                }
                status.Text(hstring{text});
                status.Opacity(1);
                status.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{st->error.empty() ? green : red});
                dlg.CloseButtonText(lycan::Tr(L"main.close"));
            }(st, q, dlg, bar, status, out, green, red);
            co_await ShowDialogAsync(dlg);
            if (!st->finished)   // closed while it ran: cancel (the file is not left behind)
            {
                st->cancel = true;
                while (!st->finished) { co_await resume_after(50ms); co_await resume_ui{dispatcher_}; }
            }
            finishHost();
            co_return;
        }

        // quick: it is done
        if (st->cancelled) { finishHost(); co_return; }
        if (!st->error.empty())
        {
            auto err = MakeDialog(lycan::Tr(L"sums.save_failed_title"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.could_not_write", out.wstring()) + L"\n" + st->error});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            err.Content(tb);
            err.CloseButtonText(lycan::Tr(L"main.close"));
            co_await ShowDialogAsync(err);
            finishHost();
            co_return;
        }
        if (settings_.notifications && !closing_)
        {
            notifier_.Notify(lycan::Tr(L"sums.notify_title"),
                             st->result.skipped ? lycan::Tf(L"sums.notify_skipped", out.filename().wstring(), st->result.skipped) : out.filename().wstring(), false);
            // the notification goes away when the program ends: stay until it has been shown
            for (int i = 0; i < 40 && notifier_.BalloonActive() && !closing_; ++i)
            {
                co_await resume_after(300ms);
                co_await resume_ui{dispatcher_};
            }
        }
        finishHost();
    }

    void MainWindow::ShowNotice(Border const& strip, bool warning, hstring const& title, hstring const& message,
                                std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> const& links)
    {
        Grid g;
        g.ColumnSpacing(8);
        for (GridUnitType t : {GridUnitType::Auto, GridUnitType::Star, GridUnitType::Auto, GridUnitType::Auto})
        {
            ColumnDefinition c;
            c.Width(GridLength{1, t});
            g.ColumnDefinitions().Append(c);
        }
        FontIcon icon;
        icon.Glyph(warning ? L"\uE7BA" : L"\uE946");
        icon.FontSize(14);
        icon.VerticalAlignment(VerticalAlignment::Center);
        if (warning) icon.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90})});
        g.Children().Append(icon);
        Grid text;   // the title (bold) and the message (dim, cut off with an ellipsis when it does not fit)
        text.ColumnSpacing(10);
        text.VerticalAlignment(VerticalAlignment::Center);
        {
            ColumnDefinition c0, c1;
            c0.Width(GridLength{0, GridUnitType::Auto});
            c1.Width(GridLength{1, GridUnitType::Star});
            text.ColumnDefinitions().Append(c0);
            text.ColumnDefinitions().Append(c1);
        }
        TextBlock t;
        t.Text(title);
        t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        text.Children().Append(t);
        if (!message.empty())
        {
            TextBlock m;
            m.Text(message);
            m.Opacity(0.75);
            m.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(m, 1);
            text.Children().Append(m);
        }
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(text, box_value(hstring{std::wstring{title} + (message.empty() ? L"" : L"  ") + std::wstring{message}}));
        Grid::SetColumn(text, 1);
        g.Children().Append(text);
        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.Spacing(2);
        for (auto const& [label, action] : links)
        {
            HyperlinkButton b;
            b.Content(box_value(label.first));
            b.Padding(ThicknessHelper::FromLengths(8, 2, 8, 2));
            if (!label.second.empty()) Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(b, box_value(label.second));
            b.Click([action = action](auto&&, auto&&) { action(); });
            buttons.Children().Append(b);
        }
        Grid::SetColumn(buttons, 2);
        g.Children().Append(buttons);
        Button close;
        close.Content(MakeGlyph(L"\uE711", 10));
        close.Padding(ThicknessHelper::FromUniformLength(6));
        close.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
        close.BorderThickness(ThicknessHelper::FromUniformLength(0));
        Automation::AutomationProperties::SetName(close, lycan::Tr(L"main.close"));
        close.Click([strip, weak = get_weak()](auto&&, auto&&) {
            auto self = weak.get();
            if (!self || !self->AnimOn()) { strip.Visibility(Visibility::Collapsed); return; }
            // fold away: fade out and move up a little, then collapse
            using namespace Microsoft::UI::Xaml::Media;
            using namespace Microsoft::UI::Xaml::Media::Animation;
            auto ct = strip.RenderTransform().try_as<CompositeTransform>();
            if (!ct) { ct = CompositeTransform{}; strip.RenderTransform(ct); }
            Storyboard sb;
            for (auto [target, prop, to] : {std::tuple<DependencyObject, const wchar_t*, double>{strip, L"Opacity", 0.0}, {ct, L"TranslateY", -8.0}})
            {
                DoubleAnimation a;
                a.To(to);
                a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(150)));
                Storyboard::SetTarget(a, target);
                Storyboard::SetTargetProperty(a, prop);
                sb.Children().Append(a);
            }
            sb.Completed([strip, ct, sb](auto&&, auto&&) { sb.Stop(); strip.Visibility(Visibility::Collapsed); strip.Opacity(1); ct.TranslateY(0); });
            sb.Begin();
        });
        Grid::SetColumn(close, 3);
        g.Children().Append(close);
        strip.Child(g);
        const bool wasHidden = strip.Visibility() == Visibility::Collapsed;
        strip.Visibility(Visibility::Visible);
        if (wasHidden) FadeIn(strip, -8, 200);   // comes down into place
    }

    // The notice above an archive that was opened: a checksum file sits next to it
    void MainWindow::ShowSumsBanner()
    {
        SumsStrip().Visibility(Visibility::Collapsed);
        std::error_code ec;
        if (!current_ || currentPath_.empty() || !fs::is_regular_file(currentPath_, ec)) return;
        const lycan::ArchiveSidecars sc = lycan::FindArchiveSidecars(currentPath_);
        if (sc.archiveSum.empty() && sc.contentsSum.empty()) return;
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        if (!sc.archiveSum.empty())
            links.push_back({{lycan::Tr(L"sums.link_archive"), hstring{lycan::Tf(L"sums.verify_archive_with", sc.archiveSum.filename().wstring())}},
                             [this, sum = sc.archiveSum]() {
                                 HashRequest hr;
                                 hr.files = {currentPath_};
                                 hr.sidecar = sum;
                                 ShowHashDialogAsync(std::move(hr));
                             }});
        if (!sc.contentsSum.empty())
            links.push_back({{lycan::Tr(L"sums.link_files"), hstring{lycan::Tf(L"sums.verify_files_with", sc.contentsSum.filename().wstring())}},
                             [this, sum = sc.contentsSum]() { ShowVerifyDialogAsync({sum}, currentPath_); }});
        ShowNotice(SumsStrip(), false, lycan::Tr(L"sums.banner_title"), {}, links);
    }

    // ---------------------------------------------------------------- Archive information

    // "Archive information": what the archive is (the Windows Properties window is about the file; this is about the archive)
    fire_and_forget MainWindow::ShowArchiveInfoAsync()
    {
        auto strong = get_strong();
        if (!current_) co_return;
        uint64_t files = 0, dirs = 0, unpacked = 0, packed = 0;
        uint32_t encrypted = 0;
        for (auto const& e : current_->Entries())
        {
            if (e.isDir) { ++dirs; continue; }
            ++files;
            unpacked += e.size;
            packed += e.packedSize;
            if (e.isEncrypted) ++encrypted;
        }
        const lycan::ArchiveDetails d = current_->Details();
        const lycan::Capabilities caps = current_->Caps();
        std::error_code ec;
        const uint64_t disk = fs::file_size(currentPath_, ec);
        uint64_t mtime = 0;
        {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(currentPath_.c_str(), GetFileExInfoStandard, &fa))
                mtime = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        }

        std::vector<std::pair<std::wstring, std::wstring>> rows;
        rows.emplace_back(lycan::Tr(L"main.info_location"), currentPath_.parent_path().wstring());
        rows.emplace_back(lycan::Tr(L"main.format"), current_->FormatName());
        if (!ec) rows.emplace_back(lycan::Tr(L"main.file_size"), lycan::Tf(L"fmt.size_bytes", FormatSize(disk), disk));
        rows.emplace_back(lycan::Tr(L"main.properties"), lycan::Tf(L"main.file_s_folder_s", files, dirs));
        {
            std::wstring s = lycan::Tf(L"fmt.size_bytes", FormatSize(unpacked), unpacked);
            if (unpacked > 0 && !ec) s += lycan::Tf(L"main.archive", std::lround(100.0 * static_cast<double>(disk) / static_cast<double>(unpacked)));
            rows.emplace_back(lycan::Tr(L"main.original_size"), s);
        }
        if (!d.method.empty()) rows.emplace_back(lycan::Tr(L"main.compression_method"), d.method);
        {
            std::wstring s = caps.solid ? lycan::Tr(L"main.yes") : lycan::Tr(L"main.no");
            if (caps.solid && d.blocks) s += lycan::Tf(L"main.block_s", d.blocks);
            rows.emplace_back(lycan::Tr(L"main.solid"), s);
        }
        rows.emplace_back(lycan::Tr(L"main.info_encrypted_label"), caps.encrypted ? lycan::Tf(L"main.yes_encrypted_file_s", encrypted) : lycan::Tr(L"main.no"));
        if (d.volumes > 1) rows.emplace_back(lycan::Tr(L"main.volumes"), lycan::Tf(L"main.volume_s", d.volumes));
        if (mtime) rows.emplace_back(lycan::Tr(L"main.modified"), FormatTime(mtime));
        rows.emplace_back(lycan::Tr(L"main.can_be_changed"), caps.canModify ? lycan::Tr(L"main.yes_files_can_be_added_deleted") : lycan::Tr(L"main.no_format_can_only_be_extracted"));

        auto dlg = MakeDialog(hstring{currentPath_.filename().wstring()}, L"\uE7B8");
        dlg.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(720.0));
        StackPanel sp;
        sp.Spacing(6);
        sp.MinWidth(620);
        Grid grid;
        grid.ColumnSpacing(14);
        grid.RowSpacing(5);
        ColumnDefinition c0, c1;
        c0.Width(GridLength{110, GridUnitType::Pixel});
        c1.Width(GridLength{1, GridUnitType::Star});
        grid.ColumnDefinitions().Append(c0);
        grid.ColumnDefinitions().Append(c1);
        int r = 0;
        for (auto const& [label, value] : rows)
        {
            RowDefinition rd;
            rd.Height(GridLength{0, GridUnitType::Auto});
            grid.RowDefinitions().Append(rd);
            TextBlock l;
            l.Text(hstring{label});
            l.Opacity(0.7);
            Grid::SetRow(l, r);
            grid.Children().Append(l);
            TextBlock v;
            v.Text(hstring{value});
            v.TextWrapping(TextWrapping::Wrap);
            v.IsTextSelectionEnabled(true);
            Grid::SetRow(v, r);
            Grid::SetColumn(v, 1);
            grid.Children().Append(v);
            ++r;
        }
        sp.Children().Append(grid);
        if (!d.comment.empty())
        {
            sp.Children().Append(Caption(lycan::Tr(L"main.archive_comment"), 0.7));
            TextBlock c;
            c.Text(hstring{d.comment});
            c.TextWrapping(TextWrapping::Wrap);
            c.IsTextSelectionEnabled(true);
            ScrollViewer sv;
            sv.MaxHeight(160);
            sv.Content(c);
            sp.Children().Append(MakeCardBox(sv));
        }
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.calculate_hash_values"));
        dlg.SecondaryButtonText(lycan::Tr(L"main.show_explorer"));
        dlg.CloseButtonText(lycan::Tr(L"main.close"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const auto res = co_await ShowDialogAsync(dlg);
        if (res == ContentDialogResult::Primary) OnHashArchiveClick(nullptr, nullptr);
        else if (res == ContentDialogResult::Secondary && fs::exists(currentPath_, ec))
        {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(currentPath_.c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        }
    }

    void MainWindow::OnArchiveInfoClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { ShowArchiveInfoAsync(); }

    void MainWindow::OnVerifySumsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { VerifySumsFromPickerAsync(); }

    // "Verify a checksum file…": pick the file, then check the files inside the open archive against it
    fire_and_forget MainWindow::VerifySumsFromPickerAsync()
    {
        auto strong = get_strong();
        if (!current_) co_return;
        auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeChecksum, false, false);
        if (files.Size() == 0) co_return;
        ShowVerifyDialogAsync({fs::path{std::wstring{files.GetAt(0)}}}, currentPath_);
    }

}
