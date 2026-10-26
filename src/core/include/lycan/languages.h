#pragma once
// The languages of the program: the one list. To add a language: put a row here (and add the translation file app/Assets/Lang/<code>.tsv,
// see the design notes "Adding a language"). The settings page and the welcome window show these (after "follow the system"), the context menu
// DLL has one column of texts for each row (tools/i18n.py writes shellext/shell_texts.inc in this order), and the system language is
// mapped to a row by LanguageForTag. Header only: the context menu DLL does not link the Core.
#include <cwctype>
#include <iterator>
#include <string>

namespace lycan {

struct LanguageInfo {
    const wchar_t* code;    // the name of the file in Assets\Lang, and the value of the language setting
    const wchar_t* nameId;  // the id of the name in its own language (the same text in every language file)
};

// en-US is the master language (its texts are what every other language falls back to); the order is the order of the settings page.
// tools/i18n.py reads the rows with the pattern {L"<code>", L"lang.<id>"}.
inline constexpr LanguageInfo kLanguages[] = {
    {L"zh-TW", L"lang.zh_tw"},
    {L"en-US", L"lang.en_us"},
    {L"zh-CN", L"lang.zh_cn"},
};
inline constexpr int kLanguageCount = static_cast<int>(std::size(kLanguages));

inline bool SameLanguageText(const std::wstring& a, const wchar_t* b) {
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    return i == a.size() && b[i] == 0;
}

// The index in kLanguages of a language code (not case sensitive); -1 when it is not one of them.
inline int LanguageIndex(const std::wstring& code) {
    for (int i = 0; i < kLanguageCount; ++i)
        if (SameLanguageText(code, kLanguages[i].code)) return i;
    return -1;
}

// Maps a language tag of the system or the user (zh-Hant-TW, zh-CN, en-GB, ja-JP ...) to a language of the list: Chinese by script or region
// (Simplified for hans / CN / SG, Traditional otherwise), any other language by its first part (en-GB gives en-US); a language that is not in
// the list gives en-US.
inline std::wstring LanguageForTag(const std::wstring& tag) {
    std::wstring t = tag;
    for (auto& c : t) c = static_cast<wchar_t>(std::towlower(c));
    auto primary = [](const std::wstring& s) { return s.substr(0, s.find(L'-')); };
    const std::wstring p = primary(t);
    if (p == L"zh") {
        if (t == L"zh" || t.find(L"hans") != std::wstring::npos || t.find(L"-cn") != std::wstring::npos || t.find(L"-sg") != std::wstring::npos) return L"zh-CN";
        return L"zh-TW";  // zh-TW, zh-HK, zh-MO, zh-Hant ...
    }
    for (int i = 0; i < kLanguageCount; ++i) {
        std::wstring code = kLanguages[i].code;
        for (auto& c : code) c = static_cast<wchar_t>(std::towlower(c));
        if (primary(code) == p) return kLanguages[i].code;
    }
    return L"en-US";
}

}  // namespace lycan
