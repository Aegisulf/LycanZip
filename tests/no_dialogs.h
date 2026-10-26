#pragma once
// Test programs / the CLI must not open any dialog: the assert / abort window of the Debug CRT and the Windows error report window wait for a person to press a button for ever,
// which blocks the whole run in automation (ctest, scripts, CI). Write to stderr and exit instead.
#include <crtdbg.h>
#include <initializer_list>
#include <stdlib.h>
#include <windows.h>

namespace {
struct NoDialogs {
    NoDialogs() {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        // A temp folder given by its 8.3 name (CI runners: a user folder named RUNNER~1) must not decide what a path looks like: the program
        // under test (the shell extension, the worker) hands back full names, so the tests compare against those.
        for (const wchar_t* name : {L"TEMP", L"TMP"}) {
            wchar_t shortPath[MAX_PATH], longPath[MAX_PATH];
            const DWORD n = GetEnvironmentVariableW(name, shortPath, MAX_PATH);
            if (n && n < MAX_PATH) {
                const DWORD m = GetLongPathNameW(shortPath, longPath, MAX_PATH);
                if (m && m < MAX_PATH) SetEnvironmentVariableW(name, longPath);
            }
        }
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
        for (int t : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(t, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
        }
#endif
    }
};
const NoDialogs g_noDialogs;
}  // namespace
