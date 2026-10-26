#pragma once
// File associations (HKCU, no administrator needed).
//
// Windows 10 / 11 does not let a program make itself the "default" of an extension (UserChoice is protected by a hash; getting around it is what malware does).
// The right way is to register as "an application that can handle these files" (ProgID + Applications + Capabilities + RegisteredApplications),
// and the user chooses it in "Settings > Default apps". That registration is what RegisterFileAssociations does; OpenDefaultAppsSettings
// opens that settings page and points to LycanZip.
#include <filesystem>
#include <string>

namespace lycan {

struct FileAssocOptions {
    std::filesystem::path exe;                         // full path of LycanZip.exe
    std::wstring classesKey = L"Software\\Classes";    // Classes under HKCU (a test can use a temporary place)
    std::wstring softwareKey = L"Software";            // where RegisteredApplications / Capabilities go under HKCU
    bool notifyShell = true;                           // tell Explorer when done that the associations changed (off in tests)
};

// Registers as an application that can open archives; can be called again (overwrites with the newest exe path).
bool RegisterFileAssociations(const FileAssocOptions& opt, std::wstring* error = nullptr);
// After the language of the program changed: writes the names of the types again (only the names: where the registration points to is left alone).
// Returns true when something was changed; false also when the associations are not registered.
bool UpdateAssociationNames(const FileAssocOptions& opt);
// Removes everything RegisterFileAssociations wrote (absent counts as success).
bool UnregisterFileAssociations(const FileAssocOptions& opt, std::wstring* error = nullptr);
bool IsFileAssociationsRegistered(const FileAssocOptions& opt);
// The LycanZip.exe the registration points to (the open command of the generic program id); empty when it is not registered.
// After a portable folder was moved this is the old place.
std::filesystem::path RegisteredAssocExe(const FileAssocOptions& opt);

// Read only: the display name of the program that opens ext (with the dot, for example L".zip") by default; an empty string if there is no association.
std::wstring CurrentDefaultAppName(const wchar_t* ext);
// Read only: whether the program that opens ext by default is LycanZip.exe (compared by the name of the executable).
bool IsLycanZipDefaultFor(const wchar_t* ext);

// Opens the "Default apps" page of the system and points to LycanZip (it must be registered).
bool OpenDefaultAppsSettings();

// The program id and the name Explorer shows in its "Type" column for an extension (with the dot): "LycanZip.zip" and "LycanZip ZIP archive" (the
// language of the program). One program id per extension, so that the kind of archive can be told apart; the icon is that of the family (fileicons.h).
std::wstring AssociationProgId(const wchar_t* ext);
std::wstring AssociationTypeName(const wchar_t* ext);

// The extensions (with the dot) that the registration covers.
const wchar_t* const* AssociatedExtensions(size_t* count);

}  // namespace lycan
