// Checksum files (checksum.h): reading the formats (sha256sum, BSD, .sfv, BOM, CRLF, ANSI names), checking files on disk and inside
// archives (a different value, a missing file, a file that is not listed, names that need a guess), the checksum files that sit
// next to an archive, and making them (a folder, next to a new archive; cancel leaves nothing).
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <random>

#include "lycan/archive_source.h"
#include "lycan/checksum.h"
#include "lycan/compress.h"
#include "lycan/error.h"
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

static void WriteFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}
static std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
static std::string Sha(const std::string& s) { return HashHex(kHashSha256, s.data(), s.size()); }

static const SumProblem* Find(const SumVerifyReport& r, const std::wstring& name) {
    for (auto& p : r.problems) if (p.name == name) return &p;
    return nullptr;
}

static int Run() {
    const fs::path root = fs::temp_directory_path() / (L"lycan_test_checksum_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    // ---- which file names are checksum files ----
    {
        EXPECT(SumAlgoForPath(L"a.zip.sha256") == kHashSha256 && SumAlgoForPath(L"A.SHA512") == kHashSha512 && SumAlgoForPath(L"x.md5") == kHashMd5);
        EXPECT(SumAlgoForPath(L"x.sfv") == kHashCrc32 && SumAlgoForPath(L"x.sha1") == kHashSha1 && SumAlgoForPath(L"SHA256SUMS") == kHashSha256);
        EXPECT(SumAlgoForPath(L"x.txt") == 0 && SumAlgoForPath(L"sha256") == 0 && !IsSumFile(L"a.zip"));
        EXPECT(std::wstring(SumExtensionFor(kHashSha256)) == L".sha256" && std::wstring(SumExtensionFor(kHashCrc32)) == L".sfv");
    }

    // ---- reading ----
    {
        const std::string a = Sha("A"), b = Sha("B");
        // GNU, BSD, comments, a blank line, CRLF, a name with spaces, a line that is nothing, a BOM
        WriteFile(root / "r1.sha256", "\xEF\xBB\xBF# made by a tool\r\n" + a + " *dir/a one.txt\r\n\r\n" + b + "  b.txt\r\nSHA256 (c.txt) = " + a + "\r\nnonsense line\r\n");
        auto s = ReadSumFile(root / "r1.sha256");
        EXPECT(s.error == SumFileError::None && s.entries.size() == 3 && s.badLines == 1);
        EXPECT(s.entries.size() == 3 && s.entries[0].name == L"dir/a one.txt" && s.entries[0].hex == a && s.entries[0].algo == kHashSha256);
        EXPECT(s.entries.size() == 3 && s.entries[1].name == L"b.txt" && s.entries[2].name == L"c.txt");
        // .sfv: the CRC is the last word, ';' is a comment
        WriteFile(root / "r2.sfv", "; generated\n; more\nsub dir/file one.bin CBF43926\nlast.bin 352441c2\n");
        s = ReadSumFile(root / "r2.sfv");
        EXPECT(s.error == SumFileError::None && s.entries.size() == 2 && s.entries[0].name == L"sub dir/file one.bin" && s.entries[0].hex == "cbf43926" &&
               s.entries[0].algo == kHashCrc32 && s.entries[1].hex == "352441c2");
        // md5 by the length of the value
        WriteFile(root / "r3.md5", "900150983cd24fb0d6963f7d28e17f72 *abc.txt\n");
        s = ReadSumFile(root / "r3.md5");
        EXPECT(s.entries.size() == 1 && s.entries[0].algo == kHashMd5);
        // UTF-8 names, and a name in the ANSI code page (older tools): the line is still read
        WriteFile(root / "r4.sha256", a + " *\xE8\xB3\x87\xE6\x96\x99/\xE6\xAA\x94.txt\n");
        s = ReadSumFile(root / "r4.sha256");
        EXPECT(s.entries.size() == 1 && s.entries[0].name == L"資料/檔.txt");
        // nothing usable, not there
        WriteFile(root / "r5.sha256", "just text\nmore text\n");
        EXPECT(ReadSumFile(root / "r5.sha256").error == SumFileError::NoLines);
        EXPECT(ReadSumFile(root / "nowhere.sha256").error == SumFileError::Unreadable);
        WriteFile(root / "r6.sha256", "");
        EXPECT(ReadSumFile(root / "r6.sha256").error == SumFileError::NoLines);
    }

    // ---- files on disk ----
    const fs::path tree = root / L"樹 data";
    {
        WriteFile(tree / "a.txt", "alpha");
        WriteFile(tree / "sub" / "b one.txt", "bravo");
        WriteFile(tree / "sub" / L"深" / "c.bin", std::string(300000, 'c'));
        WriteFile(tree / "z.txt", "zulu");

        // a folder → one checksum file, then check it: everything matches
        const fs::path sums = root / L"樹 data.sha256";
        auto w = WriteSumFile(sums, {tree}, root);
        EXPECT(w.written.size() == 1 && w.skipped == 0 && fs::exists(sums));
        const std::string text = ReadAll(sums);
        EXPECT(text.find("\xEF\xBB\xBF") == std::string::npos);                       // no BOM
        EXPECT(text.find(" *\xE6\xA8\xB9 data/a.txt\n") != std::string::npos);        // '/' and relative to the base
        EXPECT(text.find("sub/\xE6\xB7\xB1/c.bin") != std::string::npos);
        auto rep = VerifySumFile(ReadSumFile(sums));
        EXPECT(rep.allMatch() && rep.total == 4 && rep.matched == 4 && rep.problems.empty() && rep.bytes == 5 + 5 + 300000 + 4);

        // one changed, one removed, one that cannot be a file (a folder)
        WriteFile(tree / "a.txt", "ALPHA");
        fs::remove(tree / "z.txt");
        auto rep2 = VerifySumFile(ReadSumFile(sums));
        EXPECT(!rep2.allMatch() && rep2.total == 4 && rep2.matched == 2 && rep2.mismatched == 1 && rep2.missing == 1 && rep2.problems.size() == 2);
        const auto* bad = Find(rep2, L"樹 data/a.txt");
        EXPECT(bad && bad->status == SumStatus::Mismatch && bad->expected == Sha("alpha") && bad->actual == Sha("ALPHA"));
        const auto* gone = Find(rep2, L"樹 data/z.txt");
        EXPECT(gone && gone->status == SumStatus::Missing);
        WriteFile(tree / "a.txt", "alpha");
        WriteFile(tree / "z.txt", "zulu");
        {
            std::ofstream(root / "dir.sha256") << Sha("x") << " *樹 data\n";   // a folder is listed as a file
            auto rep3 = VerifySumFile(ReadSumFile(root / "dir.sha256"));
            EXPECT(rep3.errors == 1 && rep3.problems.size() == 1 && rep3.problems[0].status == SumStatus::Error);
        }

        // sums in the folder of the files: names relative to the checksum file, a mix of algorithms
        WriteFile(tree / "mix.sha256", Sha("alpha") + " *a.txt\n" + HashHex(kHashMd5, "zulu", 4) + " *z.txt\n" + "CRC32 (sub/b one.txt) = " + HashHex(kHashCrc32, "bravo", 5) + "\n");
        auto rep4 = VerifySumFile(ReadSumFile(tree / "mix.sha256"));
        EXPECT(rep4.allMatch() && rep4.total == 3);

        // sfv written and read back
        const fs::path sfv = tree / "tree.sfv";
        WriteSumFile(sfv, {tree / "a.txt", tree / "z.txt"}, tree, {kHashCrc32, {}});
        EXPECT(ReadAll(sfv) == "a.txt " + HashHex(kHashCrc32, "alpha", 5) + "\nz.txt " + HashHex(kHashCrc32, "zulu", 4) + "\n");
        EXPECT(VerifySumFile(ReadSumFile(sfv)).allMatch());

        // an unreadable checksum file is reported, not thrown
        auto rep5 = VerifySumFile(ReadSumFile(root / "nowhere.sha256"));
        EXPECT(rep5.error == SumFileError::Unreadable && !rep5.allMatch());

        // cancel: an exception, and no checksum file is left
        const fs::path never = root / "never.sha256";
        bool cancelled = false;
        try {
            WriteSumFile(never, {tree}, root, {kHashSha256, [](uint64_t, uint64_t) { return false; }});
        } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT(cancelled && !fs::exists(never) && !fs::exists(never.wstring() + L".lycan-tmp"));
    }

    // ---- an archive and the checksum files next to it ----
    {
        const fs::path src = root / "proj";
        WriteFile(src / "readme.txt", "read me");
        WriteFile(src / "src" / "main.cpp", "int main() {}");
        WriteFile(src / "src" / "util.h", "#pragma once");
        const fs::path arc = root / "out" / "proj.zip";
        fs::create_directories(arc.parent_path());
        CompressOptions co;
        EXPECT(CreateArchive(arc, {src}, co).ok());

        // both checksum files at once
        auto w = WriteArchiveSums(arc, {src}, true, true);
        EXPECT(w.written.size() == 2 && w.skipped == 0);
        EXPECT(fs::exists(root / "out" / "proj.zip.sha256") && fs::exists(root / "out" / "proj.sha256"));
        const std::string contents = ReadAll(root / "out" / "proj.sha256");
        EXPECT(contents.find(" *proj/src/main.cpp\n") != std::string::npos);   // the names the archive has
        auto car = FindArchiveSidecars(arc);
        EXPECT(car.archiveSum == root / "out" / "proj.zip.sha256" && car.contentsSum == root / "out" / "proj.sha256");

        // the archive file against its own checksum file
        auto a1 = VerifySumFile(ReadSumFile(car.archiveSum));
        EXPECT(a1.allMatch() && a1.total == 1);

        // the files inside against the archive: a pass over the archive, nothing is written
        auto src1 = ArchiveSource::Open(arc, {});
        auto r1 = VerifySumAgainstArchive(ReadSumFile(car.contentsSum), *src1);
        EXPECT(r1.allMatch() && r1.total == 3 && r1.extra.empty() && r1.sumSkip == 0 && r1.entrySkip == 0 && r1.confident);

        // the archive has one more file, the checksum file mentions one that is not there, one value is wrong
        WriteFile(root / "x.sha256", Sha("read me") + " *proj/readme.txt\n" + Sha("WRONG") + " *proj/src/main.cpp\n" + Sha("q") + " *proj/gone.txt\n");
        auto r2 = VerifySumAgainstArchive(ReadSumFile(root / "x.sha256"), *src1);
        EXPECT(!r2.allMatch() && r2.matched == 1 && r2.mismatched == 1 && r2.missing == 1);
        EXPECT(r2.extra.size() == 1 && r2.extra[0] == L"proj/src/util.h");
        EXPECT(Find(r2, L"proj/gone.txt") && Find(r2, L"proj/gone.txt")->status == SumStatus::Missing);

        // names that need a guess: a checksum file made inside the folder (no "proj/"), and one made a level higher ("x/proj/...")
        WriteFile(root / "inner.sha256", Sha("read me") + " *readme.txt\n" + Sha("int main() {}") + " *src/main.cpp\n" + Sha("#pragma once") + " *src/util.h\n");
        auto r3 = VerifySumAgainstArchive(ReadSumFile(root / "inner.sha256"), *src1);
        EXPECT(r3.allMatch() && r3.entrySkip == 1 && r3.sumSkip == 0 && r3.confident);
        WriteFile(root / "outer.sha256", Sha("read me") + " *x\\proj\\readme.txt\n" + Sha("int main() {}") + " *x\\proj\\src\\main.cpp\n" + Sha("#pragma once") + " *./x/proj/src/UTIL.h\n");
        auto r4 = VerifySumAgainstArchive(ReadSumFile(root / "outer.sha256"), *src1);
        EXPECT(r4.allMatch() && r4.sumSkip == 1 && r4.entrySkip == 0);   // backslashes, "./" and case do not matter

        // names that cannot be guessed: not confident, and a prefix given by the user fixes it
        WriteFile(root / "weird.sha256", Sha("read me") + " *a/b/c/d/readme.txt\n" + Sha("int main() {}") + " *a/b/c/d/src/main.cpp\n");
        auto r5 = VerifySumAgainstArchive(ReadSumFile(root / "weird.sha256"), *src1);
        EXPECT(!r5.confident && r5.matched == 0 && r5.missing == 2);
        ArchiveSumOptions fix;
        fix.manual = true;
        fix.stripPrefix = L"a/b/c/d/";
        fix.addPrefix = L"proj/";
        auto r6 = VerifySumAgainstArchive(ReadSumFile(root / "weird.sha256"), *src1, {}, fix);
        EXPECT(r6.matched == 2 && r6.missing == 0 && r6.confident && r6.extra.size() == 1);

        // the checksum file inside the archive is not an "extra" file
        WriteFile(src / "proj.sha256", "x");
        const fs::path arc2 = root / "out" / "withsums.zip";
        EXPECT(CreateArchive(arc2, {src}, co).ok());
        auto src2 = ArchiveSource::Open(arc2, {});
        auto r7 = VerifySumAgainstArchive(ReadSumFile(root / "out" / "proj.sha256"), *src2);
        EXPECT(r7.extra.empty() && r7.matched == 3);

        // only the archive's checksum file / only the files': the other is not made
        const fs::path arc3 = root / "out" / "only.7z";
        co.type = ArchiveType::SevenZip;
        EXPECT(CreateArchive(arc3, {src / "src"}, co).ok());
        auto w3 = WriteArchiveSums(arc3, {src / "src"}, false, true);
        EXPECT(w3.written.size() == 1 && fs::exists(root / "out" / "only.sha256") && !fs::exists(root / "out" / "only.7z.sha256"));
        auto src3 = ArchiveSource::Open(arc3, {});
        EXPECT(VerifySumAgainstArchive(ReadSumFile(root / "out" / "only.sha256"), *src3).allMatch());
        EXPECT(WriteArchiveSums(arc3, {src / "src"}, false, false).written.empty());

        // an archive in volumes: the checksum file of the archive has one line per volume (named after the first volume)
        {
            const fs::path big = root / "bigsrc";
            std::string data(300000, 'q');
            std::mt19937 rng(7);
            for (auto& c : data) c = static_cast<char>(rng());   // not compressible: the volumes fill up
            WriteFile(big / "data.bin", data);
            const fs::path vol = root / "out" / "vol.7z";
            CompressOptions vo;
            vo.type = ArchiveType::SevenZip;
            vo.volumeSize = 100000;
            auto vrep = CreateArchive(vol, {big}, vo);
            EXPECT(vrep.ok() && vrep.outputs.size() >= 3);
            auto vw = WriteArchiveSums(vrep.outputs[0], {big}, true, false, {}, vrep.outputs);
            EXPECT(vw.written.size() == 1 && vw.written[0] == fs::path(vrep.outputs[0].wstring() + L".sha256"));
            auto vs = ReadSumFile(vw.written[0]);
            EXPECT(vs.entries.size() == vrep.outputs.size());
            auto vr = VerifySumFile(vs);
            EXPECT(vr.allMatch() && vr.total == vrep.outputs.size());
            EXPECT(FindArchiveSidecars(vrep.outputs[0]).archiveSum == vw.written[0]);
        }

        // cancel: nothing is left
        const fs::path arc4 = root / "out" / "cancel.zip";
        co.type = ArchiveType::Zip;
        EXPECT(CreateArchive(arc4, {src}, co).ok());
        bool cancelled = false;
        try {
            WriteArchiveSums(arc4, {src}, true, true, {kHashSha256, [](uint64_t, uint64_t) { return false; }});
        } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT(cancelled && !fs::exists(root / "out" / "cancel.zip.sha256") && !fs::exists(root / "out" / "cancel.sha256"));
        EXPECT(FindArchiveSidecars(arc4).archiveSum.empty() && FindArchiveSidecars(arc4).contentsSum.empty());
    }

    // ---- folders are expanded, sorted, in all levels ----
    {
        auto files = ExpandForHash({tree / "a.txt", tree / "sub"});
        EXPECT(files.size() == 3 && files[0] == tree / "a.txt" && files[1] == tree / "sub" / "b one.txt" && files[2] == tree / "sub" / L"深" / "c.bin");
    }

    fs::remove_all(root, ec);
    if (failures == 0) std::puts("test_checksum: all passed");
    return failures ? 1 : 0;
}

int main() {
    try {
        return Run();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "EXCEPTION: %s\n", e.what());
        return 1;
    }
}
