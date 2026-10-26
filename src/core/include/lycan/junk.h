#pragma once
// Files that only clutter an archive: the metadata macOS adds to zip / tar files (the "__MACOSX" folder, an AppleDouble
// "._name" file beside every file, ".DS_Store") and Windows' Thumbs.db. Decided by name only, never by content.
// Header-only and free of dependencies (the context menu DLL could use it too).
#include <cwctype>
#include <string>

namespace lycan {

// `path` is the path inside an archive ('/' or '\' as separator).
inline bool IsMacMetadataPath(const std::wstring& path) {
    auto lower = [](std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; };
    size_t start = 0;
    bool first = true;
    while (start <= path.size()) {
        size_t end = path.find_first_of(L"/\\", start);
        if (end == std::wstring::npos) end = path.size();
        const std::wstring part = path.substr(start, end - start);
        if (!part.empty()) {
            if (first && lower(part) == L"__macosx") return true;  // the folder at the top, and everything below it
            if (part.size() > 2 && part[0] == L'.' && part[1] == L'_') return true;  // ._name (a file or a folder)
            if (end == path.size() && lower(part) == L".ds_store") return true;
            first = false;
        }
        start = end + 1;
    }
    return false;
}

// Windows' thumbnail cache; only the name of a file counts (case-insensitive).
inline bool IsThumbsDbName(const std::wstring& name) {
    if (name.size() != 9) return false;
    const wchar_t* t = L"thumbs.db";
    for (size_t i = 0; i < 9; ++i) if (std::towlower(name[i]) != t[i]) return false;
    return true;
}

}  // namespace lycan
