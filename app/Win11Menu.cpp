#include "pch.h"
#include "Win11Menu.h"

#include <winrt/Windows.Management.Deployment.h>
#include <wincrypt.h>

#include <fstream>
#include <iterator>
#include <vector>

#include "lycan/i18n.h"
#include "lycan/shellreg.h"

namespace lycan_app {
using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Management::Deployment;
namespace fs = std::filesystem;

namespace {
constexpr const wchar_t* kPackageName = L"LycanZip.ShellExtension";  // the Identity Name of the Windows 11 menu package (made by the release flow)

bool DeveloperMode() {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock", L"AllowDevelopmentWithoutDevLicense",
                        RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v == 1;
}

fs::path ExeDir() {
    wchar_t exe[MAX_PATH * 2];
    const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return fs::path(std::wstring(exe, n)).parent_path();
}

// Whether the certificate in LycanZipShell.cer is in Trusted People of the computer
bool CertTrusted(fs::path const& cer) {
    std::ifstream f(cer, std::ios::binary);
    std::vector<BYTE> der((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (der.empty()) return false;
    PCCERT_CONTEXT mine = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), static_cast<DWORD>(der.size()));
    if (!mine) return false;
    bool found = false;
    if (HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_READONLY_FLAG | CERT_STORE_OPEN_EXISTING_FLAG, L"TrustedPeople")) {
        BYTE hash[20];
        DWORD size = sizeof(hash);
        if (CertGetCertificateContextProperty(mine, CERT_HASH_PROP_ID, hash, &size)) {
            CRYPT_HASH_BLOB blob{size, hash};
            if (PCCERT_CONTEXT hit = CertFindCertificateInStore(store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr)) { found = true; CertFreeCertificateContext(hit); }
        }
        CertCloseStore(store, 0);
    }
    CertFreeCertificateContext(mine);
    return found;
}

// LYCAN_WIN11_MENU_MANIFEST (tests), or with Developer Mode on and the certificate not trusted, the unsigned layout next to the exe
fs::path DevManifest() {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_WIN11_MENU_MANIFEST", buf, static_cast<DWORD>(std::size(buf)));
    if (n > 0 && n < std::size(buf)) return fs::path(std::wstring(buf, n));
    const fs::path dir = ExeDir();
    std::error_code ec;
    const fs::path loose = dir / L"shell-menu" / L"AppxManifest.xml";
    if (DeveloperMode() && fs::exists(loose, ec) && !CertTrusted(dir / L"LycanZipShell.cer")) return loose;
    return {};
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

bool Win11MenuNeedsTrust(fs::path const& exeDir) {
    std::error_code ec;
    const fs::path cer = exeDir / L"LycanZipShell.cer";
    return fs::exists(exeDir / L"LycanZipShell.msix", ec) && fs::exists(cer, ec) && DevManifest().empty() && !CertTrusted(cer);
}

IAsyncOperation<hstring> TrustWin11MenuCertAsync(fs::path exeDir) {
    const std::wstring params = L"-f -addstore TrustedPeople \"" + (exeDir / L"LycanZipShell.cer").wstring() + L"\"";
    co_await resume_background();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = L"certutil.exe";
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) co_return hstring{L"cancelled"};
        co_return hstring{lycan::Tf(L"win11.error_code", std::to_wstring(err))};
    }
    DWORD code = 1;
    if (sei.hProcess) { WaitForSingleObject(sei.hProcess, 60000); GetExitCodeProcess(sei.hProcess, &code); CloseHandle(sei.hProcess); }
    if (code != 0 || !CertTrusted(exeDir / L"LycanZipShell.cer")) co_return hstring{lycan::Tf(L"win11.error_code", std::to_wstring(code))};
    co_return hstring{};
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
        if (!manifest.empty() && fs::exists(manifest, ec)) {
            RegisterPackageOptions options;
            options.ExternalLocationUri(location);
            options.DeveloperMode(true);  // a loose layout (not a signed .msix): what Add-AppxPackage -Register does
            auto result = co_await pm.RegisterPackageByUriAsync(Uri{hstring{manifest.wstring()}}, options);
            if (result.ExtendedErrorCode().value < 0) co_return Describe(result);
        } else if (fs::exists(msix, ec)) {
            AddPackageOptions options;
            options.ExternalLocationUri(location);
            auto result = co_await pm.AddPackageByUriAsync(Uri{hstring{msix.wstring()}}, options);
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
