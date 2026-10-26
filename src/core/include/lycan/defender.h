#pragma once
// What Windows Defender says about a detection that it made through AMSI. AMSI itself only answers "detected" (a number, no
// name); Defender writes the name of the threat to its operational event log (event 1116) a moment later. That log is
// readable without administrator rights. Other antivirus products write nothing there, and then no name is known.
#include <cstdint>
#include <string>
#include <vector>

namespace lycan {

struct DefenderDetection {
    uint64_t time = 0;           // FILETIME (100 ns since 1601, UTC) of the event
    std::wstring name;           // e.g. HackTool:Win32/Patcher!AMTB
    std::wstring severity;       // as the system says it (the language of Windows), e.g. "high"
    std::wstring category;       // e.g. "tool"
};

// The AMSI detections that Defender logged for this process (by its executable path) at or after `sinceFileTime`, oldest first.
// Empty when the log cannot be read or Defender is not the antivirus product.
std::vector<DefenderDetection> DefenderDetectionsSince(uint64_t sinceFileTime);

}  // namespace lycan
