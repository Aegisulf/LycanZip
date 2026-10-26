#include "lycan/diag.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/datalocation.h"
#include "lycan/settings.h"
#include "lycan/version.h"

namespace fs = std::filesystem;

namespace lycan::diag {

namespace {

constexpr uint64_t kMaxBytes = 2ull << 20;
constexpr int kKeep = 4;  // lycanzip.log + .1 .2 .3

std::atomic<bool> g_detailed{false}, g_full{false}, g_init{false};
std::mutex g_m;
std::wstring g_process = L"?";

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8Impl(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

fs::path NumberedFile(int i) {  // 0 = the current one
    return Dir() / (i == 0 ? L"lycanzip.log" : L"lycanzip." + std::to_wstring(i) + L".log");
}

// Another process may be writing: the rename of the files is done by one process at a time
void Rotate() {
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\LycanZipLogRotate");
    if (mutex) WaitForSingleObject(mutex, 2000);
    std::error_code ec;
    // somebody else may have rotated just now: only when the current file is still big
    if (fs::exists(NumberedFile(0), ec) && fs::file_size(NumberedFile(0), ec) >= kMaxBytes) {
        fs::remove(NumberedFile(kKeep - 1), ec);
        for (int i = kKeep - 2; i >= 0; --i)
            if (fs::exists(NumberedFile(i), ec)) fs::rename(NumberedFile(i), NumberedFile(i + 1), ec);
    }
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
}

void AppendLine(const std::string& line) {
    const fs::path file = CurrentFile();
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    HANDLE h = CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    DWORD written = 0;
    if (!existed || size.QuadPart == 0) {
        static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        WriteFile(h, bom, 3, &written, nullptr);
    }
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);  // one write: lines of several processes do not mix
    GetFileSizeEx(h, &size);
    CloseHandle(h);
    if (static_cast<uint64_t>(size.QuadPart) >= kMaxBytes) Rotate();
}

bool IsDriveAt(const std::wstring& s, size_t i) {
    return i + 2 < s.size() && ((s[i] >= L'A' && s[i] <= L'Z') || (s[i] >= L'a' && s[i] <= L'z')) && s[i + 1] == L':' && s[i + 2] == L'\\' &&
           (i == 0 || !iswalnum(s[i - 1]));
}

// "cannot open D:\some folder\a.zip because ..." -> "cannot open a.zip because ...": a path in a message of a library is cut to its
// last part (the text after the last backslash up to the end of the message, so what follows the name stays). A path ends where
// the next one starts, or at a quote.
std::wstring StripDirectories(const std::wstring& s) {
    std::wstring out;
    size_t i = 0;
    while (i < s.size()) {
        if (!IsDriveAt(s, i)) {
            out += s[i++];
            continue;
        }
        size_t end = i + 3;
        while (end < s.size() && !IsDriveAt(s, end) && s[end] != L'"' && s[end] != L'\'' && s[end] != L'\u300d' && s[end] != L'<' && s[end] != L'>' && s[end] != L'|') ++end;
        const size_t cut = s.rfind(L'\\', end - 1);
        out += s.substr(cut == std::wstring::npos || cut < i ? i : cut + 1, end - (cut == std::wstring::npos || cut < i ? i : cut + 1));
        i = end;
    }
    return out;
}

const wchar_t* LevelText(Level l) {
    switch (l) {
        case Level::Error: return L"ERROR";
        case Level::Warn: return L"WARN ";
        case Level::Info: return L"INFO ";
        default: return L"DEBUG";
    }
}

std::wstring WindowsVersion() {
    using Fn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof(v);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        if (auto fn = reinterpret_cast<Fn>(GetProcAddress(nt, "RtlGetVersion")))
            if (fn(&v) == 0) return L"Windows " + std::to_wstring(v.dwMajorVersion) + L"." + std::to_wstring(v.dwMinorVersion) + L"." + std::to_wstring(v.dwBuildNumber);
    return L"Windows (unknown version)";
}

bool EnvIsDebug() {
    wchar_t v[16];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_LOG", v, static_cast<DWORD>(std::size(v)));
    if (!n || n >= std::size(v)) return false;
    const std::wstring s = v;
    return _wcsicmp(s.c_str(), L"debug") == 0 || s == L"1";
}

}  // namespace

std::wstring FromUtf8(const std::string& s) { return FromUtf8Impl(s); }

fs::path Dir() {
    wchar_t over[MAX_PATH * 2];
    const DWORD o = GetEnvironmentVariableW(L"LYCAN_LOG_DIR", over, static_cast<DWORD>(std::size(over)));
    if (o && o < std::size(over)) return fs::path(std::wstring(over, o));
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    fs::path dir = fs::path(std::wstring(exe, n)).parent_path();
    std::error_code ec;
    if (fs::exists(dir / L"LycanZip.portable", ec)) return dir / L"logs";
    return UserDataDir() / L"logs";
}

fs::path CurrentFile() { return NumberedFile(0); }

void Init(const wchar_t* process) {
    if (g_init.exchange(true)) return;
    {
        std::lock_guard lk(g_m);
        g_process = process ? process : L"?";
    }
    try {
        const Settings s = LoadSettings(DefaultSettingsPath());
        g_detailed = s.debugLog;
        g_full = s.logFullPaths;
    } catch (...) {}
    if (EnvIsDebug()) g_detailed = true;
}

void Configure(bool detailed, bool fullPaths) {
    g_detailed = detailed || EnvIsDebug();
    g_full = fullPaths;
}

bool Detailed() { return g_detailed; }

std::wstring Name(const fs::path& p) {
    if (g_full) return p.wstring();
    const std::wstring n = p.filename().wstring();
    return n.empty() ? p.wstring() : n;
}

void Write(Level level, const std::wstring& message) {
    if (level >= Level::Info && !g_detailed) return;
    try {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t head[96];
        std::wstring proc;
        {
            std::lock_guard lk(g_m);
            proc = g_process;
        }
        swprintf_s(head, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%lu %ls] %ls ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                   t.wMilliseconds, GetCurrentProcessId(), proc.c_str(), LevelText(level));
        std::wstring text = g_full ? message : StripDirectories(message);
        for (auto& c : text)
            if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';  // one message = one line
        if (text.size() > 2000) text.resize(2000);
        const std::string line = ToUtf8(std::wstring(head) + text) + "\r\n";
        std::lock_guard lk(g_m);  // the threads of this process one after the other (the processes are told apart by the one write)
        AppendLine(line);
    } catch (...) {
    }
}

