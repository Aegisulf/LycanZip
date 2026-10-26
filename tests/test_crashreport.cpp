// Crash records: writing and parsing of the snapshot / report, the pending list, acknowledging, cleaning up temporary files, the number kept.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "lycan/crashreport.h"

namespace fs = std::filesystem;
using namespace lycan;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static void Touch(const fs::path& p, const char* text = "x") {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

static fs::path MakeReport(const fs::path& dir, const wchar_t* stem, const std::vector<fs::path>& archives, const std::vector<CrashTask>& tasks) {
    fs::create_directories(dir);
    CrashHeader h;
    h.time = L"2026-10-07 21:03:11";
    h.version = L"0.1.0";
    h.code = 0xC0000005;
    h.address = L"7z.dll+0x1A2B3C";
    std::wstring dumpName = std::wstring(stem) + L".dmp";
    h.dumpName = dumpName.c_str();
    const auto snap = BuildCrashSnapshot(archives, tasks);
    fs::path file = dir / (std::wstring(stem) + L".txt");
    EXPECT(WriteCrashReport(file, h, snap.data(), snap.size()));
    Touch(dir / dumpName, "dump");
    return file;
}

int main() {
    fs::path root = fs::temp_directory_path() / (L"lycan_crash_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::path dir = root / L"crashes";

    EXPECT(DefaultCrashDir().filename() == L"crashes");

    // ---- Writing and parsing (with Chinese paths)
    std::vector<fs::path> archives = {root / L"資料.zip", root / L"b.7z"};
    std::vector<CrashTask> tasks = {{L"Extract", L"Running", L"資料.zip", root / L"out"}, {L"Compress", L"Queued", L"新.zip\t含Tab", root / L"新.zip"}};
    fs::path f1 = MakeReport(dir, L"crash-20261007-210311-100", archives, tasks);
    CrashReport r;
    EXPECT(ParseCrashReport(f1, r));
    EXPECT(r.time == L"2026-10-07 21:03:11" && r.version == L"0.1.0" && r.exception == L"0xC0000005" && r.address == L"7z.dll+0x1A2B3C");
    EXPECT(r.dump == dir / L"crash-20261007-210311-100.dmp");
    EXPECT(r.archives.size() == 2 && r.archives[0] == archives[0] && r.archives[1] == archives[1]);
    EXPECT(r.tasks.size() == 2 && r.tasks[0].kind == L"Extract" && r.tasks[0].target == root / L"out");
    EXPECT(r.tasks[1].title == L"新.zip 含Tab");  // the Tab became a space, so the columns are not disturbed
    EXPECT(!r.seen);
    EXPECT(!ParseCrashReport(root / L"不存在.txt", r));
    {  // a report with a UTF-8 BOM (saved by Notepad / PowerShell) must be parsed too
        std::ifstream src(f1, std::ios::binary);
        std::string body((std::istreambuf_iterator<char>(src)), {});
        Touch(root / L"bom.txt", ("\xEF\xBB\xBF" + body).c_str());
        CrashReport rb;
        EXPECT(ParseCrashReport(root / L"bom.txt", rb) && rb.archives.size() == 2 && rb.version == L"0.1.0");
    }
    Touch(dir / L"crash-bad.txt", "not a report");
    EXPECT(!ParseCrashReport(dir / L"crash-bad.txt", r));

    // ---- Pending list: newest first; broken files ignored; not listed again after acknowledging
    MakeReport(dir, L"crash-20261008-090000-200", {root / L"c.zip"}, {});
    auto pending = PendingCrashReports(dir);
    EXPECT(pending.size() == 2);
    EXPECT(pending.size() == 2 && pending[0].archives.size() == 1 && pending[0].archives[0] == root / L"c.zip");  // 20261008 is newer than 20261007
    EXPECT(MarkCrashReportSeen(pending[0]));
    EXPECT(pending[0].seen && pending[0].file.filename() == L"crash-20261008-090000-200.seen.txt" && fs::exists(pending[0].file));
    EXPECT(!fs::exists(dir / L"crash-20261008-090000-200.txt"));
    pending = PendingCrashReports(dir);
    EXPECT(pending.size() == 1 && pending[0].archives.size() == 2);
    EXPECT(PendingCrashReports(root / L"沒有這個資料夾").empty());

    // ---- Temporary file cleanup: only the <output>.lycantmp of compression tasks is deleted
    Touch(root / L"新.zip.lycantmp");      // to be deleted
    Touch(root / L"新.zip", "real");        // the output file itself: must not be touched
    Touch(root / L"out" / L"user.txt");     // a file of the user in the extraction target: must not be touched
    Touch(root / L"資料.zip.lycantmp");    // no matching Compress task (only Extract): must not be touched
    EXPECT(CleanupCrashLeftovers(pending[0]) == 1);
    EXPECT(!fs::exists(root / L"新.zip.lycantmp"));
    EXPECT(fs::exists(root / L"新.zip") && fs::exists(root / L"out" / L"user.txt") && fs::exists(root / L"資料.zip.lycantmp"));
    EXPECT(CleanupCrashLeftovers(pending[0]) == 0);  // calling again, nothing left to delete

    // ---- Keep the newest N sets
    for (int i = 0; i < 6; ++i) {
        wchar_t stem[64];
        swprintf_s(stem, L"crash-2026101%d-000000-1", i);
        MakeReport(dir, stem, {}, {});
    }
    PruneCrashDir(dir, 3);
    int stems = 0;
    for (auto const& e : fs::directory_iterator(dir))
        if (e.path().extension() == L".dmp") ++stems;
    EXPECT(stems == 3);
    EXPECT(fs::exists(dir / L"crash-20261015-000000-1.dmp") && !fs::exists(dir / L"crash-20261010-000000-1.dmp"));
    EXPECT(!fs::exists(dir / L"crash-20261010-000000-1.txt"));

    fs::remove_all(root);
    if (failures == 0) std::puts("test_crashreport: all passed");
    return failures ? 1 : 0;
}
