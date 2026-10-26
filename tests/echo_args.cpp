// A stand-in exe for tests: writes its own full command line (UTF-16) to the file named by the environment variable LYCAN_ECHO_OUT, to verify the command line that the extension sends.
#include <windows.h>

#include <iterator>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t out[MAX_PATH * 2];
    if (!GetEnvironmentVariableW(L"LYCAN_ECHO_OUT", out, static_cast<DWORD>(std::size(out)))) return 2;
    const wchar_t* cmd = GetCommandLineW();
    HANDLE h = CreateFileW(out, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 3;
    DWORD w;
    WriteFile(h, cmd, static_cast<DWORD>(lstrlenW(cmd) * sizeof(wchar_t)), &w, nullptr);
    WriteFile(h, L"\n", sizeof(wchar_t), &w, nullptr);
    CloseHandle(h);
    return 0;
}
