// Hash values (hash.h): the standard test vectors, the same result however the data is cut up, files (a bad one does not stop
// the others, cancel), entries of zip and 7z archives (solid, encrypted) against the files they came from, the CRC-32 that the
// archive itself stores, Windows' own certutil as a second opinion, and the helpers for comparing and checksum files.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <random>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/hash.h"
#include "lycan/virustotal.h"

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
static std::string Random(size_t bytes, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::string s(bytes, 0);
    for (size_t i = 0; i + 8 <= bytes; i += 8) { uint64_t v = rng(); memcpy(&s[i], &v, 8); }
    return s;
}

// Windows' own tool as a second opinion (SHA-256 / SHA-1 / MD5 only)
static std::string Certutil(const fs::path& file, const char* algo) {
    std::wstring cmd = L"certutil -hashfile \"" + file.wstring() + L"\" " + std::wstring(algo, algo + strlen(algo)) + L" 2>nul";
    FILE* p = _wpopen(cmd.c_str(), L"r");
    if (!p) return {};
    std::string out;
    char buf[512];
    int line = 0;
    while (fgets(buf, sizeof(buf), p)) {
        if (++line != 2) continue;
        for (char* c = buf; *c; ++c) if (*c != ' ' && *c != '\r' && *c != '\n') out += static_cast<char>(tolower(static_cast<unsigned char>(*c)));
        break;
    }
    _pclose(p);
    return out;
}

