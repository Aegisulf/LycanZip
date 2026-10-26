#include "pch.h"
#include "Win11Menu.h"

#include <winrt/Windows.Management.Deployment.h>

#include "lycan/i18n.h"
#include "lycan/shellreg.h"

namespace lycan_app {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Management::Deployment;
namespace fs = std::filesystem;

namespace {
constexpr const wchar_t* kPackageName = L"LycanZip.ShellExtension";  // the Identity Name of the Windows 11 menu package (made by the release flow)

fs::path DevManifest() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_WIN11_MENU_MANIFEST", buf, static_cast<DWORD>(std::size(buf)));
    return n > 0 && n < std::size(buf) ? fs::path(std::wstring(buf, n)) : fs::path();
}

hstring Describe(DeploymentResult const& r) {
    std::wstring t{r.ErrorText()};
    if (t.empty()) {
        wchar_t code[16];
        swprintf_s(code, L"0x%08X", static_cast<unsigned>(r.ExtendedErrorCode().value));
        t = lycan::Tf(L"win11.error_code", code);
    }
    return hstring{t};
}
}  // namespace

bool IsWindows11() {
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{sizeof(v)};
    return fn && fn(&v) == 0 && v.dwMajorVersion == 10 && v.dwBuildNumber >= 22000;
}

bool Win11MenuPackageAvailable(fs::path const& installDir) {
    std::error_code ec;
    return fs::exists(installDir / L"LycanZipShell.msix", ec) || (!DevManifest().empty() && fs::exists(DevManifest(), ec));
}

Win11MenuState QueryWin11Menu() {
    if (!IsWindows11()) return Win11MenuState::Unsupported;
    try {
        PackageManager pm;
        for (auto const& p : pm.FindPackagesForUser(L""))
            if (p.Id().Name() == kPackageName) return Win11MenuState::Registered;
    } catch (...) {}
    return Win11MenuState::NotRegistered;
}

std::wstring Win11MenuLocation() {
    try {
        PackageManager pm;
        for (auto const& p : pm.FindPackagesForUser(L""))
            if (p.Id().Name() == kPackageName) return std::wstring{p.EffectiveExternalPath()};
    } catch (...) {}
    return {};
}

Win11MenuRegistration QueryWin11MenuRegistration() {
    Win11MenuRegistration r;
    try {
        PackageManager pm;
        for (auto const& p : pm.FindPackagesForUser(L"")) {
            if (p.Id().Name() != kPackageName) continue;
            r.found = true;
            r.location = std::wstring{p.EffectiveExternalPath()};
            r.major = p.Id().Version().Major;
            r.minor = p.Id().Version().Minor;
            r.build = p.Id().Version().Build;
            break;
        }
    } catch (...) {}
    return r;
}

IAsyncOperation<hstring> RegisterWin11MenuAsync(fs::path installDir) {
    try {
        if (!IsWindows11()) co_return hstring{lycan::Tr(L"win11.not_windows_11_new_style_context")};
        std::error_code ec;
        if (!fs::exists(lycan::ShellDllPath(installDir), ec)) co_return hstring{lycan::Tf(L"win11.was_not_found", lycan::ShellDllPath(installDir).wstring())};
        PackageManager pm;
        Uri location{hstring{installDir.wstring()}};
        const fs::path msix = installDir / L"LycanZipShell.msix", manifest = DevManifest();
        if (fs::exists(msix, ec)) {
            AddPackageOptions options;
            options.ExternalLocationUri(location);
            auto result = co_await pm.AddPackageByUriAsync(Uri{hstring{msix.wstring()}}, options);
            if (result.ExtendedErrorCode().value < 0) co_return Describe(result);
        } else if (!manifest.empty() && fs::exists(manifest, ec)) {
            RegisterPackageOptions options;
            options.ExternalLocationUri(location);
            options.DeveloperMode(true);  // a loose layout (not a signed .msix): what Add-AppxPackage -Register does
            auto result = co_await pm.RegisterPackageByUriAsync(Uri{hstring{manifest.wstring()}}, options);
            if (result.ExtendedErrorCode().value < 0) co_return Describe(result);
        } else {
            co_return hstring{lycan::Tf(L"win11.was_not_found_version_has_no", msix.wstring())};
        }
        co_return hstring{};
    } catch (hresult_error const& e) {
        co_return e.message();
    }
}

IAsyncOperation<hstring> UnregisterWin11MenuAsync() {
    try {
        PackageManager pm;
        for (auto const& p : pm.FindPackagesForUser(L"")) {
            if (p.Id().Name() != kPackageName) continue;
            auto result = co_await pm.RemovePackageAsync(p.Id().FullName());
            if (result.ExtendedErrorCode().value < 0) co_return Describe(result);
        }
        co_return hstring{};
    } catch (hresult_error const& e) {
        co_return e.message();
    }
}

}  // namespace lycan_app