std::wstring RecentLog(size_t maxLines) {
    std::vector<std::string> lines;
    for (int i = 0; i < kKeep && lines.size() < maxLines; ++i) {
        HANDLE h = CreateFileW(NumberedFile(i).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        const uint64_t take = (std::min<uint64_t>)(static_cast<uint64_t>(size.QuadPart), 256 * 1024);  // the end of the file is enough
        LARGE_INTEGER pos;
        pos.QuadPart = size.QuadPart - static_cast<LONGLONG>(take);
        std::string buf(static_cast<size_t>(take), '\0');
        DWORD got = 0;
        if (SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, buf.data(), static_cast<DWORD>(take), &got, nullptr)) buf.resize(got);
        else buf.clear();
        CloseHandle(h);
        if (buf.size() >= 3 && memcmp(buf.data(), "\xEF\xBB\xBF", 3) == 0) buf.erase(0, 3);
        std::vector<std::string> mine;
        size_t p = 0;
        while (p < buf.size()) {
            size_t e = buf.find('\n', p);
            if (e == std::string::npos) e = buf.size();
            std::string l = buf.substr(p, e - p);
            while (!l.empty() && l.back() == '\r') l.pop_back();
            if (!l.empty()) mine.push_back(std::move(l));
            p = e + 1;
        }
        if (static_cast<uint64_t>(size.QuadPart) > take && !mine.empty()) mine.erase(mine.begin());  // the first line may be cut
        // older lines go in front of what is there
        mine.insert(mine.end(), lines.begin(), lines.end());
        lines = std::move(mine);
    }
    if (lines.size() > maxLines) lines.erase(lines.begin(), lines.end() - static_cast<std::ptrdiff_t>(maxLines));
    std::wstring out;
    for (auto const& l : lines) out += FromUtf8Impl(l) + L"\r\n";
    return out;
}

void LogStartup() {
    if (!Detailed()) return;
    std::wstring dll = L"not found";
    try { dll = SevenZipDllPath().wstring(); } catch (...) {}
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::error_code ec;
    const bool portable = fs::exists(fs::path(std::wstring(exe, n)).parent_path() / L"LycanZip.portable", ec);
    Write(Level::Info, std::wstring(L"start: LycanZip " LYCAN_VERSION_WSTRING L", ") + WindowsVersion() + L", 7z.dll " + Name(dll) + L", portable " + (portable ? L"yes" : L"no"));
}

std::wstring DiagnosticText(size_t maxLogLines) {
    std::wstring s = L"LycanZip " LYCAN_VERSION_WSTRING L"\r\n";
    s += WindowsVersion() + L"\r\n";
    try {
        s += L"7z.dll: " + SevenZipDllPath().wstring() + L"\r\n";
    } catch (...) {
        s += L"7z.dll: not found\r\n";
    }
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    std::error_code ec;
    const bool portable = fs::exists(fs::path(std::wstring(exe, n)).parent_path() / L"LycanZip.portable", ec);
    s += std::wstring(L"Portable: ") + (portable ? L"yes" : L"no") + L"\r\n";
    try {
        const Settings st = LoadSettings(DefaultSettingsPath());
        s += L"Settings: " + DefaultSettingsPath().wstring() + L"\r\n";
        s += L"Language: " + st.language + L"\r\n";
    } catch (...) {}
    s += std::wstring(L"Detailed log: ") + (g_detailed ? L"on" : L"off") + L", full paths: " + (g_full ? L"on" : L"off") + L"\r\n";
    s += L"Log folder: " + Dir().wstring() + L"\r\n";
    const std::wstring recent = RecentLog(maxLogLines);
    s += L"\r\n--- the last lines of the log ---\r\n" + (recent.empty() ? std::wstring(L"(empty)\r\n") : recent);
    return s;
}

}  // namespace lycan::diag
