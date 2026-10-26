#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "CrashHandler.h"
#include "ImagePreview.h"
#include "AudioPreview.h"
#include "OpenTempDir.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <crtdbg.h>
#include <stdlib.h>

#include "lycan/crashreport.h"
#include "lycan/diag.h"
#include "lycan/i18n.h"
#include "lycan/launch.h"
#include "lycan/settings.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::LycanZip::implementation
{
    // The keys as they were when the program was loaded: a double click on the archive starts the program, and the key may be let go before
    // OnLaunched runs (a WinUI start takes a while), so it is read as early as it can be (static initialisation).
    static const bool sShiftAtLoad = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    static const bool sCtrlAtLoad = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    App::App()
    {
        // The automated tests (scripts\ui-smoke.ps1) set this environment variable: any error fails at once, without a CRT / error report dialog that would block the test
        if (GetEnvironmentVariableW(L"LYCAN_NO_DIALOGS", nullptr, 0) > 0)
        {
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#ifdef _DEBUG
            for (int t : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT})
            {
                _CrtSetReportMode(t, _CRTDBG_MODE_FILE);
                _CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
            }
#endif
        }

#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& e)
        {
            // leave a readable reason in automated tests (otherwise only 0xc000027b is left)
            wchar_t log[MAX_PATH];
            if (GetEnvironmentVariableW(L"LYCAN_CRASH_LOG", log, MAX_PATH) > 0)
            {
                std::wstring text = std::wstring(e.Message()) + L"\n";
                HANDLE h = CreateFileW(log, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE)
                {
                    DWORD n = 0;
                    WriteFile(h, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &n, nullptr);
                    CloseHandle(h);
                }
            }
            if (IsDebuggerPresent())
            {
                auto errorMessage = e.Message();
                __debugbreak();
            }
        });
