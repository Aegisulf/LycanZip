#include "lycan/shellreg.h"

#include <windows.h>
#include <shlobj.h>

#include <cwctype>
#include <fstream>
#include <string>
#include <vector>

#include "lycan/version.h"

namespace lycan {
namespace {

constexpr const wchar_t* kFilesVerb = L"LycanZipFiles";
constexpr const wchar_t* kArchiveVerb = L"LycanZipArchive";
constexpr const wchar_t* kOldVerb = L"LycanZip";  // the registry-only menu of earlier versions
constexpr const wchar_t* kVerifyVerb = L"LycanZipVerify";
// The menu of a right-button drag: where files are dropped (a folder, a drive, the desktop is a folder)
constexpr const wchar_t* kDragHandlerName = L"LycanZip";
constexpr const wchar_t* const kDragTargets[] = {L"Directory", L"Drive", L"Folder"};

// Archive extensions: these files get the extraction menu. Keep in step with shellext/shellext.cpp and the package manifests of
// the release (the release flow checks that they match).
const wchar_t* const kArchiveExts[] = {L".zip", L".7z",  L".rar", L".tar", L".gz",  L".tgz", L".xz",  L".txz",
                                       L".bz2", L".tbz2", L".tbz", L".iso", L".cab", L".arj", L".lzh", L".z",
                                       L".wim", L".zst", L".001", L".cbz", L".cbr", L".cb7", L".cbt"};

// Checksum files get "Verify the checksum file". Keep in step with shellext/shellext.cpp and the package manifests of the
// release (the release flow checks that they match).
const wchar_t* const kSumExts[] = {L".sha256", L".sha1", L".sha384", L".sha512", L".md5", L".sfv"};

struct Key {
    HKEY h = nullptr;
    ~Key() { if (h) RegCloseKey(h); }
};

bool SetValue(const std::wstring& subKey, const wchar_t* name, const std::wstring& value, std::wstring* error) {
    Key k;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, subKey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k.h, nullptr);
    if (s == ERROR_SUCCESS)
        s = RegSetValueExW(k.h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (s != ERROR_SUCCESS) {
        if (error) *error = L"Could not write to the registry (" + std::to_wstring(s) + L"): HKCU\\" + subKey;
        return false;
    }
    return true;
}

LSTATUS DeleteTree(const std::wstring& subKey) {
    LSTATUS s = RegDeleteTreeW(HKEY_CURRENT_USER, subKey.c_str());
    return s == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : s;
}

std::wstring Join(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }

// A verb that hands the menu to the COM class: no sub keys, Explorer asks the class for the title, icon and entries
bool WriteVerb(const std::wstring& verbKey, const wchar_t* clsid, std::wstring* error) {
    return SetValue(verbKey, L"ExplorerCommandHandler", clsid, error);
}

bool WriteClass(const std::wstring& root, const wchar_t* clsid, const wchar_t* name, const std::wstring& dll, std::wstring* error) {
    const std::wstring key = Join(Join(root, L"CLSID"), clsid);
    return SetValue(key, nullptr, name, error) && SetValue(Join(key, L"InprocServer32"), nullptr, dll, error) &&
           SetValue(Join(key, L"InprocServer32"), L"ThreadingModel", L"Apartment", error);
}

std::vector<std::wstring> AllKeys(const std::wstring& root) {
    std::vector<std::wstring> keys = {Join(root, L"*\\shell\\") + kFilesVerb, Join(root, L"Directory\\shell\\") + kFilesVerb,
                                      Join(Join(root, L"CLSID"), kClassicFilesClsid), Join(Join(root, L"CLSID"), kClassicArchiveClsid),
                                      Join(Join(root, L"CLSID"), kDragDropClsid), Join(Join(root, L"CLSID"), kClassicVerifyClsid),
                                      // earlier versions
                                      Join(root, L"*\\shell\\") + kOldVerb, Join(root, L"Directory\\shell\\") + kOldVerb};
    for (auto ext : kSumExts) keys.push_back(Join(root, std::wstring(L"SystemFileAssociations\\") + ext + L"\\shell\\" + kVerifyVerb));
    for (auto cls : kDragTargets) keys.push_back(Join(root, std::wstring(cls) + L"\\shellex\\DragDropHandlers\\" + kDragHandlerName));
    for (auto ext : kArchiveExts) {
        keys.push_back(Join(root, std::wstring(L"SystemFileAssociations\\") + ext + L"\\shell\\" + kArchiveVerb));
        keys.push_back(Join(root, std::wstring(L"SystemFileAssociations\\") + ext + L"\\shell\\" + kOldVerb));
    }
    return keys;
}

}  // namespace

bool RegisterShellMenu(const ShellRegOptions& opt, std::wstring* error) {
    if (opt.exe.empty()) { if (error) *error = L"The path of LycanZip.exe is not known"; return false; }
    const std::filesystem::path dll = !opt.dll.empty() ? opt.dll : ShellDllPath(EnsureShellHome(opt.exe.parent_path()));
    std::error_code ec;
    if (!std::filesystem::exists(dll, ec)) { if (error) *error = dll.filename().wstring() + L" was not found: " + dll.wstring(); return false; }
    const std::wstring& root = opt.classesKey;

    // start clean: entries of earlier versions would show a second menu
    for (auto& k : AllKeys(root)) DeleteTree(k);

    if (!WriteClass(root, kClassicFilesClsid, L"LycanZip context menu (files and folders)", dll.wstring(), error)) return false;
    if (!WriteClass(root, kClassicArchiveClsid, L"LycanZip context menu (archives)", dll.wstring(), error)) return false;
    if (!WriteClass(root, kClassicVerifyClsid, L"LycanZip context menu (checksum files)", dll.wstring(), error)) return false;
    for (auto ext : kSumExts)
        if (!WriteVerb(Join(root, std::wstring(L"SystemFileAssociations\\") + ext + L"\\shell\\" + kVerifyVerb), kClassicVerifyClsid, error)) return false;
    if (!WriteClass(root, kDragDropClsid, L"LycanZip right-drag menu", dll.wstring(), error)) return false;
    for (auto cls : kDragTargets)
        if (!SetValue(Join(root, std::wstring(cls) + L"\\shellex\\DragDropHandlers\\" + kDragHandlerName), nullptr, kDragDropClsid, error)) return false;
    if (!WriteVerb(Join(root, L"*\\shell\\") + kFilesVerb, kClassicFilesClsid, error)) return false;
    if (!WriteVerb(Join(root, L"Directory\\shell\\") + kFilesVerb, kClassicFilesClsid, error)) return false;
    for (auto ext : kArchiveExts)
        if (!WriteVerb(Join(root, std::wstring(L"SystemFileAssociations\\") + ext + L"\\shell\\" + kArchiveVerb), kClassicArchiveClsid, error))
            return false;
    if (root == L"Software\\Classes") SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool UnregisterShellMenu(const ShellRegOptions& opt, std::wstring* error) {
    bool ok = true;
    for (auto& k : AllKeys(opt.classesKey)) {
        LSTATUS s = DeleteTree(k);
        if (s != ERROR_SUCCESS) {
            ok = false;
            if (error) *error = L"Could not delete HKCU\\" + k + L" (" + std::to_wstring(s) + L")";
        }
    }
    if (opt.classesKey == L"Software\\Classes") SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

std::wstring ShellDllFileName() { return L"LycanZipShell-" LYCAN_VERSION_WSTRING L".dll"; }

std::filesystem::path ShellDllPath(const std::filesystem::path& dir) { return dir / ShellDllFileName(); }

bool IsInstalledLayout(const std::filesystem::path& exeDirIn) {
    std::filesystem::path exeDir = exeDirIn;
    if (!exeDir.empty() && !exeDir.has_filename()) exeDir = exeDir.parent_path();   // a trailing separator
    std::wstring name = exeDir.filename().wstring();
    for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
    std::error_code ec;
    return name == L"current" && std::filesystem::exists(exeDir.parent_path() / L"Update.exe", ec);
}

std::filesystem::path ShellHome(const std::filesystem::path& exeDir, const std::filesystem::path& dataRoot) {
    if (!IsInstalledLayout(exeDir)) return exeDir;
    if (!dataRoot.empty()) return dataRoot / L"shell";
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n >= std::size(buf)) return exeDir;   // no place to keep it: leave it in the folder of the exe
    return std::filesystem::path(std::wstring(buf, n)) / L"LycanZip" / L"shell";
}

std::filesystem::path EnsureShellHome(const std::filesystem::path& exeDir, const std::filesystem::path& dataRoot) {
    namespace fs = std::filesystem;
    const fs::path home = ShellHome(exeDir, dataRoot);
    if (home == exeDir) return home;
    std::error_code ec;
    fs::create_directories(home, ec);
    for (const fs::path& name : {fs::path(ShellDllFileName()), fs::path(L"LycanZipShell.msix")}) {
        const fs::path from = exeDir / name, to = home / name;
        if (!fs::exists(from, ec)) continue;
        if (fs::exists(to, ec) && fs::file_size(to, ec) == fs::file_size(from, ec) && fs::last_write_time(to, ec) >= fs::last_write_time(from, ec)) continue;
        fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);   // a DLL that is loaded cannot be replaced: the registration reports it
    }
    // the DLL finds the exe (its icons and the program the entries start) through this file: the DLL is not in the folder of the exe
    const auto u8 = (exeDir / L"LycanZip.exe").u8string();
    const std::string exeUtf8(reinterpret_cast<const char*>(u8.data()), u8.size());
    std::ofstream(home / L"LycanZip.exepath", std::ios::binary | std::ios::trunc) << exeUtf8;
    return home;
}

int RemoveOtherShellDlls(const std::filesystem::path& dir, const std::filesystem::path& keep) {
    namespace fs = std::filesystem;
    int removed = 0;
    std::error_code ec;
    std::vector<fs::path> found;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::wstring name = it->path().filename().wstring();
        for (auto& c : name) c = static_cast<wchar_t>(std::towlower(c));
        const bool ours = name == L"lycanzipshell.dll" || (name.rfind(L"lycanzipshell-", 0) == 0 && name.size() > 18 && name.compare(name.size() - 4, 4, L".dll") == 0);
        if (ours && it->is_regular_file(ec)) found.push_back(it->path());
    }
    for (const auto& p : found) {
        std::error_code same;
        if (fs::equivalent(p, keep, same)) continue;
        if (fs::remove(p, ec) && !ec) ++removed;   // a loaded DLL cannot be deleted: it stays
    }
    return removed;
}

std::filesystem::path RegisteredShellDll(const ShellRegOptions& opt) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = sizeof(buf);
    const std::wstring key = Join(Join(Join(opt.classesKey, L"CLSID"), kClassicFilesClsid), L"InprocServer32");
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return {};
    return buf;
}

bool IsShellMenuRegistered(const ShellRegOptions& opt) {
    Key k;
    return RegOpenKeyExW(HKEY_CURRENT_USER, (Join(opt.classesKey, L"*\\shell\\") + kFilesVerb).c_str(), 0, KEY_READ, &k.h) == ERROR_SUCCESS;
}

}  // namespace lycan
