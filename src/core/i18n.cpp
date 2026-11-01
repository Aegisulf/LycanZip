#include "lycan/i18n.h"
#include "lycan/languages.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace lycan {
namespace {

std::unordered_map<std::wstring, std::wstring>& Table() {
    static std::unordered_map<std::wstring, std::wstring> t;
    return t;
}
std::unordered_map<std::wstring, std::wstring>& EnglishTable() {  // the master, kept when a language is loaded over it (EnglishOf)
    static std::unordered_map<std::wstring, std::wstring> t;
    return t;
}
std::unordered_map<std::wstring, std::wstring>& ShownToEnglish() {  // made the first time EnglishOf is asked
    static std::unordered_map<std::wstring, std::wstring> t;
    return t;
}
std::wstring& CurrentLang() {
    static std::wstring c = i18n::kSourceLanguage;
    return c;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring Unescape(const std::wstring& s) {
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t c = s[i + 1];
            if (c == L'n') { r += L'\n'; ++i; continue; }
            if (c == L't') { r += L'\t'; ++i; continue; }
            if (c == L'\\') { r += L'\\'; ++i; continue; }
        }
        r += s[i];
    }
    return r;
}

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool ValidCode(const std::wstring& c) {
    if (c.empty() || c.size() > 16) return false;
    return std::all_of(c.begin(), c.end(), [](wchar_t ch) { return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-'; });
}

// The pseudo language (LYCAN_LANG=pseudo): every English text gets accents, grows by about 40 % and is put in brackets, so truncated or
// overlapping layout shows up without any translation. {0} {1:one|many} placeholders stay as they are.
std::wstring Pseudo(const std::wstring& en) {
    static const wchar_t kFrom[] = L"aeiouyncAEIOUNC";
    static const wchar_t kTo[] = L"àéîöüýñçÅÉÎÖÜÑÇ";
    std::wstring out = L"[";
    size_t letters = 0;
    for (size_t i = 0; i < en.size(); ++i) {
        const wchar_t c = en[i];
        if (c == L'{') {
            const size_t close = en.find(L'}', i);
            if (close != std::wstring::npos && i + 1 < en.size() && en[i + 1] >= L'0' && en[i + 1] <= L'9') {
                out += en.substr(i, close - i + 1);
                i = close;
                continue;
            }
        }
        const wchar_t* at = c ? std::wcschr(kFrom, c) : nullptr;
        out += at ? kTo[at - kFrom] : c;
        if (c != L'\n') ++letters;
    }
    if (letters >= 4) out += L" " + std::wstring((letters * 2 + 4) / 5, L'·');
    return out + L"]";
}

}  // namespace

namespace i18n {

size_t ParseTable(const std::string& utf8, std::unordered_map<std::wstring, std::wstring>& out) {
    size_t count = 0;
    std::string data = utf8;
    if (data.size() >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB && static_cast<unsigned char>(data[2]) == 0xBF)
        data.erase(0, 3);
    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        size_t tab2 = line.find('\t', tab + 1);  // a third column (the note, the hash of the English text) is not read
        std::wstring key = Unescape(FromUtf8(line.substr(0, tab)));
        std::wstring val = Unescape(FromUtf8(line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1)));
        if (val.empty()) continue;  // not translated
        out[key] = val;
        ++count;
    }
    return count;
}

std::wstring MapLanguageTag(const std::wstring& tag) {
    return LanguageForTag(tag);
}

std::wstring SystemLanguage() {
    ULONG num = 0, len = 0;
    if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, nullptr, &len) && len > 0) {
        std::wstring buf(len, L'\0');
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &num, buf.data(), &len) && !buf.empty() && buf[0]) return MapLanguageTag(buf.c_str());
    }
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {};
    if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) return MapLanguageTag(name);
    return L"en-US";
}

