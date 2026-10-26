#pragma once
// Files that are compressed already (photos, music, video, archives, Office documents ...): packing them again takes time
// and gains nothing, so "smart store" (CompressOptions::storeCompressed) puts them into the archive as they are.
// This header is the extension test only (header-only, no dependencies); probe.h looks at the content of the files this list
// does not know.
#include <cwctype>
#include <iterator>
#include <string>

namespace lycan {

// Files smaller than this are not worth a separate pass (and cost nothing to compress)
inline constexpr unsigned long long kStoreMinBytes = 64 * 1024;

inline constexpr const wchar_t* kCompressedExts[] = {
    // pictures
    L".jpg", L".jpeg", L".png", L".gif", L".webp", L".avif", L".heic", L".heif", L".jxl",
    // audio
    L".mp3", L".aac", L".m4a", L".ogg", L".opus", L".flac", L".wma",
    // video
    L".mp4", L".m4v", L".mkv", L".mov", L".webm", L".wmv", L".flv", L".3gp",
    // archives and packages
    L".zip", L".7z", L".rar", L".gz", L".xz", L".bz2", L".zst", L".lz4", L".lzma", L".cab", L".tgz", L".txz", L".tbz2",
    L".jar", L".apk", L".aab", L".whl", L".nupkg", L".vsix", L".msix", L".appx",
    // zip-based documents
    L".docx", L".xlsx", L".pptx", L".odt", L".ods", L".odp", L".epub",
    // fonts
    L".woff2",
};

// `name` is a file name or path ('/' or '\' as separator); the extension is compared without regard to case.
inline bool IsAlreadyCompressedName(const std::wstring& name) {
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    const size_t slash = name.find_last_of(L"/\\");
    if (slash != std::wstring::npos && slash > dot) return false;
    std::wstring ext = name.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    for (const wchar_t* e : kCompressedExts)
        if (ext == e) return true;
    return false;
}

}  // namespace lycan