#endif
    }

    namespace
    {
        implementation::MainWindow* Impl(Window const& w) { return get_self<implementation::MainWindow>(w.as<winrt::LycanZip::MainWindow>()); }
    }

    // Creates and shows a new main window and registers it in the window list
    Window App::NewWindow(bool previewOnly, bool deferShow)
    {
        auto main = make<MainWindow>();
        Window w = main;
        windows_.push_back(w);
        void* key = get_abi(w);  // the event handlers remember only an identifying value, not the window itself (otherwise the window and its own events would hold each other)
        w.Closed([this, key](auto&&, auto&&) { OnWindowClosed(key); });
        w.Activated([this, key](auto&&, WindowActivatedEventArgs const& e) {
            if (e.WindowActivationState() == WindowActivationState::Deactivated) return;
            for (auto const& x : windows_)
                if (get_abi(x) == key)
                {
                    active_ = x;
                    Impl(x)->ReloadSettingsIfChanged();  // another window changed the settings: apply them
                    break;
                }
        });
        if (!deferShow) w.Activate();   // a deferred window is not shown at all until its first screen is set (MainWindow::RevealWindow)
        if (previewOnly) Impl(w)->SetPreviewOnly();  // the preview window is the whole UI; this one only hosts it
        else if (deferShow) Impl(w)->DeferShow();   // a request is on its way: show the window when it has the screen that fits it, not the home page first
        return w;
    }

    Window App::ActiveWindow()
    {
        if (active_) return active_;
        return windows_.empty() ? Window{nullptr} : windows_.back();
    }

    // An empty start page window (no archive, no tasks): the one activated most recently is preferred
    Window App::FindPristineWindow()
    {
        if (active_ && Impl(active_)->IsPristine()) return active_;
        for (auto const& w : windows_)
            if (Impl(w)->IsPristine()) return w;
        return Window{nullptr};
    }

    bool App::FocusIfOpen(std::filesystem::path const& archive)
    {
        for (auto const& w : windows_)
            if (Impl(w)->ShowsArchive(archive))
            {
                Impl(w)->BringToFront();
                return true;
            }
        return false;
    }

    Window App::OpenOne(std::filesystem::path const& archive)
    {
        if (FocusIfOpen(archive)) return active_ ? active_ : windows_.back();  // opened twice: do not open a second window
        Window w = FindPristineWindow();
        if (!w) w = NewWindow(false, true);
        lycan::LaunchRequest r;
        r.valid = true;
        r.action = lycan::LaunchAction::Open;
        r.paths = {archive};
        Impl(w)->HandleRequest(r, false);  // this also brings the window to the front
        return w;
    }

    void App::UpdateCrashSnapshot()
    {
        std::vector<std::filesystem::path> archives;
        std::vector<lycan::CrashTask> tasks;
        for (auto const& w : windows_) Impl(w)->AppendCrashState(archives, tasks);
        lycan_app::SetCrashSnapshot(lycan::BuildCrashSnapshot(archives, tasks));
    }

    void App::OnWindowClosed(void* key)
    {
        // a window cannot be released in the middle of an event handler: queue it for the next round
        dispatcher_.TryEnqueue([this, key] {
            for (auto it = windows_.begin(); it != windows_.end(); ++it)
                if (get_abi(*it) == key)
                {
                    if (active_ && get_abi(active_) == key) active_ = nullptr;
                    windows_.erase(it);
                    UpdateCrashSnapshot();
                    break;
                }
            if (windows_.empty())
            {
                lycan_app::CloseImagePreview();  // the preview window is a window too; the process would not end without closing it
                lycan_app::CloseAudioPreview();
                lycan_app::CleanOwnOpenTempDirs();  // temporary files of "open": delete our own (those still in use stay)
                Exit();
            }
        });
    }

    // ---- Background work -------------------------------------------------------------------------------------------
    // The user clicked "run in the background": hide every window; the tasks keep running in their worker processes.
    // The tray icon exists only in this mode. When all tasks are done: failures bring the windows back, otherwise one
    // notification and the process exits (nothing stays resident).
    void App::EnterBackground()
    {
        if (bgActive_) return;
        int running = 0;
        for (auto const& w : windows_) running += Impl(w)->CollectTaskStats().running;
        if (running == 0) return;

        lycan_app::BackgroundHost::Callbacks cb;
        cb.restore = [this] { dispatcher_.TryEnqueue([this] { LeaveBackground(); }); };
        auto cancelAll = [this] {
            dispatcher_.TryEnqueue([this] {
                bgQuiet_ = true;  // leave quietly once the tasks have ended
                for (auto const& w : windows_) Impl(w)->CancelAllTasks();
            });
        };
        cb.cancelAll = cancelAll;
        cb.exitApp = cancelAll;
        if (!bgHost_.Start(std::move(cb))) return;  // no icon: stay visible instead of vanishing

        bgActive_ = true;
        bgQuiet_ = bgFinishing_ = false;
        bgIdleTicks_ = bgTicks_ = 0;
        for (auto const& w : windows_) Impl(w)->HideForBackground();
        if (!bgTimer_)
        {
            bgTimer_ = dispatcher_.CreateTimer();
            bgTimer_.Interval(std::chrono::milliseconds(500));
            bgTimer_.Tick([this](auto&&, auto&&) { PollBackground(); });
        }
        bgTimer_.Start();
        SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));  // free the window memory
        PollBackground();
    }

    void App::LeaveBackground()
    {
        if (!bgActive_) return;
        bgActive_ = false;
        bgFinishing_ = false;
        bgTimer_.Stop();
        bgHost_.Stop();
        for (auto const& w : windows_) Impl(w)->ShowFromBackground();
        if (active_) Impl(active_)->BringToFront();
    }

    void App::PollBackground()
    {
        if (!bgActive_ || bgFinishing_) return;
        if (++bgTicks_ == 4) SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));  // after the UI has settled
        MainWindow::TaskStats all;
        for (auto const& w : windows_)
        {
            auto s = Impl(w)->CollectTaskStats();
            all.running += s.running; all.failed += s.failed; all.needsUi += s.needsUi; all.done += s.done; all.total += s.total;
        }
        if (all.running > 0 || all.needsUi > 0)
        {
            bgIdleTicks_ = 0;
            if (all.running > 0)
            {
                const int pct = all.total > 0 ? static_cast<int>(std::min<unsigned long long>(all.done * 100 / all.total, 100)) : 0;
                bgHost_.SetTip(all.total > 0 ? lycan::Tf(L"app.lycanzip_task_s_running", all.running, pct)
                                             : lycan::Tf(L"app.lycanzip_tasks_running_no_percent", all.running));
            }
            return;
        }
        // nothing is running: wait for two quiet ticks (a follow-up test may be queued right after a compression ends)
        if (++bgIdleTicks_ < 2) return;

        if (all.failed > 0)
        {
            LeaveBackground();  // the user must see what went wrong
            return;
        }
        auto quit = [this] {
            bgActive_ = false;
            bgTimer_.Stop();
            bgHost_.Stop();
            lycan_app::CloseImagePreview();
            lycan_app::CloseAudioPreview();
            lycan_app::CleanOwnOpenTempDirs();
            Exit();
        };
        if (bgQuiet_ || !lycan::LoadSettings(lycan::DefaultSettingsPath()).notifications) { quit(); return; }
        bgFinishing_ = true;
        bgHost_.Notify(lycan::Tr(L"app.tasks_finished"), lycan::Tr(L"app.all_tasks_finished"), false, [this, quit](bool clicked) {
            dispatcher_.TryEnqueue([this, quit, clicked] {
                if (!bgActive_) return;
                if (clicked) LeaveBackground();  // the user wants to see the results
                else quit();
            });
        });
    }

    // Requests handed over by other instances
    void App::Route(lycan::LaunchRequest const& req)
    {
        LeaveBackground();  // somebody is back (a new request arrived): show the windows again
        if (req.action == lycan::LaunchAction::Open && !req.paths.empty() && MainWindow::IsOpenableArchive(req.paths[0]))
        {
            // On an Explorer multi-select each file starts a process: collect the same batch for a short time, then open a window for each
            for (auto const& p : req.paths)
                if (std::find(openBatch_.begin(), openBatch_.end(), p) == openBatch_.end()) openBatch_.push_back(p);
            openTimer_.Stop();
            openTimer_.Start();
            return;
        }
        if (req.IsPreviewOnly())
        {
            Impl(NewWindow(true))->HandleRequest(req, false);
            return;
        }
        // A context-menu action (compress, extract, test, add, hash) has a window of its own, like when no window is open: its progress must
        // not appear in the window of an archive that is being looked at, nor in the compact window of an action that is still running.
        // The requests of one selection (Explorer starts one process per file) arrive within a moment: they share the new window.
        if (req.IsQuickAction() || req.action == lycan::LaunchAction::Hash || req.action == lycan::LaunchAction::Verify || req.action == lycan::LaunchAction::HashSave || req.action == lycan::LaunchAction::Open)
        {
            const auto now = std::chrono::steady_clock::now();
            if (lastQuick_ && now - lastQuickAt_ < std::chrono::milliseconds(1500))
                for (auto const& w : windows_)
                    if (get_abi(w) == get_abi(lastQuick_))
                    {
                        lastQuickAt_ = now;
                        Impl(w)->HandleRequest(req, false);
                        return;
                    }
            Window w = NewWindow(false, true);
            lastQuick_ = w;
            lastQuickAt_ = now;
            Impl(w)->HandleRequest(req, true);   // as if the program had been started for it: the compact window, closed when it is done
            return;
        }
        Window w = ActiveWindow();
        if (!w) w = NewWindow();
        Impl(w)->HandleRequest(req, false);
    }

    winrt::fire_and_forget App::FlushOpenBatch()
    {
        if (askingBatch_) co_return;  // after the dialog is answered, what was received is handled again
        auto paths = std::move(openBatch_);
        openBatch_.clear();
        std::sort(paths.begin(), paths.end());  // the processes arrive in no fixed order: sorting makes the order of the windows stable
        std::vector<std::filesystem::path> todo;
        for (auto const& p : paths)
            if (!FocusIfOpen(p)) todo.push_back(p);  // already open: bring to the front, not counted in the number of new ones
        size_t limit = todo.size();
        if (limit > kMaxOpenWindows)
        {
            Window w = ActiveWindow();
            if (!w) w = NewWindow();
            askingBatch_ = true;
            const int n = co_await Impl(w)->AskOpenBatchAsync(static_cast<int>(todo.size()), static_cast<int>(kMaxOpenWindows), static_cast<int>(kMaxOpenAll));
            askingBatch_ = false;
            limit = static_cast<size_t>(n);
        }
        if (limit == 1 && todo.size() == 1)
        {
            // a single archive: according to the "open mode" setting it may replace the content of the current window
            Window w = ActiveWindow();
            if (w && Impl(w)->ReplaceOnOpen())
            {
                lycan::LaunchRequest r;
                r.valid = true;
                r.action = lycan::LaunchAction::Open;
                r.paths = {todo[0]};
                Impl(w)->HandleRequest(r, false);
                limit = 0;
            }
        }
        for (size_t i = 0; i < todo.size() && i < limit; ++i) OpenOne(todo[i]);
        if (!openBatch_.empty()) FlushOpenBatch();  // new requests arrived while asking
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        // Command line -> start request
        std::vector<std::wstring> args;
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
        {
            for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
            LocalFree(argv);
        }
        // --wait-pid <n>: started by "switch the data location": wait until that process (the old window) has ended
        for (size_t i = 0; i + 1 < args.size();)
        {
            if (args[i] == L"--wait-pid")
            {
                if (const DWORD pid = static_cast<DWORD>(_wtoi(args[i + 1].c_str())))
                    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid))
                    {
                        WaitForSingleObject(h, 15000);
                        CloseHandle(h);
                    }
                args.erase(args.begin() + static_cast<std::ptrdiff_t>(i), args.begin() + static_cast<std::ptrdiff_t>(i) + 2);
            }
            else ++i;
        }
        lycan::LaunchRequest request = lycan::ParseLaunchArgs(args);
        // an archive opened with the key of the setting held (Shift by default) goes straight to the picture preview; without pictures it
        // opens as usual (PreviewOnlyAsync). Read here, in the process that Explorer started, before it forwards to the running one.
        if (request.valid && request.action == lycan::LaunchAction::Open && request.paths.size() == 1 && MainWindow::IsOpenableArchive(request.paths[0]))
        {
            const auto key = lycan::LoadSettings(lycan::DefaultSettingsPath()).openPreviewKey;
            const int vk = key == lycan::PreviewKey::Shift ? VK_SHIFT : key == lycan::PreviewKey::Ctrl ? VK_CONTROL : 0;
            // LYCAN_TEST_KEYS=shift or ctrl: tests cannot hold a key for another process on every machine (injected keys are not always seen)
            wchar_t fake[16] = {};
            GetEnvironmentVariableW(L"LYCAN_TEST_KEYS", fake, 16);
            const bool testHeld = (vk == VK_SHIFT && _wcsicmp(fake, L"shift") == 0) || (vk == VK_CONTROL && _wcsicmp(fake, L"ctrl") == 0);
            if (vk && (testHeld || (GetAsyncKeyState(vk) & 0x8000) || (vk == VK_SHIFT ? sShiftAtLoad : sCtrlAtLoad))) request.action = lycan::LaunchAction::PreviewImages;
        }
        lycan::diag::Init(L"app");
        lycan::diag::LogStartup();
        lycan::diag::Info(L"launch: action {}, {} path(s){}", static_cast<int>(request.action), request.paths.size(), request.valid ? L"" : L" (arguments not understood)");

        // There is a main instance already: hand the request over (including "just bring the window to the front") and exit
        if (!instance_.BecomePrimary())
        {
            AllowSetForegroundWindow(ASFW_ANY);
            lycan::LaunchRequest forwarded = request.valid ? request : lycan::LaunchRequest{};
            if (instance_.Forward(lycan::SerializeLaunchRequest(forwarded), 10000))
            {
                Exit();
                return;
            }
            // the main instance does not answer (it may be stuck): open our own window as usual
        }

        // Interface language: loaded before any window is made (the environment variable LYCAN_LANG lets tests force a language)
        {
            std::wstring want = lycan::LoadSettings(lycan::DefaultSettingsPath()).language;
            wchar_t env[32] = {};
            if (GetEnvironmentVariableW(L"LYCAN_LANG", env, 32) > 0 && env[0]) want = env;
            auto langDir = lycan::i18n::DefaultLangDir();
            lycan::i18n::Load(langDir, lycan::i18n::Resolve(want, langDir));
        }

        lycan_app::InstallCrashHandler();  // on a crash write a local report (nothing is uploaded); announced at the next start

        Window first = NewWindow(request.valid && request.IsPreviewOnly(), request.valid && request.action != lycan::LaunchAction::None && !request.IsPreviewOnly());
        dispatcher_ = first.DispatcherQueue();
        MainWindow::sStateChanged = [this] { UpdateCrashSnapshot(); };
        MainWindow::sEnterBackground = [this] { EnterBackground(); };
        MainWindow::sLeaveBackground = [this] { LeaveBackground(); };
        MainWindow::sBackgroundActive = [this] { return bgActive_; };
        openTimer_ = dispatcher_.CreateTimer();
        openTimer_.IsRepeating(false);
        openTimer_.Interval(std::chrono::milliseconds(400));
        openTimer_.Tick([this](auto&&, auto&&) { FlushOpenBatch(); });

        // an archive opened from inside a window that is in use already: open a new window (see MainWindow::OpenFromUi)
        MainWindow::sOpenInNewWindow = [this](std::filesystem::path p) { OpenOne(p); };

        instance_.StartServer([this](std::wstring message) {
            dispatcher_.TryEnqueue([this, message = std::move(message)] {
                auto req = lycan::DeserializeLaunchRequest(message);
                if (req.valid) Route(req);
                else if (auto w = ActiveWindow()) { LeaveBackground(); Impl(w)->HandleRequest(lycan::LaunchRequest{}, false); }
            });
        });

        auto impl = Impl(first);
        if (!request.valid) impl->ReportBadArguments(request.error.Text());
        else impl->HandleRequest(request, true);
        if (request.valid && (request.IsQuickAction() || request.action == lycan::LaunchAction::Hash || request.action == lycan::LaunchAction::Verify || request.action == lycan::LaunchAction::HashSave)) { lastQuick_ = first; lastQuickAt_ = std::chrono::steady_clock::now(); }

        // The report left by the last crash: announce it, with one click to reopen the archives of last time. The compact window of a context menu action is not disturbed; it waits for the next normal start
        if (!(request.valid && (request.IsQuickAction() || request.IsPreviewOnly())))
        {
            auto reports = lycan::PendingCrashReports(lycan::DefaultCrashDir());
            if (!reports.empty())
            {
                lycan::diag::Warn(L"the last run ended with a crash: {} report(s), the newest is {}", reports.size(), lycan::diag::Name(reports.front().file));
                impl->ShowCrashRecovery(std::move(reports));
            }
        }

        // For tests: LYCAN_TEST_CRASH=<milliseconds>, after that time an access violation is caused on purpose on a **worker thread** (simulates 7z.dll crashing on a worker thread),
        // to verify the crash report and recovery. Note: an access violation inside a WinRT callback on the UI thread (event handler, timer) is turned into a fail-fast by the runtime,
        // does not pass the unhandled exception filter, so it is not tested here; see the limits in the design notes "Crash records and recovery".
        wchar_t crashEnv[16] = {};
        if (GetEnvironmentVariableW(L"LYCAN_TEST_CRASH", crashEnv, 16) > 0 && _wtoi(crashEnv) > 0)
        {
            const int ms = _wtoi(crashEnv);
            std::thread([ms] {
                Sleep(static_cast<DWORD>(ms));
                *reinterpret_cast<volatile int*>(nullptr) = 1;
            }).detach();
        }
    }
}
