#pragma once
// The Windows 11 context menu (the top level of the right-click menu): LycanZipShell.dll as a sparse package, registered for
// the current user. Explorer only loads the new-style menu of programs that have a package identity, and the package has to be
// signed with a certificate the PC trusts (the release flow makes and signs that package).
// The program looks for LycanZipShell.msix next to LycanZip.exe; LYCAN_WIN11_MENU_MANIFEST=<AppxManifest.xml> registers an
// unsigned loose layout instead (needs Developer Mode; for testing).
#include <filesystem>

#include <winrt/Windows.Foundation.h>

namespace lycan_app {

enum class Win11MenuState { Unsupported, NotRegistered, Registered };

bool IsWindows11();                                          // build 22000 or later
bool Win11MenuPackageAvailable(std::filesystem::path const& installDir);
Win11MenuState QueryWin11Menu();
// The folder the registered package points to (its external location; after a portable folder was moved this is the old place); empty when it is not registered.
// Also asks the package manager: call it from a background thread.
std::wstring Win11MenuLocation();                             // asks the package manager (can take a moment: call it from a background thread)

// Where the registered package points to and which version it is (the package version of the manifest is the version of the program)
struct Win11MenuRegistration {
    bool found = false;
    std::wstring location;
    unsigned major = 0, minor = 0, build = 0;
};
Win11MenuRegistration QueryWin11MenuRegistration();   // asks the package manager: call it from a background thread

// Register / remove the package; the result is empty when it worked, otherwise the reason (already translated)
winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> RegisterWin11MenuAsync(std::filesystem::path installDir);
winrt::Windows::Foundation::IAsyncOperation<winrt::hstring> UnregisterWin11MenuAsync();

}  // namespace lycan_app
