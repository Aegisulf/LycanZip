#pragma once
// Files that are plain text: "edit" opens them in Notepad (or the editor of the settings) instead of whatever program the
// extension is associated with (a .json or .xml is often associated with a browser). By the extension only.
#include <algorithm>
#include <cwctype>
#include <string>

namespace lycan {

inline bool IsTextEditName(const std::wstring& name) {
    static const wchar_t* const kExts[] = {L"txt", L"md", L"markdown", L"log", L"ini", L"cfg", L"conf", L"config", L"json", L"xml", L"yaml", L"yml",
                                           L"toml", L"csv", L"tsv", L"html", L"htm", L"css", L"js", L"ts", L"jsx", L"tsx", L"bat", L"cmd", L"ps1",
                                           L"sh", L"c", L"h", L"cpp", L"hpp", L"cc", L"cs", L"py", L"java", L"kt", L"rs", L"go", L"php", L"rb", L"sql",
                                           L"properties", L"gitignore", L"env", L"srt", L"vtt", L"tex", L"rst", L"svg", L"reg", L"inf"};
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return false;
    std::wstring ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    for (const wchar_t* e : kExts)
        if (ext == e) return true;
    return false;
}

}  // namespace lycan
