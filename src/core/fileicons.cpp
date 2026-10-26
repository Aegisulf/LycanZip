#include "lycan/fileicons.h"

#include <cwctype>
#include <string>

namespace lycan {

IconFamily IconFamilyForExtension(std::wstring_view ext) {
    if (!ext.empty() && ext.front() == L'.') ext.remove_prefix(1);
    std::wstring e(ext);
    for (auto& c : e) c = static_cast<wchar_t>(std::towlower(c));
    auto in = [&](std::initializer_list<const wchar_t*> list) {
        for (auto s : list) if (e == s) return true;
        return false;
    };
    if (in({L"cbz", L"cbr", L"cb7", L"cbt"})) return IconFamily::Comic;   // comic book archives: one icon of their own
    if (in({L"zip", L"zipx", L"jar"})) return IconFamily::Zip;
    if (in({L"7z"})) return IconFamily::SevenZip;
    if (in({L"rar"})) return IconFamily::Rar;
    if (in({L"tar", L"gz", L"tgz", L"xz", L"txz", L"bz2", L"tbz2", L"tbz", L"zst"})) return IconFamily::Tar;
    if (in({L"iso", L"wim"})) return IconFamily::Iso;
    return IconFamily::Other;
}

IconFamily IconFamilyForPath(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\/");
    if (slash != std::wstring_view::npos) path.remove_prefix(slash + 1);
    const auto dot = path.rfind(L'.');
    if (dot == std::wstring_view::npos) return IconFamily::Other;
    return IconFamilyForExtension(path.substr(dot + 1));
}

const wchar_t* IconFamilyName(IconFamily f) {
    switch (f) {
        case IconFamily::Zip: return L"zip";
        case IconFamily::SevenZip: return L"7z";
        case IconFamily::Rar: return L"rar";
        case IconFamily::Tar: return L"tar";
        case IconFamily::Iso: return L"iso";
        case IconFamily::Comic: return L"comic";
        default: return L"arc";
    }
}

int IconResourceId(IconFamily f) { return 101 + static_cast<int>(f); }

}  // namespace lycan
