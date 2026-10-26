// The update of the installed copy (installed_update.h) against a feed on this computer, with a made-up installation (no real install, nothing is applied).
// Run by the release flow, which makes the packages (vpk) and the feed, signs it and serves it:
//   test_updater <feed base url> <public key hex, 128 digits> <work folder> <ok | refused-feed | refused-package>
//   ok               the offer is 1.0.1, the download passes the checks, the packages are in the packages folder
//   refused-feed     the feed was changed or signed by another key: there is no offer
//   refused-package  the feed is fine but a package was changed: the download fails and nothing stays in the packages folder
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>

#include "installed_update.h"

namespace fs = std::filesystem;
using namespace lycan;

static bool FromHex(const std::string& hex, UpdatePublicKey& out) {
    if (hex.size() != 128) return false;
    for (size_t i = 0; i < 64; ++i) out[i] = static_cast<uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) { std::fputs("usage: test_updater <feed url> <public key hex> <work dir> <ok|refused-feed|refused-package>\n", stderr); return 2; }
    const std::wstring base = argv[1];
    const std::wstring expect = argv[4];
    UpdatePublicKey key{};
    std::string hex;
    for (const wchar_t* c = argv[2]; *c; ++c) hex += static_cast<char>(*c);
    if (!FromHex(hex, key)) { std::fputs("bad key\n", stderr); return 2; }

    const fs::path work = argv[3];
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work / "current");
    fs::create_directories(work / "packages");
    {
        std::ofstream m(work / "current" / "sq.version", std::ios::binary);
        m << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd\">\n<metadata>\n"
             "<id>Aegisulf.LycanZipTest</id>\n<title>LycanZip</title>\n<description>LycanZip</description>\n<authors>Aegisulf</authors>\n<version>1.0.0</version>\n"
             "<channel>win</channel>\n<mainExe>app.exe</mainExe>\n<os>win</os>\n<rid>win</rid>\n</metadata>\n</package>\n";
    }
    { std::ofstream(work / "Update.exe") << "x"; }
    FakeInstall fake{work, work / "Update.exe", work / "packages", work / "current" / "sq.version", work / "current"};

    int failures = 0;
    auto expectThat = [&](bool cond, const char* what) { if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; } };
    auto packagesLeft = [&] {
        int n = 0;
        for (auto& e : fs::directory_iterator(work / "packages", ec)) if (e.path().extension() == L".nupkg") ++n;
        return n;
    };

    InstalledUpdater updater(base, key, &fake);
    std::wstring error;
    auto offer = updater.Check(&error);
    if (expect == L"ok") {
        expectThat(offer.has_value() && offer->version == "1.0.1", "the offer is 1.0.1");
        if (offer) {
            expectThat(offer->bytes > 0, "the size of the offer is known");
            int last = 0;
            expectThat(updater.Download([&](int p) { last = p; }, &error), "the download passes the checks");
            expectThat(packagesLeft() >= 1, "the packages are in the packages folder");
            (void)last;
        }
    } else if (expect == L"refused-feed") {
        expectThat(!offer.has_value(), "a changed feed or a feed of another key gives no offer");
        expectThat(packagesLeft() == 0, "nothing was downloaded");
    } else if (expect == L"refused-package") {
        expectThat(offer.has_value(), "the feed is accepted");
        if (offer) {
            expectThat(!updater.Download(nullptr, &error), "a package that is not what the signed feed lists is refused");
            expectThat(packagesLeft() == 0, "and it is not kept");
        }
    } else {
        std::fputs("unknown case\n", stderr);
        return 2;
    }
    fs::remove_all(work, ec);
    if (failures == 0) std::wprintf(L"test_updater %s: passed\n", expect.c_str());
    return failures ? 1 : 0;
}
