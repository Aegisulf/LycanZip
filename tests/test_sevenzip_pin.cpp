// A build that ships its 7z.dll loads only that file: the hash must match, and no other 7-Zip on the computer is used.
// Runs `lycanzip-cli info` (it prints which 7z.dll it loaded or why it did not) with different environments.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static std::string Run(const fs::path& exe) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\" info";
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    std::string out;
    if (ok) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n) out.append(buf, n);
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
    return out;
}

static bool Has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

int main() {
#ifndef EXPECT_PINNED
    std::puts("test_sevenzip_pin: this build has no pinned 7z.dll (nothing to test)");
    return 0;
#else
    wchar_t self[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    const fs::path bin = fs::path(std::wstring(self, n)).parent_path();
    const fs::path cli = bin / L"lycanzip-cli.exe", dll = bin / L"7z.dll";
    const fs::path root = fs::temp_directory_path() / (L"lycan_pin_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root);
    _wputenv_s(L"LYCAN_LANG", L"en-US");
    _wputenv_s(L"LYCAN_7Z_DLL", L"");
    _wputenv_s(L"LYCAN_ALLOW_OTHER_7Z", L"");

    // the file next to the exe, as shipped: loads
    {
        const std::string o = Run(cli);
        EXPECT(Has(o, "(7-Zip ") && !Has(o, "not found"));
    }
    // a changed 7z.dll (one byte more) named by LYCAN_7Z_DLL: refused, with the message that says what to do
    const fs::path bad = root / L"7z.dll";
    fs::copy_file(dll, bad);
    { std::ofstream(bad, std::ios::binary | std::ios::app) << 'x'; }
    {
        _wputenv_s(L"LYCAN_7Z_DLL", bad.c_str());
        const std::string o = Run(cli);
        EXPECT(Has(o, "not the one that LycanZip came with") && !Has(o, "(7-Zip "));
        // the development switch lets it through
        _wputenv_s(L"LYCAN_ALLOW_OTHER_7Z", L"1");
        const std::string o2 = Run(cli);
        EXPECT(Has(o2, "(7-Zip "));
        _wputenv_s(L"LYCAN_ALLOW_OTHER_7Z", L"");
        _wputenv_s(L"LYCAN_7Z_DLL", L"");
    }
    // the exe without a 7z.dll next to it: an error, even when another 7-Zip is installed on the computer
    {
        const fs::path alone = root / L"alone";
        fs::create_directories(alone);
        fs::copy_file(cli, alone / L"lycanzip-cli.exe");
        const std::string o = Run(alone / L"lycanzip-cli.exe");
        EXPECT(Has(o, "not found") && Has(o, "next to LycanZip") && !Has(o, "(7-Zip "));
    }
    fs::remove_all(root);
    if (failures == 0) std::puts("test_sevenzip_pin: all passed");
    return failures ? 1 : 0;
#endif
}
