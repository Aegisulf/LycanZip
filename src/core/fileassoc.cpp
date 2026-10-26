#include "lycan/fileassoc.h"

#include "lycan/fileicons.h"
#include "lycan/i18n.h"

#include <windows.h>
#include <appmodel.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>
#include <vector>

namespace lycan {
namespace {

constexpr const wchar_t* kLegacyProgId = L"LycanZip.Archive";  // the one program id of earlier versions: removed when registering again
constexpr const wchar_t* kProbeExt = L".zip";                  // IsFileAssociationsRegistered / RegisteredAssocExe look at this one
constexpr const wchar_t* kAppName = L"LycanZip.exe";
constexpr const wchar_t* kRegisteredName = L"LycanZip";

// Not .iso / .wim (Windows mounts them) and not .jar (a program of its own). The comic book archives (.cbz .cbr .cb7 .cbt) are only offered in
// "Open with" and Default apps: registering does not take them over from the comic reader the user has.
const wchar_t* const kExts[] = {L".zip", L".7z",  L".rar", L".tar", L".gz",  L".tgz", L".xz",  L".txz",
                                L".bz2", L".tbz2", L".tbz", L".zst", L".cab", L".arj", L".lzh", L".z", L".001", L".cbz", L".cbr", L".cb7", L".cbt"};

struct Key {
    HKEY h = nullptr;
    ~Key() { if (h) RegCloseKey(h); }
};

bool SetStr(const std::wstring& sub, const wchar_t* name, const std::wstring& value, std::wstring* error) {
    Key k;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k.h, nullptr);
    if (s == ERROR_SUCCESS)
        s = RegSetValueExW(k.h, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (s != ERROR_SUCCESS) {
        if (error) *error = Tf(L"assoc.writing_registry_failed", s, sub);
        return false;
    }
    return true;
}

// REG_NONE value (for OpenWithProgids)
bool SetNone(const std::wstring& sub, const wchar_t* name, std::wstring* error) {
    Key k;
    LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k.h, nullptr);
    if (s == ERROR_SUCCESS) s = RegSetValueExW(k.h, name, 0, REG_NONE, nullptr, 0);
    if (s != ERROR_SUCCESS) {
        if (error) *error = Tf(L"assoc.writing_registry_failed", s, sub);
        return false;
    }
    return true;
}

std::wstring J(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }

LSTATUS DelTree(const std::wstring& sub) {
    LSTATUS s = RegDeleteTreeW(HKEY_CURRENT_USER, sub.c_str());
    return s == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : s;
}

LSTATUS DelValue(const std::wstring& sub, const wchar_t* name) {
    Key k;
    LSTATUS s = RegOpenKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, KEY_SET_VALUE, &k.h);
    if (s == ERROR_FILE_NOT_FOUND || s == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
    if (s != ERROR_SUCCESS) return s;
    s = RegDeleteValueW(k.h, name);
    return s == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : s;
}

}  // namespace

std::wstring AssociationProgId(const wchar_t* ext) { return std::wstring(L"LycanZip") + ext; }  // ".zip" -> "LycanZip.zip"

std::wstring AssociationTypeName(const wchar_t* ext) {
    if (std::wstring(ext) == L".001") return Tr(L"assoc.lycanzip_split_archive_001");
    std::wstring upper = ext + 1;
    for (auto& c : upper) c = static_cast<wchar_t>(towupper(c));
    if (IconFamilyForExtension(ext) == IconFamily::Comic) return Tf(L"assoc.lycanzip_comic_book_archive", upper);
    return Tf(L"assoc.lycanzip_archive", upper);
}

const wchar_t* const* AssociatedExtensions(size_t* count) {
    if (count) *count = std::size(kExts);
    return kExts;
}

