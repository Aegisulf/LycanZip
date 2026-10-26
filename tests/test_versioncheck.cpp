// versioncheck: version parsing and order, latest.json, the install channel, the seven-day rule, and a refused address.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "lycan/versioncheck.h"

using namespace lycan;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int main() {
    // versions
    EXPECT(ParseVersion("1.0.0") && *ParseVersion("1.0.0") == (Version{1, 0, 0}));
    EXPECT(ParseVersion("v0.9") && *ParseVersion("v0.9") == (Version{0, 9, 0}));
    EXPECT(ParseVersion("12") && ParseVersion("12")->major == 12);
    EXPECT(!ParseVersion("") && !ParseVersion("1.") && !ParseVersion("1..2") && !ParseVersion("1.0.0-rc1") && !ParseVersion("a.b") && !ParseVersion("1.2.3.4"));
    EXPECT(!ParseVersion("99999999999.0.0"));
    EXPECT(IsNewerVersion({1, 0, 0}, {0, 9, 9}));
    EXPECT(IsNewerVersion({0, 9, 1}, {0, 9, 0}));
    EXPECT(IsNewerVersion({0, 10, 0}, {0, 9, 5}));
    EXPECT(!IsNewerVersion({0, 9, 0}, {0, 9, 0}));
    EXPECT(!IsNewerVersion({0, 8, 9}, {0, 9, 0}));

    // latest.json
    const auto ok = ParseLatestJson("{\n  \"version\": \"1.0.0\",\n  \"released\": \"2026-11-01\",\n  \"page\": \"https://aegisulf.com/lycanzip/download/\",\n  \"changelog\": \"https://aegisulf.com/lycanzip/changelog/\"\n}\n");
    EXPECT(ok && ok->version == "1.0.0" && ok->released == "2026-11-01" && ok->page == "https://aegisulf.com/lycanzip/download/" && ok->changelog == "https://aegisulf.com/lycanzip/changelog/");
    EXPECT(ParseLatestJson("{\"version\":\"2.1\"}") && ParseLatestJson("{\"version\":\"2.1\"}")->page.empty());
    EXPECT(!ParseLatestJson(""));
    EXPECT(!ParseLatestJson("{}"));
    EXPECT(!ParseLatestJson("{\"version\": 1}"));
    EXPECT(!ParseLatestJson("{\"version\": \"soon\"}"));
    EXPECT(!ParseLatestJson("{\"version\": \"1.0.0\", \"page\": \"http://example.com/\"}"));        // not https
    EXPECT(!ParseLatestJson("{\"version\": \"1.0.0\", \"page\": \"javascript:alert(1)\"}"));
    EXPECT(!ParseLatestJson("{\"version\": \"1.0.0\", \"changelog\": \"https://a b/\"}"));
    EXPECT(!ParseLatestJson("{\"version\": \"1.0.0"));                                              // cut off
    EXPECT(!ParseLatestJson("<html>404</html>"));

    // the way the program was installed
    EXPECT(DetectChannel(true, false, L"C:\\Program Files\\WindowsApps\\x\\LycanZip.exe") == InstallChannel::Store);
    EXPECT(DetectChannel(false, false, L"C:\\Users\\a\\AppData\\Local\\Microsoft\\WinGet\\Packages\\Aegisulf.LycanZip_x\\LycanZip.exe") == InstallChannel::Winget);
    EXPECT(DetectChannel(false, false, L"C:\\Users\\a\\scoop\\apps\\lycanzip\\current\\LycanZip.exe") == InstallChannel::Scoop);
    EXPECT(DetectChannel(false, true, L"D:\\tools\\LycanZip\\LycanZip.exe") == InstallChannel::Portable);
    EXPECT(DetectChannel(false, false, L"C:\\Program Files\\LycanZip\\LycanZip.exe") == InstallChannel::Standard);
    {   // made by our installer: <root>\current\LycanZip.exe with <root>\Update.exe
        const std::filesystem::path root = std::filesystem::temp_directory_path() / (L"lycan_vc_" + std::to_wstring(GetCurrentProcessId()));
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root / L"current");
        const std::wstring exe = (root / L"current" / L"LycanZip.exe").wstring();
        EXPECT(DetectChannel(false, false, exe) == InstallChannel::Standard);        // no Update.exe: just a folder called current
        { std::ofstream(root / L"Update.exe") << "x"; }
        EXPECT(DetectChannel(false, false, exe) == InstallChannel::Installed);
        EXPECT(DetectChannel(true, false, exe) == InstallChannel::Store);            // a package identity wins
        std::filesystem::remove_all(root);
    }

    // seven days
    EXPECT(UpdateCheckDue(0, 1000000));
    EXPECT(!UpdateCheckDue(1000000, 1000000 + 3600));
    EXPECT(UpdateCheckDue(1000000, 1000000 + 7 * 24 * 3600));
    EXPECT(UpdateCheckDue(2000000, 1000000));   // the clock went back

    // an address that is refused without a request: http on another host, not a URL
    EXPECT(!FetchLatest(L"http://example.com/latest.json"));
    EXPECT(!FetchLatest(L"not a url"));
    EXPECT(!FetchLatest(L"ftp://localhost/latest.json"));

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("versioncheck: ok\n");
    return 0;
}
