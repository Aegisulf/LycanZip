#pragma once
// Files that run code when opened (executables, installers, scripts, shortcuts). Opening one from inside an archive
// asks first, and extracts the files next to it so that it finds its dependencies.
#include <algorithm>
#include <cwctype>
#include <string>

namespace lycan {

inline bool IsExecutableName(const std::wstring& name) {
    static const wchar_t* const kExts[] = {L"exe",  L"com", L"scr", L"bat", L"cmd", L"ps1", L"psm1", L"vbs", L"vbe", L"js",   L"jse",
                                           L"wsf",  L"wsh", L"msi", L"msp", L"msix", L"appx", L"lnk", L"hta", L"jar", L"reg", L"cpl",
                                           L"pif",  L"gadget", L"msc", L"inf", L"url", L"appinstaller",
                                           // less common ones that also run code or open something that does (ClickOnce, shell and
                                           // search shortcuts, compiled help, script components, Office add-ins, update packages)
                                           L"application", L"appref-ms", L"settingcontent-ms", L"library-ms", L"searchconnector-ms",
                                           L"scf", L"chm", L"vb", L"ws", L"wsc", L"sct", L"shb", L"shs", L"xll", L"xbap", L"msu", L"mst",
                                           L"diagcab", L"ps1xml", L"ps2xml", L"psc1", L"psc2", L"psd1", L"jnlp", L"mshxml", L"website"};
    const auto dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot + 1 >= name.size()) return false;
    std::wstring ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    for (const wchar_t* e : kExts)
        if (ext == e) return true;
    return false;
}

}  // namespace lycan
