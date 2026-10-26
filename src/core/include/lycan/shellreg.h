#pragma once
// The classic context menu, registered under HKCU (no administrator rights needed). Windows 10 shows it directly;
// Windows 11 shows it under "Show more options" (the new-style menu is the sparse package of shellext/).
//
// The menu itself is not described in the registry: every verb points (ExplorerCommandHandler) to a COM class in
// LycanZipShell.dll, which decides what to show, with names that follow the selection and the user's settings. Changing a
// setting therefore needs no re-registration; the registry only says "the menu exists".
//
//   *\shell\LycanZipFiles                          files        -> FilesCommand   (hides itself when everything is an archive)
//   Directory\shell\LycanZipFiles                  folders      -> FilesCommand
//   SystemFileAssociations\<ext>\shell\LycanZipArchive   archives -> ArchiveCommand
//   SystemFileAssociations\<ext>\shell\LycanZipVerify   checksum files (.sha256 .md5 .sfv ...) -> VerifyCommand
//   Directory | Drive | Folder \shellex\DragDropHandlers\LycanZip   right-button drag -> DragDropCommand (an IContextMenu)
//   CLSID\{...}\InprocServer32                     the DLL
#include <filesystem>
#include <string>

namespace lycan {

struct ShellRegOptions {
    std::filesystem::path exe;                       // full path of LycanZip.exe
    std::filesystem::path dll;                       // the menu DLL; empty = ShellDllPath(the folder of the exe)
    std::wstring classesKey = L"Software\\Classes";  // the Classes key under HKCU (tests use a temporary location)
};

// Writes the registration. On failure returns false and error (may be nullptr) says why. Calling it again is safe
// (it replaces everything with the current paths); the entries of earlier versions are removed.
bool RegisterShellMenu(const ShellRegOptions& opt, std::wstring* error = nullptr);

// Removes everything RegisterShellMenu (and earlier versions) created. Already absent counts as success.
bool UnregisterShellMenu(const ShellRegOptions& opt, std::wstring* error = nullptr);

// Is the menu registered? (`*\shell\LycanZipFiles` exists)
bool IsShellMenuRegistered(const ShellRegOptions& opt);

// The DLL the classic registration points to (the InprocServer32 of the files command); empty when it is not registered.
// After a portable folder was moved this is the old place.
std::filesystem::path RegisteredShellDll(const ShellRegOptions& opt);

// The menu DLL has the version in its file name (LycanZipShell-0.9.1.dll). Explorer (and every program with a file dialog) keeps a loaded
// shell extension locked, so a DLL of a fixed name could not be replaced by an update of a zip or portable install. With the version in
// the name the new DLL is a new file; the program registers it (the old one is not loaded any more after Explorer restarted) and removes
// the old ones when it can.
std::wstring ShellDllFileName();                                          // LycanZipShell-<version>.dll of this build
std::filesystem::path ShellDllPath(const std::filesystem::path& dir);     // dir / ShellDllFileName()
// Deletes the menu DLLs of other versions in dir (LycanZipShell.dll of the first versions and LycanZipShell-*.dll) except `keep`. A DLL that is
// loaded cannot be deleted: it stays, and the next start tries again. Returns how many were deleted.
int RemoveOtherShellDlls(const std::filesystem::path& dir, const std::filesystem::path& keep);

// Where the menu DLL (and the sparse package LycanZipShell.msix of the Windows 11 menu) lives. Normally that is the folder of the exe. An
// installed copy (the installer puts the program in <root>\current and an update replaces that whole folder, which cannot be done while
// Explorer has the DLL loaded) keeps them outside: %LOCALAPPDATA%\LycanZip\shell. dataRoot is only for tests (empty = the real place).
bool IsInstalledLayout(const std::filesystem::path& exeDir);               // exeDir is called "current" and <exeDir>\..\Update.exe exists
std::filesystem::path ShellHome(const std::filesystem::path& exeDir, const std::filesystem::path& dataRoot = {});
// ShellHome, after the files of this version were copied there when it is not the folder of the exe (nothing is copied when they are up to
// date). Returns the folder; a copy that fails leaves the files missing, which the registration reports.
std::filesystem::path EnsureShellHome(const std::filesystem::path& exeDir, const std::filesystem::path& dataRoot = {});

// The CLSIDs of the classic registration (shellext.cpp: ClassicFilesCommand / ClassicArchiveCommand / DragDropCommand)
inline constexpr const wchar_t* kClassicFilesClsid = L"{5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A20}";
inline constexpr const wchar_t* kClassicArchiveClsid = L"{5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A21}";
inline constexpr const wchar_t* kDragDropClsid = L"{5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A22}";
inline constexpr const wchar_t* kClassicVerifyClsid = L"{5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A23}";

}  // namespace lycan
