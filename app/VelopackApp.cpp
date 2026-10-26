#include "pch.h"
// The installed copy of LycanZip (made by the installer, see the design notes "Installer and automatic update"): the installer and the update program
// start LycanZip.exe with special arguments (--veloapp-install, --veloapp-uninstall ...). Velopack::VelopackApp::Run answers them and ends the
// process; on a normal start it returns at once. It is called while the program loads (static initialisation), before anything else starts.
// Without third_party\velopack (scripts\fetch-velopack.ps1) the program is built without it and has no installed form.
#include "InstalledUpdate.h"
#ifdef LYCAN_VELOPACK
#include <Velopack.hpp>

#include <filesystem>

#include "Win11Menu.h"
#include "lycan/fileassoc.h"
#include "lycan/shellreg.h"

namespace {

namespace fs = std::filesystem;

bool SamePath(fs::path const& a, fs::path const& b) {
    std::error_code ec;
    if (a.empty() || b.empty()) return false;
    return fs::equivalent(a, b, ec) || _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
}

constexpr wchar_t kRunOnceKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";
constexpr wchar_t kCleanupValue[] = L"LycanZipShellCleanup";

std::wstring TestRegistryRoot() {
    wchar_t buf[256];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_TEST_REG_ROOT", buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring{};
}

// The uninstaller is about to delete the program: take away what it registered (only what points to this copy, so another copy that the
// user registered stays), and the menu DLL that was kept outside of the folder that is deleted.
void BeforeUninstall(void*, const char*) {
    try {
        wchar_t buf[MAX_PATH * 2];
        const fs::path exe = std::wstring(buf, GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf))));
        const fs::path home = lycan::ShellHome(exe.parent_path());
        const std::wstring root = TestRegistryRoot();

        lycan::ShellRegOptions shell;
        shell.exe = exe;
        lycan::FileAssocOptions assoc;
        assoc.exe = exe;
        if (!root.empty()) { shell.classesKey = root + L"\\Classes"; assoc.classesKey = root + L"\\Classes"; assoc.softwareKey = root + L"\\Software"; assoc.notifyShell = false; }

        if (lycan::IsShellMenuRegistered(shell) && SamePath(lycan::RegisteredShellDll(shell).parent_path(), home)) lycan::UnregisterShellMenu(shell);
        if (lycan::IsFileAssociationsRegistered(assoc) && SamePath(lycan::RegisteredAssocExe(assoc), exe)) lycan::UnregisterFileAssociations(assoc);
        if (root.empty()) {   // (the package is not part of the test registry key)
            winrt::init_apartment();
            const auto w11 = lycan_app::QueryWin11MenuRegistration();
            if (w11.found && SamePath(fs::path(w11.location), home)) lycan_app::UnregisterWin11MenuAsync().get();
        }
        if (home != exe.parent_path()) {
            std::error_code ec;
            fs::remove_all(home, ec);
            // Explorer keeps the menu DLL loaded once the menu was used, so the folder cannot go now. Nothing of LycanZip runs after this:
            // the folder is removed at the next sign-in instead (the menu is not registered any more then, so nothing loads the DLL)
            if (fs::exists(home, ec) && root.empty()) {
                const std::wstring cmd = L"cmd.exe /d /c rd /s /q \"" + home.wstring() + L"\"";
                RegSetKeyValueW(HKEY_CURRENT_USER, kRunOnceKey, kCleanupValue, REG_SZ, cmd.c_str(), static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            }
        }
    } catch (...) {}   // the uninstall goes on whatever happens here
}

// Installed again before that sign-in: the folder belongs to this copy now, so its removal is called off
void AfterInstall(void*, const char*) {
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunOnceKey, kCleanupValue);
}

struct VelopackStart {
    VelopackStart() {
        if (!lycan_app::VelopackPresent()) return;
        try {
            Velopack::VelopackApp::Build().OnBeforeUninstall(&BeforeUninstall).OnAfterInstall(&AfterInstall).Run();
        } catch (...) {}   // not installed by the installer (a zip, Store, development build): nothing to answer
    }
} g_velopackStart;

}  // namespace

namespace lycan_app {
bool VelopackPresent() {
    static const bool present = [] {
        wchar_t buf[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
        return n > 0 && n < std::size(buf) && std::filesystem::exists(std::filesystem::path(std::wstring(buf, n)).parent_path() / L"velopack_libc.dll");
    }();
    return present;
}
}  // namespace lycan_app
#else
namespace lycan_app {
bool VelopackPresent() { return false; }
}  // namespace lycan_app
#endif
