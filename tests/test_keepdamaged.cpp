// Extracting items that come out with a data error: they are kept (ExtractOptions::keepDamaged) so that what can be read of them is not lost;
// a good file is never replaced by a damaged one; a wrong password, a cancel and the like still leave nothing behind.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/extract.h"

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

static void Put16(std::string& s, uint16_t v) { s.append(reinterpret_cast<const char*>(&v), 2); }
static void Put32(std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); }

// a zip with one stored file whose stored CRC is `crc`: a wrong one is a CRC error, the whole content comes out
static std::string StoredZip(const std::string& name, const std::string& data, uint32_t crc) {
    std::string z;
    Put32(z, 0x04034B50); Put16(z, 20); Put16(z, 0); Put16(z, 0); Put16(z, 0); Put16(z, 0x21);
    Put32(z, crc); Put32(z, static_cast<uint32_t>(data.size())); Put32(z, static_cast<uint32_t>(data.size()));
    Put16(z, static_cast<uint16_t>(name.size())); Put16(z, 0);
    z += name; z += data;
    const uint32_t cd = static_cast<uint32_t>(z.size());
    Put32(z, 0x02014B50); Put16(z, 20); Put16(z, 20); Put16(z, 0); Put16(z, 0); Put16(z, 0); Put16(z, 0x21);
    Put32(z, crc); Put32(z, static_cast<uint32_t>(data.size())); Put32(z, static_cast<uint32_t>(data.size()));
    Put16(z, static_cast<uint16_t>(name.size())); Put16(z, 0); Put16(z, 0); Put16(z, 0); Put16(z, 0); Put32(z, 0); Put32(z, 0);
    z += name;
    const uint32_t cdSize = static_cast<uint32_t>(z.size()) - cd;
    Put32(z, 0x06054B50); Put16(z, 0); Put16(z, 0); Put16(z, 1); Put16(z, 1); Put32(z, cdSize); Put32(z, cd); Put16(z, 0);
    return z;
}

static uint32_t Crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) { c ^= b; for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
    return ~c;
}

static void Write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}
static std::string Read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
static size_t CountFiles(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) if (it->is_regular_file()) ++n;
    return n;
}

