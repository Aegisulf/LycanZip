#pragma once
#include "App.g.h"  // includes factory_implementation::AppT and pulls in the App.xaml.g.h made by the XAML compiler
#include "BackgroundHost.h"
#include "SingleInstance.h"

#include <chrono>
#include <filesystem>
#include <vector>

#include "lycan/launch.h"

namespace winrt::LycanZip::implementation
{
    struct App : AppT<App>
    {
        App();
        void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);

    private:
        // Several windows (the design notes "Several windows"): one process, one MainWindow for each archive;
        // requests handed over by other instances are dispatched by the App, and the process ends when the last window closes.
        static constexpr size_t kMaxOpenWindows = 5;   // a hand-over of more than this number is asked about
        static constexpr size_t kMaxOpenAll = 20;      // upper limit of "open all" when asking

        Microsoft::UI::Xaml::Window NewWindow(bool previewOnly = false, bool deferShow = false);   // deferShow: the window appears when its first screen is ready (see MainWindow::DeferShow)
        Microsoft::UI::Xaml::Window ActiveWindow();
        Microsoft::UI::Xaml::Window FindPristineWindow();
        bool FocusIfOpen(std::filesystem::path const& archive);  // there is a window showing (or opening) this archive already: bring it to the front
        Microsoft::UI::Xaml::Window OpenOne(std::filesystem::path const& archive);  // brings it to the front if it is open already; an empty window opens it in place, otherwise a new window
        void Route(lycan::LaunchRequest const& req);
        winrt::fire_and_forget FlushOpenBatch();
        void OnWindowClosed(void* key);
        // Background work (the design notes "Background work"): hidden windows + tray icon while tasks run, quit when they are done
        void EnterBackground();
        void LeaveBackground();
        void PollBackground();
        bool bgActive_ = false;
        bool bgQuiet_ = false;        // cancel all / exit from the tray menu: leave without a notification
        bool bgFinishing_ = false;    // the final notification is showing; the process exits when it is gone
        int bgIdleTicks_ = 0;
        int bgTicks_ = 0;
        lycan_app::BackgroundHost bgHost_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer bgTimer_{nullptr};
        void UpdateCrashSnapshot();  // gives the crash handler "which archives are open, which tasks are not finished" (see CrashHandler.h)

        std::vector<Microsoft::UI::Xaml::Window> windows_;
        Microsoft::UI::Xaml::Window active_{nullptr};  // the window activated last (cleared when it closes)
        Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueueTimer openTimer_{nullptr};
        // The window of the context-menu action that came last: requests of the same selection (Explorer starts one process per file) go to it
        Microsoft::UI::Xaml::Window lastQuick_{nullptr};
        std::chrono::steady_clock::time_point lastQuickAt_{};
        std::vector<std::filesystem::path> openBatch_;  // "open" requests that the processes hand over one after the other on an Explorer multi-select; collected for a short time and handled together
        bool askingBatch_ = false;                      // asking "how many to open": requests arriving meanwhile are kept
        SingleInstance instance_;
    };
}

namespace winrt::LycanZip::factory_implementation
{
    struct App : AppT<App, implementation::App> {};
}
