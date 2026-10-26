// File associations: written only to a temporary test key (HKCU\Software\LycanZipTest_<pid>), with the Explorer notification off; the real Classes / RegisteredApplications are not touched.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "lycan/fileassoc.h"
#include "lycan/fileicons.h"
#include "lycan/i18n.h"

using namespace lycan;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static std::wstring ReadReg(const std::wstring& sub, const wchar_t* name, DWORD* type = nullptr) {
    wchar_t buf[2048];
    DWORD n = sizeof(buf), t = 0;
    LSTATUS s = RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_ANY, &t, buf, &n);
    if (type) *type = t;
    if (s != ERROR_SUCCESS) return L"<missing>";
    return t == REG_SZ ? std::wstring(buf) : std::wstring(L"<non-string>");
}

static bool HasValue(const std::wstring& sub, const wchar_t* name, DWORD* type = nullptr) {
    DWORD t = 0, n = 0;
    LSTATUS s = RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), name, RRF_RT_ANY, &t, nullptr, &n);
    if (type) *type = t;
    return s == ERROR_SUCCESS;
}

int main() {
    lycan::i18n::Load(std::filesystem::u8path(LYCAN_LANG_DIR), L"zh-TW");  // the type names are texts of the language file
    std::wstring base = L"Software\\LycanZipTest_" + std::to_wstring(GetCurrentProcessId());
    FileAssocOptions o;
    o.exe = L"C:\\Apps\\LycanZip\\LycanZip.exe";
    o.classesKey = base + L"\\Classes";
    o.softwareKey = base + L"\\Software";
    o.notifyShell = false;

    EXPECT(!IsFileAssociationsRegistered(o));
    std::wstring err;
    EXPECT(RegisterFileAssociations(o, &err));
    EXPECT(IsFileAssociationsRegistered(o));

    const std::wstring cls = o.classesKey, sw = o.softwareKey;
    const std::wstring cmd = L"\"C:\\Apps\\LycanZip\\LycanZip.exe\" \"%1\"";

    // ProgID
    // one program id per extension (the name in Explorer's "Type" column tells the kind of archive), the icon of its family (a resource of the exe, lycan/fileicons.h)
    struct Case { const wchar_t* ext; const wchar_t* prog; const wchar_t* name; const wchar_t* icon; };
    const Case cases[] = {{L".zip", L"LycanZip.zip", L"LycanZip ZIP 壓縮檔", L",-101"}, {L".7z", L"LycanZip.7z", L"LycanZip 7Z 壓縮檔", L",-102"},
                          {L".rar", L"LycanZip.rar", L"LycanZip RAR 壓縮檔", L",-103"}, {L".gz", L"LycanZip.gz", L"LycanZip GZ 壓縮檔", L",-104"},
                          {L".tgz", L"LycanZip.tgz", L"LycanZip TGZ 壓縮檔", L",-104"}, {L".cab", L"LycanZip.cab", L"LycanZip CAB 壓縮檔", L",-106"},
                          {L".001", L"LycanZip.001", L"LycanZip 分卷壓縮檔 (.001)", L",-106"},
                          {L".cbz", L"LycanZip.cbz", L"LycanZip CBZ 漫畫壓縮檔", L",-107"}, {L".cb7", L"LycanZip.cb7", L"LycanZip CB7 漫畫壓縮檔", L",-107"}};
    for (auto& c : cases) {
        EXPECT(AssociationProgId(c.ext) == c.prog && AssociationTypeName(c.ext) == c.name);
        const std::wstring pid = cls + L"\\" + c.prog;
        EXPECT(ReadReg(pid, nullptr) == c.name);
        EXPECT(ReadReg(pid + L"\\DefaultIcon", nullptr) == std::wstring(L"\"C:\\Apps\\LycanZip\\LycanZip.exe\"") + c.icon);
        EXPECT(ReadReg(pid + L"\\shell\\open\\command", nullptr) == cmd);
    }
    EXPECT(ReadReg(cls + L"\\LycanZip.Archive", nullptr) == L"<missing>");  // the one program id of earlier versions is not written
    EXPECT(!HasValue(cls + L"\\LycanZip.Iso", nullptr) && ReadReg(cls + L"\\LycanZip.Iso\\shell\\open\\command", nullptr) == L"<missing>");  // .iso is not associated
    // a change of the language of the program: the names are written again, nothing else changes
    {
        HKEY k;
        RegCreateKeyExW(HKEY_CURRENT_USER, (cls + L"\\LycanZip.gz").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
        RegSetValueExW(k, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(L"old name"), sizeof(L"old name"));
        RegCloseKey(k);
        EXPECT(UpdateAssociationNames(o));
        EXPECT(ReadReg(cls + L"\\LycanZip.gz", nullptr) == L"LycanZip GZ 壓縮檔");
        EXPECT(ReadReg(cls + L"\\LycanZip.gz\\shell\\open\\command", nullptr) == cmd);   // where it points to is left alone
        EXPECT(!UpdateAssociationNames(o));   // nothing to change the second time
    }
    // Applications\LycanZip.exe ("Open with")
    EXPECT(ReadReg(cls + L"\\Applications\\LycanZip.exe", L"FriendlyAppName") == L"LycanZip");
    EXPECT(ReadReg(cls + L"\\Applications\\LycanZip.exe\\shell\\open\\command", nullptr) == cmd);
    EXPECT(HasValue(cls + L"\\Applications\\LycanZip.exe\\SupportedTypes", L".zip"));
    EXPECT(HasValue(cls + L"\\Applications\\LycanZip.exe\\SupportedTypes", L".7z"));
    // Capabilities + RegisteredApplications (Settings > Default apps)
    EXPECT(ReadReg(sw + L"\\LycanZip\\Capabilities", L"ApplicationName") == L"LycanZip");
    EXPECT(!ReadReg(sw + L"\\LycanZip\\Capabilities", L"ApplicationDescription").empty());
    for (auto& c : cases) EXPECT(ReadReg(sw + L"\\LycanZip\\Capabilities\\FileAssociations", c.ext) == c.prog);
    EXPECT(ReadReg(sw + L"\\RegisteredApplications", L"LycanZip") == sw + L"\\LycanZip\\Capabilities");
    // OpenWithProgids is REG_NONE
    DWORD type = 0;
    EXPECT(HasValue(cls + L"\\.zip\\OpenWithProgids", L"LycanZip.zip", &type) && type == REG_NONE);
    EXPECT(HasValue(cls + L"\\.rar\\OpenWithProgids", L"LycanZip.rar"));
    EXPECT(HasValue(cls + L"\\.cab\\OpenWithProgids", L"LycanZip.cab"));
    EXPECT(!HasValue(cls + L"\\.zip\\OpenWithProgids", L"LycanZip.Archive"));
    // these extensions are not touched
    EXPECT(!HasValue(cls + L"\\.iso\\OpenWithProgids", L"LycanZip.Iso"));
    EXPECT(!HasValue(cls + L"\\.jar\\OpenWithProgids", L"LycanZip.zip"));
    // Note: UserChoice must never be written (the default program can only be chosen by the user)
    EXPECT(ReadReg(cls + L"\\.zip", L"UserChoice") == L"<missing>");

    // extension list
    size_t n = 0;
    auto exts = AssociatedExtensions(&n);
    EXPECT(n >= 10 && exts != nullptr);
    bool allDotted = true;
    for (size_t i = 0; i < n; ++i) if (exts[i][0] != L'.') allDotted = false;
    EXPECT(allDotted);

    // OpenWithProgids values of other programs must not be touched
    {
        HKEY k;
        RegCreateKeyExW(HKEY_CURRENT_USER, (cls + L"\\.zip\\OpenWithProgids").c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr);
        RegSetValueExW(k, L"OtherApp.zip", 0, REG_NONE, nullptr, 0);
        RegCloseKey(k);
    }

    EXPECT(RegisteredAssocExe(o) == std::filesystem::path(L"C:\\Apps\\LycanZip\\LycanZip.exe"));  // what the registration points to (a moved portable folder shows here)
    // registering again / a different path -> overwritten with the newest
    o.exe = L"D:\\Tools\\LycanZip.exe";
    EXPECT(RegisterFileAssociations(o, &err));
    EXPECT(ReadReg(cls + L"\\LycanZip.zip\\shell\\open\\command", nullptr) == L"\"D:\\Tools\\LycanZip.exe\" \"%1\"");
    EXPECT(ReadReg(cls + L"\\LycanZip.zip\\DefaultIcon", nullptr) == L"\"D:\\Tools\\LycanZip.exe\",-101");
    EXPECT(RegisteredAssocExe(o) == std::filesystem::path(L"D:\\Tools\\LycanZip.exe"));

    // Remove: only our entries are removed
    EXPECT(UnregisterFileAssociations(o, &err));
    EXPECT(!IsFileAssociationsRegistered(o));
    EXPECT(RegisteredAssocExe(o).empty());
    for (auto& c : cases) EXPECT(ReadReg(cls + L"\\" + c.prog + L"\\shell\\open\\command", nullptr) == L"<missing>");
    EXPECT(ReadReg(cls + L"\\Applications\\LycanZip.exe", L"FriendlyAppName") == L"<missing>");
    EXPECT(ReadReg(sw + L"\\RegisteredApplications", L"LycanZip") == L"<missing>");
    EXPECT(!HasValue(cls + L"\\.zip\\OpenWithProgids", L"LycanZip.zip"));
    EXPECT(!HasValue(cls + L"\\.zip\\OpenWithProgids", L"LycanZip.Archive"));
    EXPECT(HasValue(cls + L"\\.zip\\OpenWithProgids", L"OtherApp.zip"));  // the value of another program is still there
    EXPECT(UnregisterFileAssociations(o, &err));                          // absent counts as success

    EXPECT(!RegisterFileAssociations(FileAssocOptions{}, &err));
    EXPECT(!err.empty());

    // Read-only query: must not crash; an extension without association gives an empty string / false
    (void)CurrentDefaultAppName(L".zip");
    EXPECT(CurrentDefaultAppName(L".definitely-not-an-extension-xyz").empty());
    EXPECT(!IsLycanZipDefaultFor(L".definitely-not-an-extension-xyz"));

    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    if (failures == 0) std::puts("test_fileassoc: all passed");
    return failures ? 1 : 0;
}
