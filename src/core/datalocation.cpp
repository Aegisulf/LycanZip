#include "lycan/datalocation.h"

#include <windows.h>
#include <appmodel.h>
#include <shlobj.h>

#include <fstream>
#include <mutex>

namespace fs = std::filesystem;

namespace lycan {

namespace {

constexpr const wchar_t* kFlag = L"LycanZip.portable";
constexpr const wchar_t* kFiles[] = {L"settings.ini", L"presets.ini"};

fs::path ExeDirNow() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return fs::path(std::wstring(exe, n)).parent_path();
}

fs::path LocalAppDataNow(const fs::path& fallback) {
    PWSTR local = nullptr;
    fs::path base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) base = local;
    CoTaskMemFree(local);
    return base.empty() ? fallback : base;
}

std::wstring CurrentPackageFamily() {
    UINT32 len = 0;
    if (GetCurrentPackageFamilyName(&len, nullptr) != ERROR_INSUFFICIENT_BUFFER || len == 0) return {};
    std::wstring s(len, L'\0');
    if (GetCurrentPackageFamilyName(&len, s.data()) != ERROR_SUCCESS) return {};
    s.resize(wcslen(s.c_str()));
    return s;
}

}  // namespace

fs::path UserDataDirFor(const fs::path& localAppData, const fs::path& packageDir) {
    return packageDir.empty() ? localAppData / L"LycanZip" : packageDir / L"LocalState";
}

bool ImportIntoPackageData(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    const fs::path marker = to / L"imported";
    if (fs::exists(marker, ec) || fs::exists(to / L"settings.ini", ec)) return false;
    fs::create_directories(to, ec);
    bool copied = false;
    for (const wchar_t* name : kFiles)
        if (fs::exists(from / name, ec)) {
            fs::copy_file(from / name, to / name, fs::copy_options::skip_existing, ec);
            copied = copied || !ec;
        }
    // Only when something was copied: with nothing to take over, the check runs again at the next start, until the package has saved
    // its own settings.ini (an installed copy set up later is then taken over once)
    if (copied)
        std::ofstream(marker, std::ios::binary | std::ios::trunc) << "The settings and presets of %LOCALAPPDATA%\\LycanZip were copied here once.\n";
    return copied;
}

fs::path UserDataDir() {
    const fs::path local = LocalAppDataNow(ExeDirNow());
    const std::wstring family = CurrentPackageFamily();
    if (family.empty()) return UserDataDirFor(local, {});
    const fs::path dir = UserDataDirFor(local, local / L"Packages" / family);
    static std::once_flag once;
    std::call_once(once, [&] { ImportIntoPackageData(UserDataDirFor(local, {}), dir); });
    return dir;
}

bool IsStorePackageInstalled() {
    wchar_t over[8];
    if (const DWORD o = GetEnvironmentVariableW(L"LYCAN_TEST_STORE_INSTALLED", over, static_cast<DWORD>(std::size(over))); o && o < std::size(over)) return over[0] == L'1';
    UINT32 count = 0, len = 0;
    const LONG r = GetPackagesByPackageFamily(kStorePackageFamily, &count, nullptr, &len, nullptr);
    return (r == ERROR_SUCCESS || r == ERROR_INSUFFICIENT_BUFFER) && count > 0;
}

DataLocation GetDataLocation() {
    DataLocation d;
    d.exeDir = ExeDirNow();
    d.userDir = UserDataDir();
    std::error_code ec;
    d.portable = fs::exists(d.exeDir / kFlag, ec);
    d.dataDir = d.portable ? d.exeDir : d.userDir;
    UINT32 len = 0;
    d.packaged = GetCurrentPackageFullName(&len, nullptr) != APPMODEL_ERROR_NO_PACKAGE;
    return d;
}

bool DirWritable(const fs::path& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path probe = dir / (L"lycanzip-write-test-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

SwitchStatus SwitchDataLocation(const fs::path& exeDir, const fs::path& userDir, bool toPortable) {
    std::error_code ec;
    const bool portableNow = fs::exists(exeDir / kFlag, ec);
    if (portableNow == toPortable) return SwitchStatus::Ok;
    const fs::path from = toPortable ? userDir : exeDir;
    const fs::path to = toPortable ? exeDir : userDir;
    if (!DirWritable(to)) return SwitchStatus::NotWritable;
    for (const wchar_t* name : kFiles) {
        const fs::path src = from / name, dst = to / name, tmp = to / (std::wstring(name) + L".new");
        if (!fs::exists(src, ec)) continue;
        fs::remove(tmp, ec);
        fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec) { fs::remove(tmp, ec); return SwitchStatus::CopyFailed; }
        if (fs::exists(dst, ec)) {
            const fs::path bak = to / (std::wstring(name) + L".bak");
            fs::remove(bak, ec);
            fs::rename(dst, bak, ec);
            if (ec) { fs::remove(tmp, ec); return SwitchStatus::CopyFailed; }
        }
        fs::rename(tmp, dst, ec);
        if (ec) { fs::remove(tmp, ec); return SwitchStatus::CopyFailed; }
    }
    if (toPortable) {
        std::ofstream f(exeDir / kFlag, std::ios::binary | std::ios::trunc);
        f.close();
        if (!fs::exists(exeDir / kFlag, ec)) return SwitchStatus::FlagFailed;
    } else {
        fs::remove(exeDir / kFlag, ec);
        if (fs::exists(exeDir / kFlag, ec)) return SwitchStatus::FlagFailed;
    }
    return SwitchStatus::Ok;
}

}  // namespace lycan
