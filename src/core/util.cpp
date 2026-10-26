#include <windows.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "lycan/error.h"
#include "lycan/tempdir.h"
#include "lycan/testhooks.h"

namespace lycan {

std::string ToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring WideFromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

namespace {
std::atomic<uint64_t> g_failAfter{UINT64_MAX};  // UINT64_MAX = off
std::atomic<uint64_t> g_written{0};
std::atomic<bool> g_envRead{false};
}  // namespace

void SetTestWriteFault(uint64_t afterBytes) {
    g_envRead = true;
    g_written = 0;
    g_failAfter = afterBytes ? afterBytes : UINT64_MAX;
}

bool TestWriteShouldFail(uint64_t size) {
    if (!g_envRead.exchange(true)) {
        wchar_t buf[32];
        if (GetEnvironmentVariableW(L"LYCAN_TEST_FAIL_WRITE_AFTER", buf, 32) > 0) {
            const uint64_t v = _wcstoui64(buf, nullptr, 10);
            if (v) g_failAfter = v;
        }
    }
    const uint64_t limit = g_failAfter.load(std::memory_order_relaxed);
    if (limit == UINT64_MAX) return false;
    return g_written.fetch_add(size) + size > limit;
}

namespace {
std::atomic<uint64_t> g_testFree{0};
}

void SetTestFreeSpace(uint64_t bytes) { g_testFree = bytes; }

bool QueryFreeSpace(const wchar_t* path, uint64_t& freeBytes) {
    if (const uint64_t t = g_testFree.load()) { freeBytes = t; return true; }
    // The folder may not exist yet: walk up to the first existing parent.
    std::wstring p = path;
    while (!p.empty()) {
        ULARGE_INTEGER avail{};
        if (GetDiskFreeSpaceExW(p.c_str(), &avail, nullptr, nullptr)) { freeBytes = avail.QuadPart; return true; }
        const size_t cut = p.find_last_of(L"\\/");
        if (cut == std::wstring::npos || cut == 0) break;
        p.resize(cut);
    }
    return false;
}

// A new, uniquely named scratch file in %TEMP% that is deleted when the last handle closes. Several jobs can
// create one at the same time (also within one process), so the name carries a counter and a name that
// already exists is skipped.
HANDLE CreateScratchFile(const wchar_t* prefix, const std::filesystem::path& custom) {
    std::wstring dir = ScratchDirectory(custom).wstring();
    if (dir.empty()) return INVALID_HANDLE_VALUE;
    if (dir.back() != L'\\' && dir.back() != L'/') dir += L'\\';
    static std::atomic<uint64_t> counter{0};
    for (int attempt = 0; attempt < 1000; ++attempt) {
        std::wstring name = dir + prefix + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()) +
                            L"_" + std::to_wstring(counter++) + L".tmp";
        HANDLE h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) return h;
    }
    return INVALID_HANDLE_VALUE;
}

}  // namespace lycan
