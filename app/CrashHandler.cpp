#include "pch.h"
#include "CrashHandler.h"

#include <dbghelp.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <filesystem>

#include "lycan/crashreport.h"
#include "lycan/version.h"

namespace lycan_app {
namespace {

// Snapshot: two buffers written in turn; the crash handler reads only the one that is valid now (no lock, so a crash cannot get stuck on a lock someone else holds)
constexpr size_t kSnapCap = 24 * 1024;
char g_snap[2][kSnapCap];
size_t g_snapLen[2] = {0, 0};
std::atomic<int> g_active{0};

wchar_t g_dir[MAX_PATH * 2] = L"";
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                         PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDumpFn g_writeDump = nullptr;
std::atomic<bool> g_inHandler{false};

// Best effort: memory may be damaged already, so only stack buffers and Win32 API are used, and if something fails, so be it
void WriteReport(EXCEPTION_POINTERS* ep, unsigned long code) {
    if (!g_dir[0]) return;
    CreateDirectoryW(g_dir, nullptr);  // the folder is made only on a real crash (the parent was checked at install); fine if it exists
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stem[96], time[32];
    swprintf_s(stem, L"crash-%04u%02u%02u-%02u%02u%02u-%lu", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
    swprintf_s(time, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // Where the exception happened: module name + offset (for example 7z.dll+0x1A2B3C)
    wchar_t addr[MAX_PATH + 40] = L"";
    if (ep && ep->ExceptionRecord) {
        const auto where = reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress);
        HMODULE mod = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(where), &mod) && mod) {
            wchar_t path[MAX_PATH] = L"";
            GetModuleFileNameW(mod, path, MAX_PATH);
            const wchar_t* base = wcsrchr(path, L'\\');
            swprintf_s(addr, L"%s+0x%llX", base ? base + 1 : path, static_cast<unsigned long long>(where - reinterpret_cast<uintptr_t>(mod)));
        } else {
            swprintf_s(addr, L"0x%llX", static_cast<unsigned long long>(where));
        }
    }

    wchar_t dumpName[128] = L"";
    if (g_writeDump) {
        swprintf_s(dumpName, L"%s.dmp", stem);
        wchar_t full[MAX_PATH * 2 + 128];
        swprintf_s(full, L"%s\\%s", g_dir, dumpName);
        HANDLE f = CreateFileW(full, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), ep, FALSE};
            const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
            const BOOL ok = g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, ep ? &mei : nullptr, nullptr, nullptr);
            CloseHandle(f);
            if (!ok) {
                DeleteFileW(full);
                dumpName[0] = L'\0';
            }
        } else {
            dumpName[0] = L'\0';
        }
    }

    wchar_t txt[MAX_PATH * 2 + 128];
    swprintf_s(txt, L"%s\\%s.txt", g_dir, stem);
    lycan::CrashHeader h;
    h.time = time;
    h.version = LYCAN_VERSION_WSTRING;
    h.code = code;
    h.address = addr;
    h.dumpName = dumpName;
    const int idx = g_active.load();
    lycan::WriteCrashReport(txt, h, g_snap[idx], g_snapLen[idx]);
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
    if (g_inHandler.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;  // crashed again while handling a crash: give it back to the system, no endless recursion
    WriteReport(ep, ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0);
    return EXCEPTION_EXECUTE_HANDLER;  // end the process at once (no Windows error report window)
}

void __cdecl OnTerminate() {
    if (!g_inHandler.exchange(true)) WriteReport(nullptr, 0xE0000001);  // a C++ exception / coroutine exception that nobody caught: no exception information, the dump has only the current stack
    TerminateProcess(GetCurrentProcess(), 0xE0000001);
}

}  // namespace

void InstallCrashHandler() {
    const auto dir = lycan::DefaultCrashDir();
    std::error_code ec;
    std::filesystem::create_directories(dir.parent_path(), ec);  // only make sure the parent exists; the crash folder itself is made on a real crash (no empty folder in the portable folder)
    if (!ec) {
        wcsncpy_s(g_dir, dir.c_str(), _TRUNCATE);
        lycan::PruneCrashDir(dir, 5);  // if the folder does not exist, do nothing
    }
    if (HMODULE dbg = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbg, "MiniDumpWriteDump"));
    SetUnhandledExceptionFilter(CrashFilter);
    std::set_terminate(OnTerminate);
}

void SetCrashSnapshot(const std::string& utf8) {
    const int idx = 1 - g_active.load();
    size_t n = std::min(utf8.size(), kSnapCap - 1);
    if (n < utf8.size()) {  // too long: cut at the last complete line end (not in the middle of a line or of a multi-byte character)
        while (n > 0 && utf8[n - 1] != '\n') --n;
    }
    std::memcpy(g_snap[idx], utf8.data(), n);
    g_snapLen[idx] = n;
    g_active.store(idx);
}

}  // namespace lycan_app
