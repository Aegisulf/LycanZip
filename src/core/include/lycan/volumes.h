#pragma once
// Which volume of a split archive a file is, judged by its name only (nothing is opened):
//   name.part1.rar, name.part2.rar ... (also part01 / part001)   RAR 5 and new style RAR
//   name.rar, name.r00, name.r01 ...                              old style RAR
//   name.zip, name.z01, name.z02 ...                              split zip
//   name.7z.001, name.7z.002 ... (any name.NNN)                   plain split (7-Zip, HJSplit)
// Opening a volume that is not the first shows only part of the archive (or nothing); the first one is what to open.
// Header-only and free of dependencies.
#include <cwctype>
#include <filesystem>
#include <string>

namespace lycan {

struct VolumeInfo {
    unsigned index = 0;                // 0 = not a split archive by its name; 1 = the first volume; >= 2 = a later one
    std::filesystem::path first;       // index >= 2: where the first volume would be (same folder), whether it exists or not
    bool IsLater() const { return index >= 2; }
};

namespace detail {
inline std::wstring LowerName(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}
inline bool AllDigits(const std::wstring& s) {
    if (s.empty()) return false;
    for (wchar_t c : s)
        if (c < L'0' || c > L'9') return false;
    return true;
}
}  // namespace detail

inline VolumeInfo DetectVolume(const std::filesystem::path& file) {
    using namespace detail;
    VolumeInfo r;
    const std::wstring name = file.filename().wstring();
    const std::wstring low = LowerName(name);
    const std::filesystem::path dir = file.parent_path();
    auto sibling = [&](const std::wstring& n) { return dir.empty() ? std::filesystem::path(n) : dir / n; };
    const size_t dot = low.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= low.size()) return r;
    const std::wstring ext = low.substr(dot + 1);  // without the dot

    // name.partN.rar
    if (ext == L"rar") {
        const std::wstring stem = low.substr(0, dot);
        const size_t p = stem.rfind(L".part");
        if (p != std::wstring::npos && AllDigits(stem.substr(p + 5))) {
            const std::wstring digits = stem.substr(p + 5);
            const unsigned n = static_cast<unsigned>(std::stoul(digits));
            r.index = n == 0 ? 1 : n;
            if (r.index >= 2) {
                std::wstring one(digits.size(), L'0');
                one.back() = L'1';
                r.first = sibling(name.substr(0, p + 5) + one + name.substr(dot));
            }
        } else {
            r.index = 1;  // name.rar: the first volume of an old style set (or a single archive)
        }
        return r;
    }
    // name.r00, name.r01 ... (old style RAR: .rar is volume 1, .r00 volume 2)
    if (ext.size() >= 3 && ext[0] == L'r' && AllDigits(ext.substr(1))) {
        r.index = static_cast<unsigned>(std::stoul(ext.substr(1))) + 2;
        r.first = sibling(name.substr(0, dot) + name.substr(dot, 1) + L"rar");
        return r;
    }
    // name.z01, name.z02 ... (split zip: the pieces come first on disk, but the archive to open is name.zip)
    if (ext.size() >= 3 && ext[0] == L'z' && AllDigits(ext.substr(1))) {
        r.index = static_cast<unsigned>(std::stoul(ext.substr(1))) + 1;
        r.first = sibling(name.substr(0, dot) + name.substr(dot, 1) + L"zip");
        return r;
    }
    // name.NNN
    if (ext.size() >= 3 && AllDigits(ext)) {
        const unsigned n = static_cast<unsigned>(std::stoul(ext));
        r.index = n == 0 ? 1 : n;
        if (r.index >= 2) {
            std::wstring one(ext.size(), L'0');
            one.back() = L'1';
            r.first = sibling(name.substr(0, dot + 1) + one);
        }
        return r;
    }
    return r;
}

}  // namespace lycan
