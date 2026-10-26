#pragma once
// Crash records and recovery (only on this computer; nothing is ever uploaded).
// On a crash (the unhandled exception handler of the App) two files are written to DefaultCrashDir(): crash-<time>-<pid>.dmp (minidump) and a .txt of the same name
// (text report: time / version / exception code and place / which archives were open and which tasks were not finished).
// At the next start PendingCrashReports finds the reports that have not been acknowledged; the App tells the user, can reopen the archives of last time with one click, and cleans up
// the compression temporary files that are clearly ours (<output>.lycantmp); after acknowledging, the report is renamed to .seen.txt (kept for the user to report problems).
// Only a real crash makes a report: ending the process by hand or a power cut is not mistaken for a crash.
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace lycan {

struct CrashTask {
    std::wstring kind;   // Extract / Compress / Test
    std::wstring state;  // Queued / Running / Paused
    std::wstring title;  // usually the archive name
    std::filesystem::path target;  // extract: target folder; compress: output file; test: the archive
};

struct CrashReport {
    std::filesystem::path file;  // crash-*.txt (or .seen.txt)
    std::filesystem::path dump;  // .dmp (may not exist)
    std::wstring time, version, exception, address;
    std::vector<std::filesystem::path> archives;  // archives that were open at the crash
    std::vector<CrashTask> tasks;                  // tasks that were not finished at the crash
    bool seen = false;
};

// Portable mode (LycanZip.portable next to the exe): the crashes folder next to the exe; otherwise UserDataDir()\crashes (datalocation.h)
std::filesystem::path DefaultCrashDir();

// A snapshot of the current state (UTF-8, written into the report as it is later): each line is "window<Tab>path" or "task<Tab>kind<Tab>state<Tab>title<Tab>target".
// A Tab or line break in a path / title is replaced by a space.
std::string BuildCrashSnapshot(const std::vector<std::filesystem::path>& archives, const std::vector<CrashTask>& tasks);

struct CrashHeader {
    const wchar_t* time = L"";       // for example 2026-10-07 21:03:11
    const wchar_t* version = L"";
    unsigned long code = 0;          // exception code (0xC0000005 ...)
    const wchar_t* address = L"";    // for example 7z.dll+0x1A2B3C
    const wchar_t* dumpName = L"";   // file name of the .dmp (empty if none)
};

// Writes the report. For the crash handler: only simple Win32 file API, throws nothing (memory may be damaged already; best effort).
bool WriteCrashReport(const std::filesystem::path& file, const CrashHeader& header, const char* snapshotUtf8, size_t snapshotLen);

bool ParseCrashReport(const std::filesystem::path& file, CrashReport& out);

// Reports not acknowledged yet (without .seen.txt), newest first
std::vector<CrashReport> PendingCrashReports(const std::filesystem::path& dir);
// Acknowledge: crash-X.txt -> crash-X.seen.txt (false on failure)
bool MarkCrashReportSeen(CrashReport& report);
// Deletes the <output>.lycantmp that compression tasks in the report left behind (only this fixed name, and only when the file exists); returns how many were deleted.
// The target of an extraction may hold files the user had before, which are not touched.
int CleanupCrashLeftovers(const CrashReport& report);
// Keeps only the newest keep sets of reports (.txt / .seen.txt and the .dmp of the same name); deletes the rest
void PruneCrashDir(const std::filesystem::path& dir, size_t keep = 5);

}  // namespace lycan