bool RegisterFileAssociations(const FileAssocOptions& opt, std::wstring* error) {
    std::wstring exe = opt.exe.wstring();
    if (exe.empty()) { if (error) *error = Tr(L"assoc.path_lycanzip_exe_was_not_given"); return false; }
    std::wstring quoted = L"\"" + exe + L"\"";
    const std::wstring& cls = opt.classesKey;
    const std::wstring openCmd = quoted + L" \"%1\"";

    // 1. ProgID: one for each extension ("LycanZip ZIP archive", "LycanZip GZ archive" ...). The "Type" column of Explorer shows this name, so it must tell the kinds apart;
    //    the icon follows the icon family (ZIP, 7Z, RAR, TAR, other) in the resources of LycanZip.exe (see fileicons.h). The name uses the language of the system now.
    for (auto e : kExts) {
        std::wstring prog = J(cls, AssociationProgId(e));
        if (!SetStr(prog, nullptr, AssociationTypeName(e), error)) return false;
        if (!SetStr(J(prog, L"DefaultIcon"), nullptr, quoted + L",-" + std::to_wstring(IconResourceId(IconFamilyForExtension(e))), error)) return false;
        if (!SetStr(J(prog, L"shell\\open"), L"FriendlyAppName", L"LycanZip", error)) return false;
        if (!SetStr(J(prog, L"shell\\open\\command"), nullptr, openCmd, error)) return false;
    }
    DelTree(J(cls, kLegacyProgId));

    // 2. Applications\\LycanZip.exe: makes the program appear in the "Open with" list
    std::wstring app = J(cls, std::wstring(L"Applications\\") + kAppName);
    if (!SetStr(app, L"FriendlyAppName", L"LycanZip", error)) return false;
    if (!SetStr(J(app, L"shell\\open\\command"), nullptr, openCmd, error)) return false;
    for (auto e : kExts)
        if (!SetStr(J(app, L"SupportedTypes"), e, L"", error)) return false;

    // 3. Capabilities + RegisteredApplications: makes the program appear in "Settings > Default apps"
    std::wstring caps = J(opt.softwareKey, L"LycanZip\\Capabilities");
    if (!SetStr(caps, L"ApplicationName", L"LycanZip", error)) return false;
    if (!SetStr(caps, L"ApplicationDescription", Tr(L"assoc.open_source_ad_free_lightweight"), error)) return false;
    for (auto e : kExts)
        if (!SetStr(J(caps, L"FileAssociations"), e, AssociationProgId(e), error)) return false;
    if (!SetStr(J(opt.softwareKey, L"RegisteredApplications"), kRegisteredName, caps, error)) return false;

    // 4. OpenWithProgids of each extension (REG_NONE): adds the program to the "Open with" list of that extension
    for (auto e : kExts) {
        const std::wstring owp = J(J(cls, e), L"OpenWithProgids");
        if (!SetNone(owp, AssociationProgId(e).c_str(), error)) return false;
        DelValue(owp, kLegacyProgId);  // earlier versions used the one program id for everything
    }

    if (opt.notifyShell) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return true;
}

