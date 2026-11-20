#pragma once
// Safety layer for extraction paths. Pure functions that do not touch the file system, easy to unit test and fuzz.
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "lycan/message.h"

namespace lycan {

struct SafePath {
    bool rejected = false;       // true = must not be extracted (path traversal, absolute path, drive letter, ADS ...)
    bool modified = false;       // true = the file name was cleaned (reserved name, illegal characters, trailing space / dot)
    Message reason;               // why (for the log / the UI)
    std::filesystem::path relative;  // cleaned relative path (empty when rejected)
};

// Turns a raw path in the archive into a relative path that is safe to put after the target folder.
SafePath SanitizeEntryPath(std::wstring_view raw);

// Characters that change the direction of the text after them (U+202E "right-to-left override" and its kind). In a file name they hide
// the real extension: "x" U+202E "txt.exe" is shown as "xexe.txt". VisibleName shows each one as a mark ("[RLO]") so that the name
// reads as it really is; it is for showing only (the name itself is not changed).
bool HasBidiControls(std::wstring_view name);
std::wstring VisibleName(std::wstring_view name);

// A name that pretends to be another type: hidden direction marks, or a program whose name ends in a document / picture / archive
// extension before the real one ("invoice.pdf.exe", "photo.jpg      .scr").
bool LooksDisguised(std::wstring_view name);

// What is worth a warning when an archive is opened: names that pretend to be another type, and paths the extraction refuses
// (path traversal, absolute paths ...). `example` is the first such name (VisibleName), for the message.
struct NameWarnings {
    uint32_t disguised = 0, unsafePaths = 0;
    std::wstring example;
};
NameWarnings ScanEntryNames(const std::vector<std::wstring>& paths);

// Turns it into the \\?\ long path form (returned unchanged if it is that already).
std::filesystem::path ToLongPath(const std::filesystem::path& p);

}  // namespace lycan
