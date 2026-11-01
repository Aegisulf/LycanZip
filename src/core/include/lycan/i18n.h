#pragma once
// Localization with string ids. The code and the XAML only contain ids such as "main.extract_here"; Tr(L"main.extract_here") gives the text
// in the current language, Tf(L"main.items", n) also fills in {0} {1}... The texts are in Assets\Lang\<language>.tsv: en-US.tsv has all
// of them (the master, and what is shown when a language has no text for an id), the other files are translations. An id that no file
// has is shown as it is (so a missing text is easy to see). The language is loaded once when the program starts (Load); to change it
// the program is restarted. tools/i18n.py checks the files against the code.
#include <filesystem>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace lycan {

namespace i18n {

constexpr const wchar_t* kPseudoLanguage = L"pseudo";  // test language: accented English, 40 % longer, in brackets (LYCAN_LANG=pseudo)
constexpr const wchar_t* kSourceLanguage = L"en-US";  // the language of the master file (and the one used when nothing else fits)

// File format (UTF-8, a BOM is allowed): one line "id<Tab>text[<Tab>more]"; a line that starts with # is a comment; \n \t \\ are escapes;
// an empty text = not translated yet. Only the first two columns are read. Returns the number of texts read.
size_t ParseTable(const std::string& utf8, std::unordered_map<std::wstring, std::wstring>& out);

// Maps the language tag of the system or the user (zh-Hant-TW, zh-CN, en-GB, ja-JP ...) to a language of lycan::kLanguages (languages.h).
// A language that is not in the list gives en-US.
std::wstring MapLanguageTag(const std::wstring& tag);

std::wstring SystemLanguage();  // the user's preferred display language, run through MapLanguageTag

std::filesystem::path DefaultLangDir();  // Assets\Lang next to the exe

// setting: the language value of settings.ini ("auto" or a language code). Returns the code that will really be used: a language counts
// when it has a file in langDir, otherwise it is en-US.
std::wstring Resolve(const std::wstring& setting, const std::filesystem::path& langDir);

// Loads a language (what was loaded before is dropped): the master en-US.tsv first, then the file of the language over it. A language
// without a file stays en-US. Returns the number of texts of the language file (of the master when the language is en-US).
size_t Load(const std::filesystem::path& langDir, const std::wstring& code);

const std::wstring& Current();  // the code of the language that is loaded
bool IsSourceLanguage();        // is it en-US
// The language tag for the XAML text of the interface (FrameworkElement.Language), when the loaded language is not the one of Windows (which
// XAML uses by itself); empty otherwise, and for the pseudo language. The font fallback picks its fonts by it, so a Japanese text on a
// Chinese Windows gets the Japanese forms of the kanji.
std::wstring UiLanguageTag();

// The English text behind a text of the current language (the text of some id, as it is shown), so that a search also finds the English
// words in a translated interface. Empty when the language is English or no id has this text. Two ids with the same text: one of them.
std::wstring EnglishOf(std::wstring_view shown);

}  // namespace i18n

// The text of an id in the current language; an id without text is returned as it is. The pointer is valid until the next i18n::Load.
const wchar_t* Tr(const wchar_t* id);

namespace i18n_detail {
std::wstring FormatImpl(const wchar_t* id, const std::vector<std::wstring>& args);
std::wstring FormatEnglish(const std::wstring& id, const std::vector<std::wstring>& args);  // the Core table (message.h)
inline std::wstring ToW(const std::wstring& s) { return s; }
inline std::wstring ToW(const wchar_t* s) { return s ? s : L""; }
inline std::wstring ToW(std::wstring_view s) { return std::wstring(s); }  // also what converts to a wstring_view, such as winrt::hstring
template <class T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
std::wstring ToW(T v) { return std::to_wstring(v); }
}  // namespace i18n_detail

// The text of an id with {0} {1}... filled in (a translation may reorder them). An argument is a string or a number.
// {N:one|many} chooses by the number, for plural forms such as "{0} {0:file|files}" (the first when the argument is 1).
template <class... Args>
std::wstring Tf(const wchar_t* id, Args&&... args) {
    return i18n_detail::FormatImpl(id, {i18n_detail::ToW(args)...});
}

}  // namespace lycan
