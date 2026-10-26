// Scratch folder: where it is, the room check before a tar.gz is made, the full-disk error while one is opened
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/tempdir.h"
#include "lycan/testhooks.h"

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
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path root = fs::path(tmp) / L"lycan_test_tempdir";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / L"src", ec);
    {
        std::ofstream f(root / L"src" / L"big.bin", std::ios::binary);
        std::string chunk(1 << 20, 'x');
        for (int i = 0; i < 2; ++i) f.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));  // 2 MB
    }

    // the folder
    EXPECT(!ScratchDirectory({}).empty());
    const fs::path custom = root / L"scratch" / L"LycanZip";
    EXPECT(ScratchDirectory(custom) == custom && fs::is_directory(custom));
    EXPECT(ScratchDirectory(root / L"src" / L"big.bin") != root / L"src" / L"big.bin");  // a file is not a folder: back to the system temp

    // slow locations, volumes, message round trip
    EXPECT(IsSlowLocation(L"\\\\server\\share\\temp"));
    EXPECT(!IsSlowLocation(root));
    EXPECT(!LocalFixedVolumes().empty());
    {
        const Message m = MakeTempSpaceMessage(5ull << 30, 3ull << 20, L"D:\\Temp\\中文");
        uint64_t need = 0, have = 0;
        std::wstring dir;
        EXPECT(ParseTempSpaceMessage(m, need, have, dir));
        EXPECT(need == (5ull << 30) && have == (3ull << 20) && dir == L"D:\\Temp\\中文" && !m.English().empty() && m.English().find(L"@@") == std::wstring::npos);
        EXPECT(!ParseTempSpaceMessage(Message(L"some other error"), need, have, dir));
    }

    // compressing to tar.gz: not enough room in the scratch folder is reported before anything is written
    CompressOptions o;
    o.type = ArchiveType::TarGz;
    o.tempDir = custom;
    SetTestFreeSpace(100 * 1024);
    bool threw = false;
    try {
        CreateArchive(root / L"out1.tar.gz", {root / L"src" / L"big.bin"}, o);
    } catch (const Error& e) {
        threw = e.tempSpace() && e.diskFull();
        uint64_t need = 0, have = 0;
        std::wstring dir;
        EXPECT(ParseTempSpaceMessage(e.message(), need, have, dir) && need >= (2u << 20) && have == 100 * 1024 && dir == custom.wstring());
    }
    EXPECT(threw);
    EXPECT(!fs::exists(root / L"out1.tar.gz") && !fs::exists(root / L"out1.tar.gz.lycantmp"));
    SetTestFreeSpace(0);

    // with room it works, and zip / 7z never need the scratch folder
    auto rep = CreateArchive(root / L"out2.tar.gz", {root / L"src" / L"big.bin"}, o);
    EXPECT(rep.ok() && fs::exists(root / L"out2.tar.gz"));
    CompressOptions z;
    z.type = ArchiveType::Zip;
    SetTestFreeSpace(100 * 1024);
    EXPECT(CreateArchive(root / L"out3.zip", {root / L"src" / L"big.bin"}, z).ok());
    SetTestFreeSpace(0);

    // opening a tar.gz: the disk fills up while the tar is unpacked into the scratch file
    OpenOptions oo;
    oo.tempDir = custom;
    {
        auto src = ArchiveSource::Open(root / L"out2.tar.gz", oo);  // fine
        EXPECT(src && !src->Entries().empty());
    }
    SetTestWriteFault(64 * 1024);
    threw = false;
    try {
        auto src = ArchiveSource::Open(root / L"out2.tar.gz", oo);
    } catch (const Error& e) {
        threw = e.tempSpace();
        uint64_t need = 1, have = 0;
        std::wstring dir;
        const bool pr = ParseTempSpaceMessage(e.message(), need, have, dir);
        if (!(pr && need == 0 && dir == custom.wstring())) std::fprintf(stderr, "msg=[%s] parsed=%d need=%llu dir=[%ls]\n", e.what(), pr, need, dir.c_str());
        EXPECT(pr && need == 0 && dir == custom.wstring());
    }
    SetTestWriteFault(0);
    EXPECT(threw);

    fs::remove_all(root, ec);
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("tempdir: all passed\n");
    return failures ? 1 : 0;
}
