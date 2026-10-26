#pragma once
// Safety layer for extraction paths. Pure functions that do not touch the file system, easy to unit test and fuzz.
#include <filesystem>
#include <string>
#include <string_view>

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

// Turns it into the \\?\ long path form (returned unchanged if it is that already).
std::filesystem::path ToLongPath(const std::filesystem::path& p);

}  // namespace lycan
