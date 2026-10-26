#pragma once
// Single instance: the context menu starts the exe once for each selected file; this lets the second and later processes hand their request to the first one
// (the main instance) and exit at once, and the main instance handles them together (for example putting several files into one archive).
// Transport is a named pipe, current user only, remote connections refused.
// One main instance per COPY of LycanZip, not per user: the names carry a hash of the data folder (lycan::GetDataLocation: the Store package's
// LocalState, %LOCALAPPDATA%\LycanZip, or a portable folder). Otherwise starting the Store version while an installed or portable copy runs would
// hand the request to that copy, which shows its own window with its own settings.
#include <windows.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance();
    SingleInstance(const SingleInstance&) = delete;

    // Becomes the main instance; returns false when there is a main instance already.
    bool BecomePrimary();

    // Secondary instance: sends a message to the main instance. When the main instance has just started and the pipe is not ready it retries until timeoutMs.
    bool Forward(const std::wstring& message, DWORD timeoutMs);

    // Main instance: starts receiving messages. The callback runs on a background thread.
    void StartServer(std::function<void(std::wstring)> onMessage);
    void Stop();

private:
    static std::wstring PipeName();
    static std::wstring CopyTag();   // hash of the data folder of this copy
    void ServerLoop();

    HANDLE mutex_ = nullptr;
    std::thread server_;
    std::atomic<bool> stopping_{false};
    std::function<void(std::wstring)> onMessage_;
};