int main() {
    fs::path root = fs::temp_directory_path() / (L"lycan_kd_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root);

    std::string content;
    for (int i = 0; i < 30; ++i) content += "LycanZip keep-damaged test. ";
    const std::string good = StoredZip("font.txt", content, Crc32(content));
    const std::string badCrc = StoredZip("font.txt", content, Crc32(content) ^ 0x1234u);
    Write(root / "good.zip", good);
    Write(root / "crc.zip", badCrc);

    // 1. a CRC error: the whole content came out; kept by default of the program, deleted when asked
    {
        auto src = ArchiveSource::Open(root / "crc.zip");
        ExtractOptions o;
        o.keepDamaged = true;
        auto rep = ExtractTo(*src, root / "k1", o);
        EXPECT(!rep.ok() && rep.errors.size() == 1 && rep.files == 0);
        EXPECT(rep.kept.size() == 1 && rep.kept[0].English().find(L"font.txt") != std::wstring::npos);
        EXPECT(Read(root / "k1" / "font.txt") == content);   // kept under its name, with its content

        o.keepDamaged = false;
        auto rep2 = ExtractTo(*src, root / "k2", o);
        EXPECT(!rep2.ok() && rep2.kept.empty());
        EXPECT(!fs::exists(root / "k2" / "font.txt"));       // the old behaviour: nothing left
        auto rep3 = ExtractTo(*src, root / "k3");             // the library default is the old behaviour
        EXPECT(!rep3.ok() && !fs::exists(root / "k3" / "font.txt"));
    }

    // 2. a good file is never replaced by a damaged one
    {
        fs::create_directories(root / "ow");
        Write(root / "ow" / "font.txt", "GOOD");
        auto src = ArchiveSource::Open(root / "crc.zip");
        ExtractOptions o;
        o.keepDamaged = true;
        o.overwrite = OverwriteMode::Overwrite;
        auto rep = ExtractTo(*src, root / "ow", o);
        EXPECT(!rep.ok() && rep.kept.size() == 1);
        EXPECT(Read(root / "ow" / "font.txt") == "GOOD");                       // untouched
        EXPECT(Read(root / "ow" / L"font (damaged).txt") == content);              // the damaged one beside it
        EXPECT(rep.kept[0].English().find(L"font (damaged).txt") != std::wstring::npos);
        auto rep2 = ExtractTo(*src, root / "ow", o);                              // again: another name, not a replacement
        EXPECT(fs::exists(root / "ow" / L"font (damaged 2).txt"));
        // without keeping: the good file is still untouched, nothing new
        fs::create_directories(root / "ow2");
        Write(root / "ow2" / "font.txt", "GOOD");
        o.keepDamaged = false;
        auto rep3 = ExtractTo(*src, root / "ow2", o);
        EXPECT(!rep3.ok() && Read(root / "ow2" / "font.txt") == "GOOD" && CountFiles(root / "ow2") == 1);
        // a good archive replaces it, and leaves no temporary file
        auto good1 = ArchiveSource::Open(root / "good.zip");
        o.keepDamaged = true;
        auto rep4 = ExtractTo(*good1, root / "ow2", o);
        EXPECT(rep4.ok() && rep4.files == 1 && Read(root / "ow2" / "font.txt") == content && CountFiles(root / "ow2") == 1);
    }

    // 3. a cut-off archive: what came out of the file that was cut is kept (the first part)
    {
        std::mt19937 rng(7);
        std::string big;
        for (int i = 0; i < 40000; ++i) big += "line " + std::to_string(rng()) + " of the big file\r\n";   // compressible, a few hundred KB packed
        fs::create_directories(root / "src");
        Write(root / "src" / "a.txt", "small and good");
        Write(root / "src" / "big.txt", big);
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.storeCompressed = false;
        CreateArchive(root / "full.zip", {root / "src" / "a.txt", root / "src" / "big.txt"}, co);
        std::string zip = Read(root / "full.zip");
        Write(root / "cut.zip", zip.substr(0, zip.size() * 6 / 10));   // the download stopped inside big.txt
        try {
            auto src = ArchiveSource::Open(root / "cut.zip");
            ExtractOptions o;
            o.keepDamaged = true;
            auto rep = ExtractTo(*src, root / "cut", o);
            EXPECT(!rep.ok());                                                    // the damage is reported
            const bool aGood = Read(root / "cut" / "a.txt") == "small and good";
            EXPECT(aGood);                                                        // what came out before is fine
            if (fs::exists(root / "cut" / "big.txt")) {
                const std::string part = Read(root / "cut" / "big.txt");
                size_t same = 0;
                while (same < part.size() && same < big.size() && part[same] == big[same]) ++same;
                EXPECT(!part.empty() && part.size() < big.size() && same + 64 >= part.size());   // the first part (the last few bytes come from the block that was cut: they can be wrong)
                EXPECT(rep.kept.size() == 1);
            } else {
                std::fprintf(stderr, "note: the cut archive gave no partial file (kept=%zu)\n", rep.kept.size());
                ++failures;
            }
            o.keepDamaged = false;
            auto rep2 = ExtractTo(*src, root / "cut2", o);
            EXPECT(!rep2.ok() && !fs::exists(root / "cut2" / "big.txt") && rep2.kept.empty());
        } catch (const std::exception& e) {
            std::fprintf(stderr, "FAIL: cut archive: %s\n", e.what());
            ++failures;
        }
    }

    // 4. a wrong password gives garbage: nothing is kept
    {
        fs::create_directories(root / "enc");
        Write(root / "enc" / "secret.txt", content);
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.password = L"right";
        CreateArchive(root / "enc.zip", {root / "enc" / "secret.txt"}, co);
        auto src = ArchiveSource::Open(root / "enc.zip");
        ExtractOptions o;
        o.keepDamaged = true;
        o.password = [](std::wstring& p) { p = L"wrong"; return true; };
        auto rep = ExtractTo(*src, root / "encout", o);
        EXPECT(!rep.ok() && rep.wrongPassword && rep.kept.empty() && !fs::exists(root / "encout" / "secret.txt"));
    }

    // 5. a cancel leaves nothing of the file that was being written, and the file it was to replace is untouched
    {
        fs::create_directories(root / "cancel");
        Write(root / "cancel" / "font.txt", "GOOD");
        auto src = ArchiveSource::Open(root / "good.zip");
        ExtractOptions o;
        o.keepDamaged = true;
        o.overwrite = OverwriteMode::Overwrite;
        o.progress = [](uint64_t, uint64_t) { return false; };
        try { ExtractTo(*src, root / "cancel", o); } catch (const Error&) {}
        EXPECT(Read(root / "cancel" / "font.txt") == "GOOD" && CountFiles(root / "cancel") == 1);
    }

    fs::remove_all(root);
    if (failures == 0) std::puts("test_keepdamaged: all passed");
    return failures ? 1 : 0;
}
