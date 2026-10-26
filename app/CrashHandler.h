#pragma once
// Crash handling (unhandled SEH exceptions and std::terminate): writes a minidump and a text report to lycan::DefaultCrashDir() (see lycan/crashreport.h).
// Only on this computer, nothing is uploaded. The report says which archives were open and which tasks were not finished (the UI thread keeps it current with SetCrashSnapshot).
#include <string>

namespace lycan_app {

// Call once at start: makes the folder, removes reports that are too old, loads dbghelp, installs the handlers
void InstallCrashHandler();

// The UI thread updates the snapshot (UTF-8, the result of lycan::BuildCrashSnapshot). Double buffered, no lock: the crash handler can read it safely on any thread at any time.
void SetCrashSnapshot(const std::string& utf8);

}  // namespace lycan_app
