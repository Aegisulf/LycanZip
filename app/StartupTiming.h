#pragma once
// Where a start spends its time: "startup:" lines in the log with the milliseconds since this process was created
// (scripts\startup-timing.ps1 reads them). Only the first window of a process writes them.
#include <windows.h>
#include <atomic>
#include "lycan/diag.h"

namespace lycan_app
{
    inline long long MsSinceProcessStart()
    {
        FILETIME created{}, exited{}, kernel{}, user{}, now{};
        if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
        GetSystemTimePreciseAsFileTime(&now);
        auto v = [](FILETIME f) { return (static_cast<long long>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        return (v(now) - v(created)) / 10000;
    }

    inline std::atomic<bool> gStartupDone{false};   // set when the first archive (or the home page) is drawn: later windows write nothing

    inline void StartupMark(const wchar_t* what, long long ms = -2)
    {
        if (gStartupDone) return;
        lycan::diag::Info(L"startup: {} at {} ms", what, ms == -2 ? MsSinceProcessStart() : ms);
    }

    // Writes the mark at the next frame that is drawn (the work before it is then on the screen); last = the start is over.
    // After the start the mark is still written ("view: ..."), for a request that another process forwarded.
    inline void StartupMarkAtNextFrame(const wchar_t* what, bool last)
    {
        if (!lycan::diag::Detailed()) return;
        if (gStartupDone)
        {
            auto token = std::make_shared<winrt::event_token>();
            *token = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering([what, token](auto&&, auto&&) {
                winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(*token);
                lycan::diag::Info(L"view: {}", what);
            });
            return;
        }
        auto token = std::make_shared<winrt::event_token>();
        *token = winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering([what, last, token](auto&&, auto&&) {
            winrt::Microsoft::UI::Xaml::Media::CompositionTarget::Rendering(*token);
            StartupMark(what);
            if (last) gStartupDone = true;
        });
    }
}
