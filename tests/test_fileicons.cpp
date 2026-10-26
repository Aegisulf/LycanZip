// The file-type icon families (lycan/fileicons.h) and the files that must exist for them.
#include "no_dialogs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "lycan/fileassoc.h"
#include "lycan/fileicons.h"

using namespace lycan;
namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int main() {
    // extensions, with or without the dot, any case
    EXPECT(IconFamilyForExtension(L".zip") == IconFamily::Zip && IconFamilyForExtension(L"ZIP") == IconFamily::Zip);
    EXPECT(IconFamilyForExtension(L".7z") == IconFamily::SevenZip && IconFamilyForExtension(L".cb7") == IconFamily::Comic);
    EXPECT(IconFamilyForExtension(L".rar") == IconFamily::Rar && IconFamilyForExtension(L".CBR") == IconFamily::Comic);
    for (auto e : {L".tar", L".gz", L".tgz", L".xz", L".txz", L".bz2", L".tbz2", L".tbz", L".zst"}) EXPECT(IconFamilyForExtension(e) == IconFamily::Tar);
    EXPECT(IconFamilyForExtension(L".iso") == IconFamily::Iso && IconFamilyForExtension(L".wim") == IconFamily::Iso);
    for (auto e : {L".cab", L".arj", L".lzh", L".z", L".001", L".nothing", L"", L"."}) EXPECT(IconFamilyForExtension(e) == IconFamily::Other);

    // paths: the last extension decides
    EXPECT(IconFamilyForPath(L"D:\\Data\\backup.tar.gz") == IconFamily::Tar);
    EXPECT(IconFamilyForPath(L"C:\\x.y\\readme") == IconFamily::Other);  // a dot in a folder name is not an extension
    EXPECT(IconFamilyForPath(L"photos.ZIP") == IconFamily::Zip);
    EXPECT(IconFamilyForPath(L"") == IconFamily::Other);

    // names, resource ids and program ids are distinct; the ids are 101..106 in order
    std::set<std::wstring> names;
    std::set<int> ids;
    const IconFamily all[] = {IconFamily::Zip, IconFamily::SevenZip, IconFamily::Rar, IconFamily::Tar, IconFamily::Iso, IconFamily::Other, IconFamily::Comic};
    for (auto f : all) { names.insert(IconFamilyName(f)); ids.insert(IconResourceId(f)); }
    EXPECT(names.size() == 7 && ids.size() == 7);
    EXPECT(IconResourceId(IconFamily::Zip) == 101 && IconResourceId(IconFamily::Other) == 106 && IconResourceId(IconFamily::Comic) == 107);
    for (auto e : {L".cbz", L".cbr", L".cb7", L".cbt", L".CBZ"}) EXPECT(IconFamilyForExtension(e) == IconFamily::Comic);

    // every extension the file associations cover maps to a family that is registered (not Iso)
    size_t n = 0;
    auto exts = AssociatedExtensions(&n);
    for (size_t i = 0; i < n; ++i) EXPECT(IconFamilyForExtension(exts[i]) != IconFamily::Iso);

    // the files of each family are in the source tree (the .ico for the exe resources, the 32 px PNG for the program)
    fs::path dir = fs::path(__FILE__).parent_path().parent_path() / "app" / "Assets" / "FileIcons";
    for (auto f : all) {
        const fs::path ico = dir / (std::wstring(IconFamilyName(f)) + L".ico"), png = dir / (std::wstring(IconFamilyName(f)) + L"-32.png"), png64 = dir / (std::wstring(IconFamilyName(f)) + L"-64.png");
        EXPECT(fs::exists(ico) && fs::file_size(ico) > 1000);
        EXPECT(fs::exists(png) && fs::file_size(png) > 100);
        EXPECT(fs::exists(png64) && fs::file_size(png64) > 100);
        std::ifstream in(ico, std::ios::binary);
        unsigned char h[6] = {};
        in.read(reinterpret_cast<char*>(h), 6);
        EXPECT(h[0] == 0 && h[1] == 0 && h[2] == 1 && h[3] == 0 && h[4] >= 6);  // an .ico with at least 6 images (16 .. 256)
    }

    if (failures == 0) std::puts("test_fileicons: all passed");
    return failures ? 1 : 0;
}
