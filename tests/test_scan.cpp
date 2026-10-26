// "Scan for malware" (scan.h): the pipeline with a stand-in scanner (what is looked at, what is skipped, what is reported), and
// -- only when LYCAN_TEST_AMSI=1 -- the real AMSI of this machine on a clean archive. No malware and no EICAR file is ever
// written: detections are simulated by a marker in the content.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/scan.h"

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

static const std::string kMarker = "LYCAN-TEST-THREAT";

static ScanFn Marker(std::vector<std::wstring>* seen = nullptr) {
    return [seen](const std::wstring& name, const void* data, size_t size) {
        if (seen) seen->push_back(name);
        const std::string s(static_cast<const char*>(data), size);
        return s.find(kMarker) != std::string::npos ? ScanVerdict::Detected : ScanVerdict::Clean;
    };
}

int main() {
    const fs::path root = fs::temp_directory_path() / (L"lycan_test_scan_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    const fs::path src = root / "src";
    WriteFile(src / "clean.txt", std::string(5000, 'a'));
    WriteFile(src / "sub" / "evil.txt", "xx " + kMarker + " xx");
    WriteFile(src / "empty.txt", "");
    WriteFile(src / "big.bin", std::string(4000, 'b'));
    fs::create_directories(src / "folder");
    const std::vector<fs::path> inputs = {src / "clean.txt", src / "sub", src / "empty.txt", src / "big.bin", src / "folder"};

    for (ArchiveType type : {ArchiveType::Zip, ArchiveType::SevenZip}) {
        const wchar_t* ext = type == ArchiveType::Zip ? L".zip" : L".7z";
        std::printf("[%s]\n", type == ArchiveType::Zip ? "zip" : "7z solid");
        const fs::path arc = root / (std::wstring(L"a") + ext);
        CompressOptions co;
        co.type = type;
        CreateArchive(arc, inputs, co);
        auto s = ArchiveSource::Open(arc);

        // everything is looked at once; the empty file and the folders are not files worth a scan
        std::vector<std::wstring> seen;
        ScanOptions so;
        so.scanner = Marker(&seen);
        ScanReport r = ScanArchive(*s, so);
        EXPECT(r.amsiHr == 0 && r.files == 3 && r.Skipped() == 0 && r.unreadable == 0 && r.scanErrors == 0);
        EXPECT(r.bytes == 5000 + 4000 + (3 + kMarker.size() + 3));
        EXPECT(r.threats.size() == 1 && r.threats[0].path == L"sub/evil.txt" && !r.threats[0].blocked && r.Found());
        // the content name carries the archive and the path inside it, with backslashes
        EXPECT(std::find(seen.begin(), seen.end(), std::wstring(L"a") + ext + L"\\sub\\evil.txt") != seen.end());
        EXPECT(seen.size() == 3);

        // the name that Defender logged (the log is a test hook here)
        {
            auto det = [](std::vector<std::wstring> names) {
                return [names](uint64_t) {
                    std::vector<DefenderDetection> v;
                    uint64_t t = 1;
                    for (auto& n : names) { DefenderDetection d; d.time = t++; d.name = n; d.severity = L"high"; d.category = L"tool"; v.push_back(d); }
                    return v;
                };
            };
            ScanOptions no = so;
            no.lookupWaitMs = 0;
            no.detections = det({L"Trojan:Test/One"});  // one entry for the one detected file: it is that file's name
            r = ScanArchive(*s, no);
            EXPECT(r.threats.size() == 1 && r.threats[0].name == L"Trojan:Test/One" && r.threats[0].severity == L"high" && r.threats[0].category == L"tool");
            EXPECT(r.threatNames == std::vector<std::wstring>{L"Trojan:Test/One"});
            no.detections = det({});  // another antivirus product: no names at all
            r = ScanArchive(*s, no);
            EXPECT(r.threats.size() == 1 && r.threats[0].name.empty() && r.threatNames.empty());
            no.detections = det({L"A", L"A"});  // two entries, one file, one name: still the name of the file
            r = ScanArchive(*s, no);
            EXPECT(r.threats[0].name == L"A" && r.threatNames.size() == 1);
            no.detections = det({L"A", L"B"});  // two different names for one file: only the list is known
            r = ScanArchive(*s, no);
            EXPECT(r.threats[0].name.empty() && r.threatNames.size() == 2);
            bool asked = false;
            no.detections = [&asked](uint64_t) { asked = true; return std::vector<DefenderDetection>{}; };
            no.scanner = [](const std::wstring&, const void*, size_t) { return ScanVerdict::Blocked; };  // policy blocks are not Defender's
            r = ScanArchive(*s, no);
            EXPECT(!asked && r.threats[0].name.empty());
            // the wait: the log catches up after a while
            int calls = 0;
            no.scanner = Marker();
            no.lookupWaitMs = 2000;
            no.detections = [&calls](uint64_t) {
                std::vector<DefenderDetection> v;
                if (++calls >= 3) { DefenderDetection d; d.name = L"Late"; v.push_back(d); }
                return v;
            };
            r = ScanArchive(*s, no);
            EXPECT(calls == 3 && r.threats[0].name == L"Late");
        }

        // a limit: what is bigger is skipped and listed, never counted as scanned; exactly the limit is scanned
        so.maxFileBytes = 4000;
        r = ScanArchive(*s, so);
        EXPECT(r.files == 2 && r.skippedLarge == 1);  // big.bin is exactly 4000: scanned; clean.txt (5000) is over
        so.maxFileBytes = 3999;
        r = ScanArchive(*s, so);
        EXPECT(r.skippedLarge == 2 && r.files == 1 && r.threats.size() == 1);  // big.bin (4000) and clean.txt (5000) are over; evil.txt is 3 + 17 + 3 bytes
        EXPECT(r.notes.size() == 2 && r.notes[0].kind == ScanNote::Kind::TooLarge);

        // the scanner failing on a file, and a policy block
        so.maxFileBytes = kAmsiMaxFileBytes;
        so.scanner = [](const std::wstring&, const void*, size_t) { return ScanVerdict::Failed; };
        r = ScanArchive(*s, so);
        EXPECT(r.scanErrors == 3 && r.files == 0 && !r.Found());
        so.scanner = [](const std::wstring&, const void*, size_t) { return ScanVerdict::Blocked; };
        r = ScanArchive(*s, so);
        EXPECT(r.threats.size() == 3 && r.threats[0].blocked);

        // cancel
        so.scanner = Marker();
        so.progress = [](uint64_t, uint64_t) { return false; };
        bool cancelled = false;
        try { ScanArchive(*s, so); } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT(cancelled);
    }

    // encrypted: skipped without the password, scanned with it, unreadable with a wrong one
    {
        std::printf("[encrypted]\n");
        const fs::path arc = root / "enc.zip";
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.password = L"Secret1";
        CreateArchive(arc, {src / "clean.txt", src / "sub"}, co);
        auto s = ArchiveSource::Open(arc);
        ScanOptions so;
        so.scanner = Marker();
        ScanReport r = ScanArchive(*s, so);
        EXPECT(r.files == 0 && r.skippedEncrypted == 2 && r.notes.size() == 2 && r.notes[0].kind == ScanNote::Kind::Encrypted);
        so.password = [](std::wstring& p) { p = L"Secret1"; return true; };
        r = ScanArchive(*s, so);
        EXPECT(r.files == 2 && r.skippedEncrypted == 0 && r.threats.size() == 1);
        so.password = [](std::wstring& p) { p = L"Wrong99"; return true; };
        r = ScanArchive(*s, so);
        EXPECT(r.files == 0 && r.unreadable == 2 && !r.Found());
    }

    // the environment switch used by the UI tests (the worker process inherits it)
    {
        std::printf("[fake via environment]\n");
        _putenv_s("LYCAN_TEST_SCAN_FAKE", "1");
        const fs::path arc = root / "a.zip";
        auto s = ArchiveSource::Open(arc);
        ScanReport r = ScanArchive(*s, {});
        EXPECT(r.amsiHr == 0 && r.files == 3 && r.threats.size() == 1);
        _putenv_s("LYCAN_TEST_SCAN_FAKE", "");
    }

    EXPECT(ClassifyAmsiError(0) == AmsiProblem::None);
    EXPECT(ClassifyAmsiError(static_cast<long>(0x80070015L)) == AmsiProblem::RealTimeOff);
    EXPECT(ClassifyAmsiError(static_cast<long>(0x80070103L)) == AmsiProblem::NoProvider);
    EXPECT(ClassifyAmsiError(static_cast<long>(0x8007007EL)) == AmsiProblem::Unavailable);

    // the real AMSI of this machine (opt-in: it talks to the installed antivirus): a clean archive has to come back clean
    wchar_t flag[8];
    if (GetEnvironmentVariableW(L"LYCAN_TEST_AMSI", flag, 8) > 0) {
        std::printf("[real AMSI]\n");
        const fs::path arc = root / "clean_only.zip";
        CompressOptions co;
        co.type = ArchiveType::Zip;
        CreateArchive(arc, {src / "clean.txt", src / "big.bin"}, co);
        auto s = ArchiveSource::Open(arc);
        ScanReport r = ScanArchive(*s, {});
        if (r.amsiHr != 0) std::printf("   AMSI is not usable here: 0x%08lX (%d)\n", static_cast<unsigned long>(r.amsiHr), static_cast<int>(ClassifyAmsiError(r.amsiHr)));
        else EXPECT(r.files == 2 && r.threats.empty() && r.scanErrors == 0);
    } else {
        std::printf("[real AMSI] skipped (set LYCAN_TEST_AMSI=1 to run it)\n");
    }

    fs::remove_all(root, ec);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
