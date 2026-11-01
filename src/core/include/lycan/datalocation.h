#pragma once
// Where the settings live: next to LycanZip.exe ("portable": the file LycanZip.portable is there) or in the user's folder
// (%LOCALAPPDATA%\LycanZip). settings.h (DefaultSettingsPath), the crash reports, the log and the Explorer menu all follow the same
// flag. This module reads the state and switches it: the settings and the presets are COPIED to the new place (the old ones stay),
// then the flag file is made or removed. Crash reports and logs are not copied: they are short-lived and start again.
#include <filesystem>

namespace lycan {

struct DataLocation {
    bool portable = false;          // the flag file is next to the exe
    bool packaged = false;          // an MSIX package: the program folder is read-only and the data belong to the package; no switching
    std::filesystem::path exeDir;   // the folder of LycanZip.exe
    std::filesystem::path userDir;  // UserDataDir()
    std::filesystem::path dataDir;  // where the settings are now (exeDir or userDir)
};

DataLocation GetDataLocation();

// The data folder of a copy that is not portable: %LOCALAPPDATA%\LycanZip, or for a package (the Microsoft Store version) its own
// LocalState folder (%LOCALAPPDATA%\Packages\<family>\LocalState). Not %LOCALAPPDATA%\LycanZip for a package: Windows redirects the writes
// of a package there to a private copy, so the package would read the files of an installed copy until it saved once, and File Explorer
// ("Open the settings folder") would show files that the package does not use. The first time a package asks, the settings and the presets
// it saw in %LOCALAPPDATA%\LycanZip are copied to LocalState (once: a file "imported" is left there when something was copied; with nothing
// to copy, it looks again at the next start until the package has a settings.ini of its own). The settings, the presets, the logs,
// the crash reports and the Explorer menu (shellext.cpp) all use this folder.
std::filesystem::path UserDataDir();
// The same rule for given folders (tests): packageDir empty = not a package
std::filesystem::path UserDataDirFor(const std::filesystem::path& localAppData, const std::filesystem::path& packageDir);
// The one-time copy into the folder of a package. Returns true when it copied something.
bool ImportIntoPackageData(const std::filesystem::path& from, const std::filesystem::path& to);

// The Microsoft Store version (its package family). When it is installed for this user it provides the right-click menu and the file
// types; the other copies (installer, zip, portable) then switch their own off, so that Explorer does not show LycanZip twice.
inline constexpr const wchar_t* kStorePackageFamily = L"Aegisulf.LycanZip_9f2w5eb76dr46";
bool IsStorePackageInstalled();   // tests: LYCAN_TEST_STORE_INSTALLED=1 / 0

// Can a file be made in this folder (it is made if it is not there)? Used before switching to a folder.
bool DirWritable(const std::filesystem::path& dir);

enum class SwitchStatus { Ok, NotWritable, CopyFailed, FlagFailed };

// Switches to portable (toPortable) or to the user's folder. Does nothing (Ok) when it is the mode already. A file of the target
// folder that is there already is kept as <name>.bak. Nothing changes when it fails before the flag (the copies stay).
SwitchStatus SwitchDataLocation(const std::filesystem::path& exeDir, const std::filesystem::path& userDir, bool toPortable);

}  // namespace lycan
