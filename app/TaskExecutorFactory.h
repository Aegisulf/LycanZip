#pragma once
// Chooses where the tasks of a window run: in worker processes (lycanzip-cli.exe --worker, next to the app) when that
// executable is there, otherwise in this process. LYCAN_NO_WORKER=1 forces the in-process executor (tests, debugging).
#include <windows.h>

#include <filesystem>
#include <memory>

#include "lycan/taskexec.h"

namespace lycan_app {

inline std::shared_ptr<lycan::TaskExecutor> MakeTaskExecutor() {
    if (GetEnvironmentVariableW(L"LYCAN_NO_WORKER", nullptr, 0) > 0) return nullptr;
    wchar_t self[MAX_PATH * 2];
    if (!GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)))) return nullptr;
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::path(self).parent_path() / L"lycanzip-cli.exe";
    if (!std::filesystem::exists(exe, ec)) return nullptr;
    lycan::ProcessExecutorOptions o;
    o.workerExe = exe;
    return lycan::MakeProcessExecutor(o);
}

}  // namespace lycan_app
