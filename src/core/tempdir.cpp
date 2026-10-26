#include "lycan/tempdir.h"

#include <windows.h>

#include <algorithm>

#include "lycan/error.h"
#include "lycan/testhooks.h"

namespace fs = std::filesystem;

namespace lycan {

std::string ToUtf8(std::wstring_view w);  // util.cpp

namespace {
std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
}  // namespace

fs::path ScratchDirectory(const fs::path& custom) {
    if (!custom.empty()) {
        std::error_code ec;
        fs::create_directories(custom, ec);
        if (fs::is_directory(custom, ec)) return custom;
    }
    wchar_t tmp[MAX_PATH + 2];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    if (n == 0 || n > MAX_PATH) return fs::path{};
    return fs::path(std::wstring(tmp, n));
}

bool ScratchFreeBytes(const fs::path& dir, uint64_t& freeBytes) {
    static const uint64_t fromEnv = [] {
        wchar_t buf[32];
        return GetEnvironmentVariableW(L"LYCAN_TEST_FREE_SPACE", buf, 32) > 0 ? _wcstoui64(buf, nullptr, 10) : uint64_t{0};
    }();
    if (fromEnv) {
        freeBytes = fromEnv;
        return true;
    }
    return QueryFreeSpace(dir.c_str(), freeBytes);
}

std::vector<LocalVolume> LocalFixedVolumes() {
    std::vector<LocalVolume> v;
    const DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        const std::wstring root = std::wstring(1, static_cast<wchar_t>(L'A' + i)) + L":\\";
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED) continue;
        ULARGE_INTEGER avail{};
        if (!GetDiskFreeSpaceExW(root.c_str(), &avail, nullptr, nullptr)) continue;
        v.push_back({root, avail.QuadPart});
    }
    std::sort(v.begin(), v.end(), [](const LocalVolume& a, const LocalVolume& b) { return a.freeBytes > b.freeBytes; });
    return v;
}

bool IsSlowLocation(const fs::path& dir) {
    const std::wstring s = dir.wstring();
    if (s.rfind(L"\\\\", 0) == 0 && s.rfind(L"\\?\\", 0) != 0) return true;  // UNC share
    std::wstring root = dir.root_path().wstring();
    if (root.empty()) return false;
    if (root.rfind(L"\\\\", 0) == 0) return root.rfind(L"\\?\\", 0) != 0;
    const UINT t = GetDriveTypeW(root.c_str());
    return t == DRIVE_REMOTE || t == DRIVE_REMOVABLE || t == DRIVE_CDROM;
}

Message MakeTempSpaceMessage(uint64_t need, uint64_t have, const fs::path& dir) {
    // args 0..2 are shown ({0} MB needed, {1} MB left, {2} the folder), 3 and 4 give the exact numbers back to the program
    return Message::Make(need ? L"core.temp_space_need" : L"core.temp_space_full", (need + (1u << 20) - 1) >> 20, have >> 20, dir.wstring(), need, have);
}

bool ParseTempSpaceMessage(const Message& message, uint64_t& need, uint64_t& have, std::wstring& dir) {
    if ((message.id != L"core.temp_space_need" && message.id != L"core.temp_space_full") || message.args.size() < 5) return false;
    dir = message.args[2];
    need = _wcstoui64(message.args[3].c_str(), nullptr, 10);
    have = _wcstoui64(message.args[4].c_str(), nullptr, 10);
    return true;
}

}  // namespace lycan
