// i18n: language file parsing, lookup by id with the English master as the fallback, placeholders and plurals, language tag mapping,
// language resolving and loading.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

#include "lycan/error.h"
#include "lycan/i18n.h"
#include "lycan/languages.h"

namespace fs = std::filesystem;
using namespace lycan;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static void WriteFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

int main() {
    // ---- parsing ----
    {
        std::unordered_map<std::wstring, std::wstring> t;
        std::string data =
            "\xEF\xBB\xBF# comment\r\n"
            "## section\r\n"
            "main.extract\tExtract\r\n"
            "main.total\tTotal {0}\tabcdef12\r\n"                // a third column (the hash) is not part of the text
            "main.two_lines\tline1\\nline2\\ttab\\\\end\r\n"     // escapes
            "main.untranslated\t\r\n"                            // empty = not translated
            "no tab on this line\r\n"
            "main.chinese\t\xE8\xA7\xA3\xE5\xA3\x93\xE7\xB8\xAE\r\n";  // UTF-8
        size_t n = i18n::ParseTable(data, t);
        EXPECT(n == 4);
        EXPECT(t.size() == 4);
        EXPECT(t[L"main.extract"] == L"Extract");
        EXPECT(t[L"main.total"] == L"Total {0}");
        EXPECT(t[L"main.two_lines"] == L"line1\nline2\ttab\\end");
        EXPECT(t[L"main.chinese"] == L"解壓縮");
        EXPECT(t.count(L"main.untranslated") == 0);
    }

    // ---- language tags ----
    EXPECT(i18n::MapLanguageTag(L"zh-TW") == L"zh-TW");
    EXPECT(i18n::MapLanguageTag(L"zh-Hant-TW") == L"zh-TW");
    EXPECT(i18n::MapLanguageTag(L"zh-HK") == L"zh-TW");
    EXPECT(i18n::MapLanguageTag(L"zh-CN") == L"zh-CN");
    EXPECT(i18n::MapLanguageTag(L"zh-Hans-CN") == L"zh-CN");
    EXPECT(i18n::MapLanguageTag(L"zh-SG") == L"zh-CN");
    EXPECT(i18n::MapLanguageTag(L"en-GB") == L"en-US");
    EXPECT(i18n::MapLanguageTag(L"ja-JP") == L"en-US");  // a language that is not supported: English

    // ---- the list of languages (languages.h) ----
    EXPECT(LanguageIndex(L"en-US") >= 0 && LanguageIndex(L"EN-us") == LanguageIndex(L"en-US"));
    EXPECT(LanguageIndex(L"zh-CN") >= 0 && LanguageIndex(L"auto") == -1 && LanguageIndex(L"") == -1 && LanguageIndex(L"ja-JP") == -1);
    for (int i = 0; i < kLanguageCount; ++i) {
        EXPECT(LanguageIndex(kLanguages[i].code) == i);                       // no code twice
        EXPECT(LanguageForTag(kLanguages[i].code) == kLanguages[i].code);     // every language is what its own tag maps to
        EXPECT(std::wstring(kLanguages[i].nameId).rfind(L"lang.", 0) == 0);
    }

    // ---- loading, lookup, falling back to English ----
    fs::path dir = fs::temp_directory_path() / (L"lycan_i18n_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(dir);
    WriteFile(dir / "en-US.tsv",
              "# master\n"
              "main.extract\tExtract\t\n"
              "main.total\tTotal {0} items, {1}\t\n"
              "main.reorder\t{0} / {1}\t\n"
              "main.templates\t{0} {0:template|templates}\t\n"
              "main.only_english\tOnly in English {0}\t\n"
              "main.plain\tPlain {0}\t\n");
    WriteFile(dir / "zh-TW.tsv",
              "# zh-TW\n"
              "main.extract\t\xE8\xA7\xA3\xE5\xA3\x93\xE7\xB8\xAE\th1\n"
              "main.total\t\xE5\x85\xB1 {0} \xE5\x80\x8B\xE9\xA0\x85\xE7\x9B\xAE\xEF\xBC\x8C{1}\th2\n"
              "main.reorder\t{1} / {0}\th3\n"
              "main.templates\t{0} \xE5\x80\x8B\xE7\xAF\x84\xE6\x9C\xAC\th4\n");  // main.only_english has no Chinese text

    EXPECT(i18n::Current() == L"en-US" && i18n::IsSourceLanguage());
    EXPECT(std::wstring(Tr(L"main.extract")) == L"main.extract");  // nothing loaded yet: the id itself

    EXPECT(i18n::Resolve(L"en-US", dir) == L"en-US");
    EXPECT(i18n::Resolve(L"zh-TW", dir) == L"zh-TW");
    EXPECT(i18n::Resolve(L"zh-CN", dir) == L"en-US");     // no file: English
    EXPECT(i18n::Resolve(L"../evil", dir) == L"en-US");   // not a valid code
    EXPECT(i18n::Resolve(L"", dir) == i18n::Resolve(L"auto", dir));
    EXPECT(i18n::Resolve(L"auto", dir) == L"en-US" || i18n::Resolve(L"auto", dir) == L"zh-TW");

    EXPECT(i18n::Load(dir, L"en-US") == 6);
    EXPECT(i18n::Current() == L"en-US" && i18n::IsSourceLanguage());
    EXPECT(std::wstring(Tr(L"main.extract")) == L"Extract");
    EXPECT(std::wstring(Tr(L"main.no_such_id")) == L"main.no_such_id");  // unknown: the id as it is
    EXPECT(Tf(L"main.total", 5, L"ok") == L"Total 5 items, ok");
    EXPECT(Tf(L"main.reorder", L"A", L"B") == L"A / B");
    EXPECT(Tf(L"main.templates", 1) == L"1 template" && Tf(L"main.templates", 3) == L"3 templates" && Tf(L"main.templates", 0) == L"0 templates");  // plural

    EXPECT(i18n::Load(dir, L"zh-TW") == 4);
    EXPECT(i18n::Current() == L"zh-TW" && !i18n::IsSourceLanguage());
    EXPECT(std::wstring(Tr(L"main.extract")) == L"解壓縮");
    EXPECT(std::wstring(Tr(L"main.only_english")) == L"Only in English {0}");  // no Chinese text: English
    EXPECT(Tf(L"main.total", 5, L"ok") == L"共 5 個項目，ok");
    EXPECT(Tf(L"main.reorder", L"A", L"B") == L"B / A");                         // a translation may reorder the arguments
    EXPECT(Tf(L"main.templates", 3) == L"3 個範本");
    EXPECT(Tf(L"main.only_english", 1) == L"Only in English 1");
    EXPECT(Tf(L"main.plain", 1, 2) == L"Plain 1");                               // an argument too many is dropped
    EXPECT(Tf(L"main.only_english") == L"Only in English ");                     // a missing argument is an empty string
    EXPECT(Tf(L"{x} {0", 1) == L"{x} {0");                                       // not a placeholder: as it is

    // Message: Text() is in the language of the user, English() is always English (the table is compiled into the Core)
    EXPECT(Message::Make(L"main.total", 5, L"ok").Text() == L"共 5 個項目，ok");
    const Message link = Message::Make(L"core.skipped_link", L"C:\\a");
    EXPECT(link.English() == L"C:\\a (symbolic link / junction)");
    EXPECT(link.id == L"core.skipped_link" && link.args.size() == 1 && !link.empty());
    EXPECT(Message(L"plain").English() == L"plain" && Message(L"plain").Text() == L"plain");  // text without an id: as it is
    EXPECT(Message().empty() && Message(L"").empty());
    EXPECT(Message::Make(L"core.no_such_id", 1).English() == L"core.no_such_id");              // unknown id: the id
    // a message as the argument of another one (the reason of an error): translated together, English stays English
    const Message nested = Message::Make(L"core.path_reason", L"a.txt", Message::Make(L"core.op_crc_error"));
    EXPECT(nested.English() == L"a.txt: CRC error");
    EXPECT(Message::Make(L"core.path_reason", L"a.txt", Message(L"plain reason")).English() == L"a.txt: plain reason");
    const Error withId(E_FAIL, link);
    EXPECT(std::string(withId.what()) == ToUtf8(link.English()) && withId.message().id == link.id && withId.message().args == link.args);
    const Error plainErr(E_FAIL, "narrow text");
    EXPECT(plainErr.message().id.empty() && plainErr.message().raw == L"narrow text" && std::string(plainErr.what()) == "narrow text");

    // the pseudo language: accents, 40 % longer, brackets; placeholders and plural forms are untouched
    EXPECT(i18n::Resolve(L"pseudo", dir) == L"pseudo" && i18n::Resolve(L"PSEUDO", dir) == L"pseudo");
    EXPECT(i18n::Load(dir, L"pseudo") == 6 && i18n::Current() == L"pseudo" && !i18n::IsSourceLanguage());
    EXPECT(std::wstring(Tr(L"main.extract")) == L"[\u00c9xtr\u00e0\u00e7t \u00b7\u00b7\u00b7]");
    EXPECT(Tf(L"main.total", 5, L"ok") == L"[T\u00f6t\u00e0l 5 \u00eet\u00e9ms, ok \u00b7\u00b7\u00b7\u00b7\u00b7]" || Tf(L"main.total", 5, L"ok").find(L"5") != std::wstring::npos);
    EXPECT(Tf(L"main.templates", 1).find(L"1 ") != std::wstring::npos && Tf(L"main.templates", 3).find(L"templates") != std::wstring::npos);
    EXPECT(Message::Make(L"main.total", 5, L"ok").Text().front() == L'[');
    EXPECT(Message::Make(L"core.skipped_link", L"C:\\a").English() == L"C:\\a (symbolic link / junction)");  // English() ignores the language

    EXPECT(i18n::Load(dir, L"fr-FR") == 0 && i18n::IsSourceLanguage());          // no file: English
    EXPECT(std::wstring(Tr(L"main.extract")) == L"Extract");
    EXPECT(i18n::Load(dir, L"..\\en-US") == 6 && i18n::IsSourceLanguage());     // an invalid code does not leave the folder: English

    fs::remove_all(dir);
    if (failures == 0) std::puts("test_i18n: all passed");
    return failures ? 1 : 0;
}
