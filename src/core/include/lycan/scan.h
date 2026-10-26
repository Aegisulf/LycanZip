#pragma once
// "Scan for malware": every file of an archive is unpacked into memory, one at a time, and handed to the antimalware
// product that is installed (through AMSI, the Antimalware Scan Interface of Windows). LycanZip has no scanning engine of
// its own, so the answer is only as good as that product: it needs real-time protection to be on and must support AMSI.
// A clean result is not a guarantee. A file is scanned whole (cutting it up would cut through signatures) and Defender stops
// scanning at 128 MB without saying so (measured: 128 MB is scanned, 129 MB comes back "not detected" at once), so bigger
// files are skipped and reported as skipped, never as clean.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/defender.h"

namespace lycan {

inline constexpr uint64_t kAmsiMaxFileBytes = 128ull << 20;
inline constexpr size_t kMaxScanNotes = 50;

enum class ScanVerdict { Clean, Detected, Blocked, Failed };  // Blocked: an administrator's policy blocks it (AMSI 0x4000..0x4FFF)

// Test hook: what looks at one file. Null = AMSI. (The environment variable LYCAN_TEST_SCAN_FAKE=1 swaps in a scanner that
// flags content containing "LYCAN-TEST-THREAT", for tests that must not depend on the antivirus of the machine.)
using ScanFn = std::function<ScanVerdict(const std::wstring& contentName, const void* data, size_t size)>;

struct ScanOptions {
    PasswordFn password;                         // for encrypted items; without it they are skipped
    ProgressFn progress;                         // false = cancel
    uint64_t maxFileBytes = kAmsiMaxFileBytes;   // bigger files are skipped
    ScanFn scanner;
    // Test hook: what the antivirus logged about this scan (null = Windows Defender's event log, asked only when the real AMSI
    // scanner was used). `lookupWaitMs`: how long to wait for the log to catch up with the detections.
    std::function<std::vector<DefenderDetection>(uint64_t sinceFileTime)> detections;
    unsigned lookupWaitMs = 3000;
};

struct ScanThreat {
    std::wstring path;      // inside the archive, '/' as separator
    bool blocked = false;   // blocked by policy rather than found by a signature
    // What the antivirus product called it, when that is known for this file (see ScanReport::threatNames)
    std::wstring name, severity, category;
};

// An item that was not scanned (the first few of them are listed)
struct ScanNote {
    enum class Kind { TooLarge, Encrypted, Unreadable, ScanFailed } kind = Kind::TooLarge;
    std::wstring path;
};

struct ScanReport {
    uint64_t files = 0, bytes = 0;               // scanned
    uint32_t skippedLarge = 0, skippedEncrypted = 0;
    uint32_t unreadable = 0;                     // could not be unpacked (damaged, wrong password, unsupported method)
    uint32_t scanErrors = 0;                     // the scanner itself failed on the file
    std::vector<ScanNote> notes;                 // at most kMaxScanNotes
    std::vector<ScanThreat> threats;
    // The different threat names that Defender logged for this scan. AMSI itself gives no name; the names come from Defender's
    // event log. Each file gets its own name only when the log has exactly one entry per detected file (or one name in all);
    // otherwise only this list is known. Empty for other antivirus products.
    std::vector<std::wstring> threatNames;
    long amsiHr = 0;                             // != 0: AMSI could not be used and nothing was scanned
    bool Found() const { return !threats.empty(); }
    uint32_t Skipped() const { return skippedLarge + skippedEncrypted + unreadable; }
};

// What an AMSI failure means (for the message to the user): 0x80070015 = real-time protection is off, 0x80070103 = the
// installed antivirus does not support AMSI, anything else = AMSI is not there or failed.
enum class AmsiProblem { None, RealTimeOff, NoProvider, Unavailable };
AmsiProblem ClassifyAmsiError(long hr);

// Reads every file once (solid archives too: one pass). Throws lycan::Error for cancel and for archive errors;
// AMSI trouble is reported in ScanReport::amsiHr.
ScanReport ScanArchive(ArchiveSource& src, const ScanOptions& options = {});

}  // namespace lycan
