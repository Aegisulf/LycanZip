#pragma once
// The words about a scan (the card of the task and the question after it). A clean result never says "safe": the installed
// antimalware product did not find anything, nothing more.
#include <string>

#include "lycan/i18n.h"
#include "lycan/scan.h"

namespace lycan_app {

inline std::wstring ScanProblemText(long hr) {
    switch (lycan::ClassifyAmsiError(hr)) {
        case lycan::AmsiProblem::RealTimeOff:
            return lycan::Tr(L"scan.cannot_scan_windows_real_time");
        case lycan::AmsiProblem::NoProvider:
            return lycan::Tr(L"scan.cannot_scan_antivirus_product");
        default:
            return lycan::Tr(L"scan.cannot_scan_computer_cannot_use");
    }
}

// "N files scanned, nothing found; skipped ..." (the card of a finished scan)
inline std::wstring ScanSummary(lycan::ScanReport const& r, std::wstring const& size) {
    std::wstring s = lycan::Tf(L"scan.file_s_scanned", r.files, size);
    s += r.Found() ? lycan::Tf(L"scan.suspicious_file_s_found", r.threats.size()) : lycan::Tr(L"scan.nothing_found");
    if (r.skippedLarge) s += lycan::Tf(L"scan.file_s_skipped_too_big", r.skippedLarge);
    if (r.skippedEncrypted) s += lycan::Tf(L"scan.file_s_skipped_encrypted", r.skippedEncrypted);
    if (r.unreadable) s += lycan::Tf(L"scan.could_not_be_read", r.unreadable);
    if (r.scanErrors) s += lycan::Tf(L"scan.could_not_be_scanned", r.scanErrors);
    return s;
}

}  // namespace lycan_app