bool UpdateAssociationNames(const FileAssocOptions& opt) {
    if (!IsFileAssociationsRegistered(opt)) return false;
    bool changed = false;
    for (auto e : kExts) {
        const std::wstring prog = J(opt.classesKey, AssociationProgId(e));
        Key k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, prog.c_str(), 0, KEY_READ | KEY_SET_VALUE, &k.h) != ERROR_SUCCESS) continue;
        wchar_t cur[256];
        DWORD n = sizeof(cur);
        const std::wstring want = AssociationTypeName(e);
        if (RegGetValueW(k.h, nullptr, nullptr, RRF_RT_REG_SZ, nullptr, cur, &n) == ERROR_SUCCESS && want == cur) continue;
        if (RegSetValueExW(k.h, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(want.c_str()), static_cast<DWORD>((want.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS) changed = true;
    }
    if (changed && opt.notifyShell) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return changed;
}

bool UnregisterFileAssociations(const FileAssocOptions& opt, std::wstring* error) {
    const std::wstring& cls = opt.classesKey;
    bool ok = true;
    auto check = [&](LSTATUS s, const std::wstring& what) {
        if (s != ERROR_SUCCESS) {
            ok = false;
            if (error) *error = Tf(L"assoc.could_not_remove", what, s);
        }
    };
    for (auto e : kExts) check(DelTree(J(cls, AssociationProgId(e))), AssociationProgId(e));
    check(DelTree(J(cls, kLegacyProgId)), kLegacyProgId);
    check(DelTree(J(cls, std::wstring(L"Applications\\") + kAppName)), kAppName);
    check(DelTree(J(opt.softwareKey, L"LycanZip")), L"Capabilities");
    check(DelValue(J(opt.softwareKey, L"RegisteredApplications"), kRegisteredName), L"RegisteredApplications");
    for (auto e : kExts) {
        const std::wstring owp = J(J(cls, e), L"OpenWithProgids");
        check(DelValue(owp, AssociationProgId(e).c_str()), e);
        check(DelValue(owp, kLegacyProgId), e);
    }
    if (opt.notifyShell) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool IsFileAssociationsRegistered(const FileAssocOptions& opt) {
    Key k;
    return RegOpenKeyExW(HKEY_CURRENT_USER, J(opt.softwareKey, L"LycanZip\\Capabilities").c_str(), 0, KEY_READ, &k.h) == ERROR_SUCCESS &&
           RegOpenKeyExW(HKEY_CURRENT_USER, J(opt.classesKey, AssociationProgId(kProbeExt)).c_str(), 0, KEY_READ, &k.h) == ERROR_SUCCESS;
}

std::filesystem::path RegisteredAssocExe(const FileAssocOptions& opt) {
    wchar_t buf[MAX_PATH * 2 + 16];
    DWORD n = sizeof(buf);
    const std::wstring key = J(J(J(opt.classesKey, AssociationProgId(kProbeExt)), L"shell\\open"), L"command");
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS) return {};
    std::wstring cmd = buf;  // "C:\\dir\\LycanZip.exe" "%1"
    if (cmd.empty() || cmd[0] != L'"') return {};
    const auto end = cmd.find(L'"', 1);
    return end == std::wstring::npos ? std::filesystem::path() : std::filesystem::path(cmd.substr(1, end - 1));
}

static std::wstring Query(ASSOCSTR what, const wchar_t* ext) {
    wchar_t buf[1024];
    DWORD n = static_cast<DWORD>(std::size(buf));
    if (FAILED(AssocQueryStringW(ASSOCF_NONE, what, ext, L"open", buf, &n))) return {};
    return buf;
}

std::wstring CurrentDefaultAppName(const wchar_t* ext) {
    // With no association, Windows answers with "Choose an app" (OpenWith.exe); it is not "the default program is missing"
    std::wstring exe = Query(ASSOCSTR_EXECUTABLE, ext);
    if (!exe.empty() && _wcsicmp(PathFindFileNameW(exe.c_str()), L"OpenWith.exe") == 0) return {};
    return Query(ASSOCSTR_FRIENDLYAPPNAME, ext);
}

bool IsLycanZipDefaultFor(const wchar_t* ext) {
    std::wstring exe = Query(ASSOCSTR_EXECUTABLE, ext);
    if (exe.empty()) return false;
    const wchar_t* name = PathFindFileNameW(exe.c_str());
    return _wcsicmp(name, kAppName) == 0;
}

bool OpenDefaultAppsSettings() {
    // Windows 11 jumps straight to LycanZip; Windows 10 opens "Default apps". A package (the Store version) is named by its app user model id.
    std::wstring uri = L"ms-settings:defaultapps?registeredAppUser=LycanZip";
    UINT32 len = 0;
    if (GetCurrentApplicationUserModelId(&len, nullptr) == ERROR_INSUFFICIENT_BUFFER && len > 0) {
        std::wstring aumid(len, L'\0');
        if (GetCurrentApplicationUserModelId(&len, aumid.data()) == ERROR_SUCCESS) {
            aumid.resize(wcslen(aumid.c_str()));
            uri = L"ms-settings:defaultapps?registeredAUMID=" + aumid;
        }
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(r) > 32;
}

}  // namespace lycan