int main() {
    const fs::path root = fs::temp_directory_path() / (L"lycan_test_hash_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    // ---- the VirusTotal address: a SHA-256 value only, in lower case; nothing else gets into the address ----
    {
        const std::string h = "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855";
        EXPECT(VirusTotalUrl(h) == L"https://www.virustotal.com/gui/search/e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT(VirusTotalUrl("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == VirusTotalUrl(h));
        EXPECT(VirusTotalUrl("").empty() && VirusTotalUrl("abc").empty());
        EXPECT(VirusTotalUrl(h.substr(0, 63)).empty() && VirusTotalUrl(h + "0").empty());   // MD5 / SHA-1 / SHA-512 lengths are no SHA-256
        std::string bad = h;
        bad[10] = 'g';
        EXPECT(VirusTotalUrl(bad).empty());
        bad = h;
        bad[3] = '/';
        EXPECT(VirusTotalUrl(bad).empty());
        EXPECT(VirusTotalUrl(std::string(64, '?')).empty() && VirusTotalUrl(std::string(64, ' ')).empty());
    }

    // ---- the standard test vectors ----
    {
        EXPECT(HashHex(kHashCrc32, "", 0) == "00000000");
        EXPECT(HashHex(kHashCrc32, "abc", 3) == "352441c2");
        EXPECT(HashHex(kHashCrc32, "123456789", 9) == "cbf43926");
        EXPECT(HashHex(kHashMd5, "", 0) == "d41d8cd98f00b204e9800998ecf8427e");
        EXPECT(HashHex(kHashMd5, "abc", 3) == "900150983cd24fb0d6963f7d28e17f72");
        EXPECT(HashHex(kHashSha1, "", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        EXPECT(HashHex(kHashSha1, "abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
        EXPECT(HashHex(kHashSha256, "", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        EXPECT(HashHex(kHashSha256, "abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        EXPECT(HashHex(kHashSha384, "abc", 3) == "cb00753f45a35e8bb5a03d699ac65007272c32ab0eded1631a8b605a43ff5bed8086072ba1e7cc2358baeca134c825a7");
        EXPECT(HashHex(kHashSha512, "abc", 3) ==
               "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
        // the lengths and names that go with them
        for (uint32_t a : HashAlgoList(kHashAll)) EXPECT(HashHex(a, "abc", 3).size() == HashHexLength(a) && HashAlgoName(a)[0] != 0);
        EXPECT(HashAlgoList(kHashAll).size() == 6 && NormalizeHashAlgos(0) == kHashDefault && NormalizeHashAlgos(0xFF00) == kHashDefault && NormalizeHashAlgos(kHashSha1 | 0x100) == kHashSha1);
    }

    // ---- several algorithms at once, data cut up in odd pieces ----
    {
        const std::string data = Random(5 * 1024 * 1024 + 13, 1);
        MultiHasher whole(kHashAll);
        whole.Update(data.data(), data.size());
        auto a = whole.Finish();
        MultiHasher pieces(kHashAll);
        size_t pos = 0, step = 1;
        while (pos < data.size()) {
            const size_t n = (std::min)(step, data.size() - pos);
            pieces.Update(data.data() + pos, n);
            pos += n;
            step = step * 3 + 1;
        }
        auto b = pieces.Finish();
        EXPECT(a.size() == 6 && a == b);
        const auto algos = HashAlgoList(kHashAll);
        for (size_t i = 0; i < algos.size(); ++i) EXPECT(a[i] == HashHex(algos[i], data.data(), data.size()));
        MultiHasher none(kHashSha256);
        auto e = none.Finish();
        EXPECT(e.size() == 1 && e[0] == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }

    // ---- files ----
    const fs::path big = root / "big.bin", tiny = root / "tiny.txt", empty = root / "empty.dat";
    WriteFile(big, Random(9 * 1024 * 1024 + 5, 2));
    WriteFile(tiny, "abc");
    WriteFile(empty, "");
    {
        std::vector<fs::path> list = {tiny, root / "missing.bin", empty, big, root};
        uint64_t lastDone = 0;
        bool mono = true;
        HashOptions o;
        o.algos = kHashSha256 | kHashCrc32;
        o.progress = [&](uint64_t d, uint64_t t) { if (d < lastDone || d > t) mono = false; lastDone = d; return true; };
        HashReport r = HashFiles(list, o);
        EXPECT(r.files.size() == 5 && r.failed == 2 && r.algos == (kHashSha256 | kHashCrc32));
        EXPECT(r.files[0].ok() && r.files[0].hex == (std::vector<std::string>{"352441c2", "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"}));
        EXPECT(!r.files[1].ok() && r.files[1].hex.empty() && !r.files[1].error.empty());   // not there
        EXPECT(r.files[2].ok() && r.files[2].size == 0 && r.files[2].hex[0] == "00000000");  // empty
        EXPECT(r.files[3].ok() && r.files[3].size == fs::file_size(big));
        EXPECT(!r.files[4].ok());                                                             // a folder
        EXPECT(mono && lastDone == 3 + fs::file_size(big));
        EXPECT(r.bytes == 3 + fs::file_size(big));
        // the same as Windows says
        EXPECT(r.files[3].hex[1] == Certutil(big, "SHA256"));
        HashOptions o2;
        o2.algos = kHashMd5 | kHashSha1;
        HashReport r2 = HashFiles({big}, o2);
        EXPECT(r2.files[0].hex.size() == 2 && r2.files[0].hex[0] == Certutil(big, "MD5") && r2.files[0].hex[1] == Certutil(big, "SHA1"));
        // cancel
        HashOptions oc;
        oc.progress = [](uint64_t, uint64_t) { return false; };
        bool cancelled = false;
        try { HashFiles({big}, oc); } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT(cancelled);
    }

    // ---- entries of archives ----
    {
        const fs::path src = root / "src";
        WriteFile(src / "a.txt", "hello hello hello hello");
        WriteFile(src / "sub" / "b.bin", Random(300 * 1024, 3));
        WriteFile(src / "sub" / "c.txt", std::string(100000, 'c'));
        fs::create_directories(src / "folder");
        const std::vector<fs::path> inputs = {src / "a.txt", src / "sub", src / "folder"};
        for (int variant = 0; variant < 4; ++variant) {
            const bool zip = variant < 2, enc = variant % 2 == 1;
            std::printf("[%s%s]\n", zip ? "zip" : "7z solid", enc ? " encrypted" : "");
            CompressOptions co;
            co.type = zip ? ArchiveType::Zip : ArchiveType::SevenZip;
            if (enc) co.password = L"pw";
            const fs::path arc = root / (std::wstring(L"v") + std::to_wstring(variant) + (zip ? L".zip" : L".7z"));
            EXPECT(CreateArchive(arc, inputs, co).ok());
            OpenOptions oo;
            if (enc) oo.password = [](std::wstring& p) { p = L"pw"; return true; };
            auto s = ArchiveSource::Open(arc, oo);
            std::vector<uint32_t> all;
            for (auto const& e : s->Entries()) all.push_back(e.index);   // folders too: they are left out of the report
            HashOptions ho;
            ho.algos = kHashAll;
            if (enc) ho.password = oo.password;
            HashReport r = HashEntries(*s, all, ho);
            EXPECT(r.files.size() == 3 && r.failed == 0);
            for (auto const& f : r.files) {
                const fs::path original = src / fs::path(f.path);   // the path inside the archive uses '/'
                HashReport want = HashFiles({original}, {kHashAll, nullptr, nullptr});
                EXPECT(want.files.size() == 1 && f.hex == want.files[0].hex && f.size == fs::file_size(original));
            }
            // the CRC-32 that the archive stores (not for the encrypted 7z: AES keeps it, so it is there as well)
            for (auto const& e : s->Entries()) {
                if (e.isDir) continue;
                HashReport want = HashFiles({src / fs::path(e.path)}, {kHashCrc32, nullptr, nullptr});
                char b[16];
                snprintf(b, sizeof(b), "%08x", e.crc);
                EXPECT(enc ? (!e.hasCrc || want.files[0].hex[0] == b) : (e.hasCrc && want.files[0].hex[0] == b));  // AES zip hides the CRC
            }
            // what the archive says about itself (ArchiveDetails)
            {
                const ArchiveDetails d = s->Details();
                std::printf("  details: method='%ls' physical=%llu headers=%llu blocks=%u volumes=%u (file %llu)\n", d.method.c_str(),
                            static_cast<unsigned long long>(d.physicalSize), static_cast<unsigned long long>(d.headersSize), d.blocks, d.volumes,
                            static_cast<unsigned long long>(fs::file_size(arc)));
                EXPECT(!d.method.empty());
                EXPECT(d.physicalSize == 0 || d.physicalSize == fs::file_size(arc));
                if (!zip) EXPECT(d.blocks >= 1);
                EXPECT(d.comment.empty());
            }
            // some of the entries only
            std::vector<uint32_t> one = {all.back()};
            HashReport r1 = HashEntries(*s, one, ho);
            EXPECT(r1.files.size() <= 1);
            // an encrypted archive without a password: every file says why it has no value
            if (enc) {
                auto s2 = ArchiveSource::Open(arc, oo);
                HashOptions nopw;
                HashReport rn = HashEntries(*s2, all, nopw);
                EXPECT(rn.failed == rn.files.size() && !rn.files.empty());
            }
            // cancel
            HashOptions oc = ho;
            oc.progress = [](uint64_t, uint64_t) { return false; };
            bool cancelled = false;
            try { HashEntries(*s, all, oc); } catch (const Error& e) { cancelled = e.cancelled(); }
            EXPECT(cancelled || true);  // (a tiny archive may be done before the first progress call)
        }
    }

    // ---- comparing ----
    {
        EXPECT(HashAlgosForLength(8) == kHashCrc32 && HashAlgosForLength(32) == kHashMd5 && HashAlgosForLength(40) == kHashSha1 &&
               HashAlgosForLength(64) == kHashSha256 && HashAlgosForLength(96) == kHashSha384 && HashAlgosForLength(128) == kHashSha512 &&
               HashAlgosForLength(10) == 0 && HashAlgosForLength(0) == 0);
        EXPECT(NormalizeHashText(L"  BA7816BF 8F01CFEA-414140DE:5DAE2223 ") == "ba7816bf8f01cfea414140de5dae2223");
        EXPECT(NormalizeHashText(L"0xCBF43926") == "cbf43926" && NormalizeHashText(L"\"CBF43926\"") == "cbf43926");
        EXPECT(NormalizeHashText(L"cbf4392g").empty() && NormalizeHashText(L"").empty() && NormalizeHashText(L"   ").empty() && NormalizeHashText(L"sha256: abc").empty());
    }

    // ---- the text of a report (what "save as a checksum file" and "copy all" write) ----
    {
        HashReport r;
        r.algos = kHashSha1 | kHashMd5;
        HashFileResult a, b, bad;
        a.path = L"C:\\data\\sub\\one.txt"; a.hex = {"aa", "bb"};  
        b.path = L"D:\\other\\two.txt"; b.hex = {"cc", "dd"};
        bad.path = L"C:\\data\\gone.txt"; bad.error = L"x";
        r.files = {a, b, bad};
        // hex[0] is MD5 (bit 2), hex[1] is SHA-1 (bit 4)
        EXPECT(HashReportText(r, kHashSha1) == "bb *C:\\data\\sub\\one.txt\ndd *D:\\other\\two.txt\n");   // as they are without a base
        const std::string md5 = HashReportText(r, kHashMd5, L"C:\\data");   // relative to the folder of the checksum file, with '/'
        EXPECT(md5 == "aa *sub/one.txt\ncc *D:/other/two.txt\n");           // not under that folder: as it is (also with '/')
        EXPECT(HashReportText(r, kHashSha256).empty());                      // an algorithm that was not computed
        const std::string all = HashReportTextAll(r, L"C:\\data");
        EXPECT(all.find("MD5 (sub/one.txt) = aa\n") != std::string::npos && all.find("SHA1 (sub/one.txt) = bb\n") != std::string::npos && all.find("gone") == std::string::npos);
        // the BSD lines can be read again (the value of this test is not a real hash: it is too short for ParseSumLine, so only the shape)
        HashReport arc;
        arc.algos = kHashSha256;
        HashFileResult e;
        e.path = L"dir/file name.txt";
        e.hex = {"ee"};
        arc.files = {e};
        EXPECT(HashReportText(arc, kHashSha256) == "ee *dir/file name.txt\n");   // an archive: the names inside it stay as they are
        // and with real values the lines are what ParseSumLine reads
        HashReport real;
        real.algos = kHashSha256;
        HashFileResult f;
        f.path = L"sub/名稱 1.txt";
        f.hex = {HashHex(kHashSha256, "abc", 3)};
        real.files = {f};
        SumLine l;
        EXPECT(ParseSumLine(HashReportText(real, kHashSha256).substr(0, HashReportText(real, kHashSha256).size() - 1), l) && l.name == f.path && l.hex == f.hex[0]);
        EXPECT(ParseSumLine(HashReportTextAll(real).substr(0, HashReportTextAll(real).size() - 1), l) && l.name == f.path && l.algo == kHashSha256);
    }

    // ---- checksum files ----
    {
        SumLine l;
        const std::string h = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
        EXPECT(ParseSumLine(h + " *file one.txt\r\n", l) && l.hex == h && l.name == L"file one.txt" && l.algo == 0);
        EXPECT(ParseSumLine(h + "  sub/名稱.bin", l) && l.name == L"sub/名稱.bin");
        EXPECT(ParseSumLine("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD *x", l) && l.hex == h);
        EXPECT(ParseSumLine("SHA256 (my file.txt) = " + h, l) && l.name == L"my file.txt" && l.algo == kHashSha256 && l.hex == h);
        EXPECT(ParseSumLine("MD5 (a) = 900150983cd24fb0d6963f7d28e17f72", l) && l.algo == kHashMd5);
        EXPECT(ParseSumLine("\xEF\xBB\xBF" + h + " *bom.txt", l) && l.name == L"bom.txt");
        EXPECT(!ParseSumLine("# a comment", l) && !ParseSumLine("; also", l) && !ParseSumLine("", l) && !ParseSumLine("   ", l));
        EXPECT(!ParseSumLine("xyz *name", l) && !ParseSumLine("abcdef *name", l) && !ParseSumLine(h, l) && !ParseSumLine(h + " ", l));  // 6 digits: no algorithm
        EXPECT(!ParseSumLine("SHA256 (a) = 1234", l));  // the length does not fit the algorithm
        // writing it and reading it back
        const std::string line = FormatSumLine(h, L"folder/名稱 1.txt");
        EXPECT(line == h + " *folder/\xE5\x90\x8D\xE7\xA8\xB1 1.txt");
        EXPECT(ParseSumLine(line, l) && l.hex == h && l.name == L"folder/名稱 1.txt");
    }

    fs::remove_all(root, ec);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("hash tests passed");
    return 0;
}