fs::path DefaultLangDir() {
    wchar_t exe[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    return fs::path(exe).parent_path() / L"Assets" / L"Lang";
}

std::wstring Resolve(const std::wstring& setting, const fs::path& langDir) {
    std::wstring want = (setting.empty() || Lower(setting) == L"auto") ? SystemLanguage() : setting;
    if (Lower(want) == kPseudoLanguage) return kPseudoLanguage;  // no file: made from the English master
    if (!ValidCode(want)) return kSourceLanguage;
    if (want == kSourceLanguage) return want;
    std::error_code ec;
    if (fs::exists(langDir / (want + L".tsv"), ec)) return want;
    return kSourceLanguage;
}

size_t Load(const fs::path& langDir, const std::wstring& code) {
    Table().clear();
    EnglishTable().clear();
    ShownToEnglish().clear();
    CurrentLang() = kSourceLanguage;
    auto read = [&](const std::wstring& lang) -> size_t {
        std::ifstream in(langDir / (lang + L".tsv"), std::ios::binary);
        if (!in) return 0;
        std::string data((std::istreambuf_iterator<char>(in)), {});
        return ParseTable(data, Table());
    };
    const size_t master = read(kSourceLanguage);  // every id has its English text, so a language that lacks one shows English
    EnglishTable() = Table();
    if (code == kPseudoLanguage) {
        for (auto& kv : Table()) kv.second = Pseudo(kv.second);
        CurrentLang() = kPseudoLanguage;
        return master;
    }
    if (code == kSourceLanguage || !ValidCode(code)) return master;
    std::ifstream probe(langDir / (code + L".tsv"), std::ios::binary);
    if (!probe) return 0;
    probe.close();
    const size_t n = read(code);
    CurrentLang() = code;
    return n;
}

const std::wstring& Current() { return CurrentLang(); }
bool IsSourceLanguage() { return CurrentLang() == kSourceLanguage; }
std::wstring UiLanguageTag() {
    const std::wstring& cur = CurrentLang();
    if (cur == kPseudoLanguage || cur == SystemLanguage()) return {};  // the language of Windows is what XAML uses anyway
    if (cur == L"zh-TW") return L"zh-Hant-TW";                        // with the script: the font fallback goes by it
    if (cur == L"zh-CN") return L"zh-Hans-CN";
    return cur;
}

std::wstring EnglishOf(std::wstring_view shown) {
    if (shown.empty() || IsSourceLanguage()) return {};
    auto& map = ShownToEnglish();
    if (map.empty())
        for (auto const& [id, text] : Table())
            if (auto en = EnglishTable().find(id); en != EnglishTable().end() && en->second != text) map.emplace(text, en->second);
    auto it = map.find(std::wstring(shown));
    return it == map.end() ? std::wstring() : it->second;
}

}  // namespace i18n

const wchar_t* Tr(const wchar_t* source) {
    if (!source) return L"";
    auto& t = Table();
    if (t.empty()) return source;
    auto it = t.find(source);
    return it == t.end() ? source : it->second.c_str();
}

namespace i18n_detail {
// zeroIsOne: the language uses the singular form for 0 too (French: "0 fichier").
static std::wstring FormatText(const std::wstring& fmt, const std::vector<std::wstring>& args, bool zeroIsOne = false) {
    std::wstring out;
    out.reserve(fmt.size() + 16);
    for (size_t i = 0; i < fmt.size(); ++i) {
        if (fmt[i] == L'{') {
            size_t j = i + 1, idx = 0;
            bool digits = false;
            while (j < fmt.size() && fmt[j] >= L'0' && fmt[j] <= L'9') { idx = idx * 10 + static_cast<size_t>(fmt[j] - L'0'); ++j; digits = true; }
            if (digits && j < fmt.size() && fmt[j] == L'}') {
                if (idx < args.size()) out += args[idx];
                i = j;
                continue;
            }
            // Plural {N:one|other}: the argument is 1 (or 0 in French) -> the first form, otherwise the second (for example "{0} {0:file|files}")
            if (digits && j < fmt.size() && fmt[j] == L':') {
                size_t close = fmt.find(L'}', j);
                if (close != std::wstring::npos) {
                    std::wstring spec = fmt.substr(j + 1, close - j - 1);
                    size_t bar = spec.find(L'|');
                    if (bar != std::wstring::npos) {
                        const bool one = idx < args.size() && (args[idx] == L"1" || (zeroIsOne && args[idx] == L"0"));
                        out += one ? spec.substr(0, bar) : spec.substr(bar + 1);
                        i = close;
                        continue;
                    }
                }
            }
        }
        out += fmt[i];
    }
    return out;
}
std::wstring FormatImpl(const wchar_t* source, const std::vector<std::wstring>& args) {
    return FormatText(Tr(source), args, CurrentLang().rfind(L"fr", 0) == 0);
}
std::wstring FormatEnglish(const std::wstring& id, const std::vector<std::wstring>& args) {
    struct Entry { const wchar_t* id; const wchar_t* text; };
    static const Entry kCore[] = {
#include "core_messages.inc"
        {nullptr, nullptr}};
    for (const Entry* e = kCore; e->id; ++e)
        if (id == e->id) return FormatText(e->text, args);
    return FormatText(id, args);  // an unknown id is shown as it is
}
}  // namespace i18n_detail

namespace i18n {
std::wstring English(const std::wstring& id, const std::vector<std::wstring>& args) { return i18n_detail::FormatEnglish(id, args); }
}  // namespace i18n

}  // namespace lycan
