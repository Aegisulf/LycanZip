// A zip whose packed size is 6 bytes larger than its deflate stream (the way some servers write them): 7-Zip reports "data after the end
// of the payload" after the CRC matched; Windows and Bandizip extract it. LycanZip keeps the content and notes it (a warning, not an error).
#include "no_dialogs.h"
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/extract.h"
#include "lycan/hash.h"

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

static const uint8_t kDeflate[] = {0xF3, 0xA9, 0x4C, 0x4E, 0xCC, 0x8B, 0xCA, 0x2C, 0x50, 0x48, 0x49, 0x2C, 0x49, 0xD4, 0x4D, 0x4C, 0x2B, 0x49, 0x2D, 0xD2, 0x4D,
                                    0xCD, 0x4B, 0x51, 0x28, 0x49, 0x2D, 0x2E, 0xD1, 0x53, 0xF0, 0x19, 0x95, 0x1D, 0x95, 0x1D, 0x95, 0xA5, 0xB1, 0x2C, 0x00};
static const uint32_t kCrc = 0xF71855B5;
static const uint32_t kSize = 900;  // "LycanZip data-after-end test. " x 30

static void Put16(std::string& s, uint16_t v) { s.append(reinterpret_cast<const char*>(&v), 2); }
static void Put32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }

// extra = how many bytes follow the deflate stream inside the packed size
static std::string MakeZip(uint32_t extra) {
    const std::string name = "font.ttf";
    const uint32_t packed = static_cast<uint32_t>(sizeof(kDeflate)) + extra;
    std::string z;
    Put32(z, 0x04034B50); Put16(z, 20); Put16(z, 0); Put16(z, 8); Put16(z, 0); Put16(z, 0x21);
    Put32(z, kCrc); Put32(z, packed); Put32(z, kSize); Put16(z, static_cast<uint16_t>(name.size())); Put16(z, 0);
    z += name;
    z.append(reinterpret_cast<const char*>(kDeflate), sizeof(kDeflate));
    z.append(extra, '\x11');
    const uint32_t cd = static_cast<uint32_t>(z.size());
    Put32(z, 0x02014B50); Put16(z, 20); Put16(z, 20); Put16(z, 0); Put16(z, 8); Put16(z, 0); Put16(z, 0x21);
    Put32(z, kCrc); Put32(z, packed); Put32(z, kSize); Put16(z, static_cast<uint16_t>(name.size())); Put16(z, 0); Put16(z, 0);
    Put16(z, 0); Put16(z, 0); Put32(z, 0); Put32(z, 0);
    z += name;
    const uint32_t cdSize = static_cast<uint32_t>(z.size()) - cd;
    Put32(z, 0x06054B50); Put16(z, 0); Put16(z, 0); Put16(z, 1); Put16(z, 1); Put32(z, cdSize); Put32(z, cd); Put16(z, 0);
    return z;
}

struct VecSink : ByteSink {
    std::string data;
    void Write(const void* p, size_t n) override { data.append(static_cast<const char*>(p), n); }
};

static std::string Expected() {
    std::string s;
    for (int i = 0; i < 30; ++i) s += "LycanZip data-after-end test. ";
    return s;
}

int main() {
    fs::path dir = fs::temp_directory_path() / (L"lycan_dae_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    auto write = [&](const char* name, const std::string& bytes) { fs::path p = dir / name; std::ofstream(p, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size())); return p; };

    const fs::path good = write("good.zip", MakeZip(0)), odd = write("odd.zip", MakeZip(6));

    // a normal zip: no warning
    {
        auto src = ArchiveSource::Open(good);
        auto rep = ExtractTo(*src, dir / "good");
        EXPECT(rep.ok() && rep.files == 1 && rep.warnings.empty());
        auto t = TestArchive(*src);
        EXPECT(t.ok() && t.files == 1 && t.warnings.empty());
    }
    // the odd one: extracted, kept, noted
    {
        auto src = ArchiveSource::Open(odd);
        auto rep = ExtractTo(*src, dir / "odd");
        EXPECT(rep.ok() && rep.errors.empty());
        EXPECT(rep.files == 1 && rep.warnings.size() == 1);
        EXPECT(rep.warnings.size() == 1 && rep.warnings[0].English().find(L"font.ttf") != std::wstring::npos);
        std::ifstream in(dir / "odd" / "font.ttf", std::ios::binary);
        std::string got((std::istreambuf_iterator<char>(in)), {});
        EXPECT(got == Expected());   // the whole content, not a partial or deleted file

        auto t = TestArchive(*src);
        EXPECT(t.ok() && t.files == 1 && t.warnings.size() == 1);   // the test passes, with the note

        VecSink sink;
        bool threw = false;
        try { src->ReadEntry(0, sink); } catch (...) { threw = true; }
        EXPECT(!threw && sink.data == Expected());   // preview / open from the archive works

        auto h = HashEntries(*src, {0});
        EXPECT(h.failed == 0 && h.files.size() == 1 && !h.files[0].hex.empty());
    }
    // a real data error is still an error (damage inside the compressed stream)
    {
        std::string bad = MakeZip(0);
        bad[30 + 8 + 5] ^= 0x55;  // inside the deflate data
        const fs::path p = write("bad.zip", bad);
        auto src = ArchiveSource::Open(p);
        auto rep = ExtractTo(*src, dir / "bad");
        EXPECT(!rep.ok() && rep.errors.size() == 1 && rep.warnings.empty());
    }

    fs::remove_all(dir);
    if (failures == 0) std::puts("test_dataafterend: all passed");
    return failures ? 1 : 0;
}
