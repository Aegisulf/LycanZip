// LycanZip context menu (IExplorerCommand). This DLL is loaded into explorer.exe, so it is deliberately small and does not
// depend on Core: it decides what to show, lists the sub menu and hands the selected paths to LycanZip.exe (command line;
// a --list file when there are many paths). No compression or extraction logic lives here, so a bug cannot slow down or
// take down Explorer. Every entry point is wrapped in try/catch.
//
// Two menu classes, each in two registrations (the same code, different CLSIDs):
//   FilesCommand   : LycanZip > Add to archive... / Compress to "name.zip" / ...   (hidden when everything is an archive)
//   ArchiveCommand : LycanZip > Open / Extract here / Extract to "name\" / ...     (shown only when everything is an archive)
// "Classic" CLSIDs are registered under HKCU for the classic menu (ExplorerCommandHandler verbs, Windows 10 and "Show
// more options" of Windows 11); the others belong to the Windows 11 sparse package and the Store package (their manifests are
// made by the release flow).
//
// What is shown comes from settings.ini (shell_items, shell_format_1/2, language, recent_extract_dirs) and presets.ini,
// read from the folder of this DLL when LycanZip.portable is there, from the LocalState of the package in the Microsoft Store version,
// otherwise %LOCALAPPDATA%\LycanZip (SettingsDir). Changing a setting
// therefore needs no re-registration. Titles are dynamic ("Compress to "project.zip"") and translated from a small table.
#include <windows.h>
#include <appmodel.h>
#include <objbase.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <wrl/implements.h>
#include <wrl/module.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "lycan/fileicons.h"
#include "lycan/languages.h"
#include "lycan/shellitems.h"

using namespace Microsoft::WRL;

namespace {

// Keep in step with src/core/shellreg.cpp
const wchar_t* const kArchiveExts[] = {L".zip", L".7z",  L".rar", L".tar", L".gz",  L".tgz", L".xz",  L".txz",
                                       L".bz2", L".tbz2", L".tbz", L".iso", L".cab", L".arj", L".lzh", L".z",
                                       L".wim", L".zst", L".001", L".cbz", L".cbr", L".cb7", L".cbt"};

HMODULE g_module = nullptr;

// ---------------------------------------------------------------------------------------------------------------------
// Files

std::wstring ModuleDir() {  // with a trailing backslash
    wchar_t dll[MAX_PATH * 2];
    const DWORD m = GetModuleFileNameW(g_module, dll, static_cast<DWORD>(std::size(dll)));
    std::wstring dir(dll, m);
    const auto pos = dir.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : dir.substr(0, pos + 1);
}

// The package family when this DLL is part of a package (the Microsoft Store version): it lives in
// ...\WindowsApps\<package full name>\, whichever process loaded it
std::wstring PackageFamilyOfModule() {
    std::wstring dir = ModuleDir();
    if (dir.size() < 2) return {};
    dir.pop_back();
    const auto slash = dir.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 12 || _wcsicmp(dir.substr(slash - 12, 12).c_str(), L"\\WindowsApps") != 0) return {};
    const std::wstring fullName = dir.substr(slash + 1);
    wchar_t family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1] = {};
    UINT32 len = static_cast<UINT32>(std::size(family));
    return PackageFamilyNameFromFullName(fullName.c_str(), &len, family) == ERROR_SUCCESS ? std::wstring(family) : std::wstring();
}

std::wstring SettingsDir() {  // with a trailing backslash; the same rule as lycan::UserDataDir (datalocation.h)
    wchar_t env[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_SETTINGS_DIR", env, static_cast<DWORD>(std::size(env)));  // tests
    if (n > 0 && n < std::size(env)) return std::wstring(env) + L"\\";
    const std::wstring dir = ModuleDir();
    if (PathFileExistsW((dir + L"LycanZip.portable").c_str())) return dir;
    PWSTR local = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)) && local) base = local;
    CoTaskMemFree(local);
    if (base.empty()) return dir;
    // the Store package: its own LocalState; until the program has run once there, the old place
    if (const std::wstring family = PackageFamilyOfModule(); !family.empty()) {
        const std::wstring state = base + L"\\Packages\\" + family + L"\\LocalState\\";
        if (PathFileExistsW((state + L"settings.ini").c_str()) || PathFileExistsW((state + L"imported").c_str())) return state;
    }
    return base + L"\\LycanZip\\";
}

std::string ReadFileUtf8(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string out;
    char buf[8192];
    DWORD got = 0;
    while (out.size() < (1u << 20) && ReadFile(h, buf, sizeof(buf), &got, nullptr) && got) out.append(buf, got);
    CloseHandle(h);
    if (out.size() >= 3 && out.compare(0, 3, "\xEF\xBB\xBF") == 0) out.erase(0, 3);
    return out;
}

std::wstring ExePath() {
    wchar_t env[MAX_PATH * 2];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_EXE", env, static_cast<DWORD>(std::size(env)));  // tests
    if (n > 0 && n < std::size(env)) return env;
    const std::wstring beside = ModuleDir() + L"LycanZip.exe";
    if (PathFileExistsW(beside.c_str())) return beside;
    // an installed copy keeps this DLL outside the folder of the exe (that folder is replaced by an update): EnsureShellHome writes where the exe is
    std::string utf8 = ReadFileUtf8(ModuleDir() + L"LycanZip.exepath");
    while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' ')) utf8.pop_back();
    if (!utf8.empty()) {
        const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (len > 0) {
            std::wstring path(static_cast<size_t>(len), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), path.data(), len);
            return path;
        }
    }
    return beside;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Trim(std::string s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings (cached; re-read when settings.ini or presets.ini changes)

struct Config {
    std::string items;                        // shell_items ("" = defaults)
    std::string builtinPresets;               // shell_presets ("" = defaults)
    std::wstring fmt1 = L"zip", fmt2 = L"7z";
    std::wstring lang = L"auto";
    std::vector<std::wstring> recent;         // recent_extract_dirs
    std::vector<std::wstring> presets;        // names of the user's presets (presets.ini)
    bool menuOff = false;                     // shell_menu=false: no entries at all (the switch of the Store version, whose menu Windows keeps registered)
};

bool KnownFormat(const std::wstring& f) { return f == L"zip" || f == L"7z" || f == L"tar.gz" || f == L"tar.xz" || f == L"tar.bz2"; }

struct Stamp {
    FILETIME t{};
    DWORD size = 0;
    bool operator==(const Stamp& o) const { return t.dwLowDateTime == o.t.dwLowDateTime && t.dwHighDateTime == o.t.dwHighDateTime && size == o.size; }
};
Stamp StampOf(const std::wstring& path) {
    Stamp s;
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) { s.t = d.ftLastWriteTime; s.size = d.nFileSizeLow; }
    return s;
}

Config ParseConfig(const std::string& settings, const std::string& presets) {
    Config c;
    size_t pos = 0;
    while (pos < settings.size()) {
        size_t end = settings.find('\n', pos);
        if (end == std::string::npos) end = settings.size();
        const std::string line = Trim(settings.substr(pos, end - pos));
        pos = end + 1;
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k == "shell_items") c.items = lycan::NormalizeShellItems(v);
        else if (k == "shell_presets") c.builtinPresets = lycan::NormalizeBuiltinPresets(v);
        else if (k == "shell_format_1") { if (KnownFormat(FromUtf8(v))) c.fmt1 = FromUtf8(v); }
        else if (k == "shell_format_2") { if (KnownFormat(FromUtf8(v))) c.fmt2 = FromUtf8(v); }
        else if (k == "language") c.lang = FromUtf8(v);
        else if (k == "shell_menu") c.menuOff = v == "false" || v == "0" || v == "no" || v == "off";
        else if (k == "recent_extract_dirs") {
            size_t p = 0;
            while (p <= v.size() && c.recent.size() < 5) {
                size_t e = v.find('|', p);
                if (e == std::string::npos) e = v.size();
                const std::wstring w = FromUtf8(v.substr(p, e - p));
                if (w.size() >= 3 && PathIsRelativeW(w.c_str()) == FALSE) c.recent.push_back(w);
                p = e + 1;
            }
        }
    }
    // presets.ini: "[name]" sections; a section with in_menu=false is not listed
    pos = 0;
    std::wstring section;  // the name of the section being read, "" = none that counts
    while (pos < presets.size()) {
        size_t end = presets.find('\n', pos);
        if (end == std::string::npos) end = presets.size();
        const std::string line = Trim(presets.substr(pos, end - pos));
        pos = end + 1;
        if (line.size() >= 3 && line.front() == '[' && line.back() == ']') {
            section.clear();
            const std::wstring name = FromUtf8(Trim(line.substr(1, line.size() - 2)));
            if (name.empty() || name.size() > 40 || name.find_first_of(L"[]") != std::wstring::npos) continue;
            bool builtin = false;
            for (auto* b : lycan::kBuiltinPresetNames) if (name == b) builtin = true;
            if (!builtin && std::find(c.presets.begin(), c.presets.end(), name) == c.presets.end()) { c.presets.push_back(name); section = name; }
            continue;
        }
        const auto eq = line.find('=');
        if (section.empty() || eq == std::string::npos) continue;
        if (Trim(line.substr(0, eq)) == "in_menu") {
            const std::string v = Trim(line.substr(eq + 1));
            if (v == "false" || v == "0" || v == "no" || v == "off") c.presets.erase(std::remove(c.presets.begin(), c.presets.end(), section), c.presets.end());
            section.clear();
        }
    }
    return c;
}

Config LoadConfig() {
    static std::mutex m;
    static Config cached;
    static Stamp s1, s2;
    static bool have = false;
    std::lock_guard<std::mutex> lk(m);
    const std::wstring dir = SettingsDir();
    const Stamp a = StampOf(dir + L"settings.ini"), b = StampOf(dir + L"presets.ini");
    if (!have || !(a == s1) || !(b == s2)) {
        cached = ParseConfig(ReadFileUtf8(dir + L"settings.ini"), ReadFileUtf8(dir + L"presets.ini"));
        s1 = a;
        s2 = b;
        have = true;
    }
    return cached;
}

// ---------------------------------------------------------------------------------------------------------------------
// Translations: the texts are in the string catalog of the app (ids "shell.*", app/Assets/Lang); tools/i18n.py --update writes them to
// shell_texts.inc, so that this DLL (it runs inside Explorer) has them compiled in and reads no file. {0} is replaced by an argument.
struct Text { const wchar_t* id; const wchar_t* text[lycan::kLanguageCount]; };   // one text for each language of lycan::kLanguages (the same order)
// the texts of the built-in presets, in the order of lycan::kBuiltinPresetNames (the names themselves are the command-line values)
const wchar_t* const kBuiltinPresetTextIds[] = {L"shell.preset_zip", L"shell.preset_7z", L"shell.preset_smallest", L"shell.preset_fast",
                                                L"shell.preset_mail", L"shell.preset_backup", L"shell.preset_store"};
static_assert(sizeof(kBuiltinPresetTextIds) / sizeof(kBuiltinPresetTextIds[0]) == lycan::kBuiltinPresetCount, "one text per built-in preset");
const Text kTexts[] = {
#include "shell_texts.inc"
};

int LangIndex(const std::wstring& lang) {  // the index in lycan::kLanguages
    int i = lycan::LanguageIndex(lang);
    if (i >= 0) return i;
    wchar_t tag[LOCALE_NAME_MAX_LENGTH] = {};
    std::wstring code = L"en-US";   // "auto": the system language
    if (LCIDToLocaleName(MAKELCID(GetUserDefaultUILanguage(), SORT_DEFAULT), tag, LOCALE_NAME_MAX_LENGTH, 0) > 0) code = lycan::LanguageForTag(tag);
    i = lycan::LanguageIndex(code);
    return i >= 0 ? i : lycan::LanguageIndex(L"en-US");
}

std::wstring Tr(const wchar_t* id, const Config& c) {
    const int li = LangIndex(c.lang);
    const int en = lycan::LanguageIndex(L"en-US");
    for (const auto& t : kTexts)
        if (wcscmp(t.id, id) == 0) return t.text[li] && *t.text[li] ? t.text[li] : t.text[en];
    return id;
}

std::wstring Fmt(std::wstring s, const std::wstring& arg, const wchar_t* placeholder = L"{0}") {
    const auto pos = s.find(placeholder);
    if (pos != std::wstring::npos) s.replace(pos, 3, arg);
    return s;
}

// Long names are cut in the middle so that the menu stays readable
std::wstring Shorten(const std::wstring& s, size_t max = 24) {
    if (s.size() <= max) return s;
    const size_t head = (max - 1) / 2, tail = max - 1 - head;
    return s.substr(0, head) + L"…" + s.substr(s.size() - tail);
}

// ---------------------------------------------------------------------------------------------------------------------
// The selection

bool HasArchiveExt(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    return std::any_of(std::begin(kArchiveExts), std::end(kArchiveExts), [&](const wchar_t* e) { return _wcsicmp(ext, e) == 0; });
}

bool IsArchive(const std::wstring& path) { return HasArchiveExt(path) && !PathIsDirectoryW(path.c_str()); }

// Checksum files (.sha256 ...: keep in step with kSumExts of src/core/shellreg.cpp): their entry is "Verify the checksum file"
const wchar_t* const kSumExts[] = {L".sha256", L".sha1", L".sha384", L".sha512", L".md5", L".sfv"};
bool IsSumFile(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    return std::any_of(std::begin(kSumExts), std::end(kSumExts), [&](const wchar_t* e) { return _wcsicmp(ext, e) == 0; }) && !PathIsDirectoryW(path.c_str());
}
bool AllSumFiles(const std::vector<std::wstring>& paths) { return !paths.empty() && std::all_of(paths.begin(), paths.end(), [](const std::wstring& p) { return IsSumFile(p); }); }

bool HasContentExt(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    return std::any_of(std::begin(lycan::kContentExts), std::end(lycan::kContentExts), [&](const wchar_t* e) { return _wcsicmp(ext, e) == 0; });
}

// The paths of the selection; any item that is not a file system path (This PC, virtual search items) -> fails
bool GetPaths(IShellItemArray* items, std::vector<std::wstring>& out) {
    if (!items) return false;
    DWORD n = 0;
    if (FAILED(items->GetCount(&n)) || n == 0) return false;
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> it;
        if (FAILED(items->GetItemAt(i, &it))) return false;
        PWSTR p = nullptr;
        if (FAILED(it->GetDisplayName(SIGDN_FILESYSPATH, &p)) || !p) return false;
        std::wstring s = p;
        CoTaskMemFree(p);
        while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();  // a trailing '\' would eat the closing quote
        out.push_back(std::move(s));
    }
    return true;
}

std::wstring LastComponent(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

struct Selection {
    std::vector<std::wstring> paths;
    bool allArchives = false;
    bool single = false;
    std::wstring compressName;  // "project" for the title Compress to "project.zip"
    std::wstring extractName;   // "project" for Extract to "project\"
    std::wstring sumName;       // "project.zip" for Calculate and save as "project.zip.sha256": one item = its name, several = the folder they are in
    bool contentFile = false;   // a single .exe / .docx ... (open its contents)
};

// Same rule as the app's DefaultArchiveOutput: a single file -> its name without the extension, a folder -> its name,
// several items -> the name of the folder they are in
Selection Analyze(std::vector<std::wstring> paths) {
    Selection s;
    s.paths = std::move(paths);
    s.allArchives = !s.paths.empty() && std::all_of(s.paths.begin(), s.paths.end(), [](const std::wstring& p) { return IsArchive(p); });
    s.single = s.paths.size() == 1;
    if (s.paths.empty()) return s;
    const std::wstring& first = s.paths[0];
    if (s.single) {
        s.sumName = LastComponent(first);
        const std::wstring name = LastComponent(first);
        if (PathIsDirectoryW(first.c_str())) s.compressName = name;
        else {
            const wchar_t* ext = PathFindExtensionW(name.c_str());
            const size_t extPos = static_cast<size_t>(ext - name.c_str());
            s.compressName = extPos == 0 ? name : name.substr(0, extPos);
            s.contentFile = HasContentExt(name);
        }
        s.extractName = lycan::ArchiveBaseNameOf(name);
    } else {
        const auto pos = first.find_last_of(L"\\/");
        if (pos != std::wstring::npos) s.compressName = LastComponent(first.substr(0, pos));
        s.sumName = s.compressName;
    }
    if (s.sumName.empty() || s.sumName.back() == L':') s.sumName = L"checksums";
    if (s.compressName.empty() || s.compressName.back() == L':') s.compressName = L"Archive";
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// The menu as data: built from the selection and the settings, turned into IExplorerCommand objects when Explorer asks

struct Node {
    std::wstring title;
    std::wstring flags;          // command line before the paths; empty with eachOpen: open every archive on its own
    bool eachOpen = false;
    std::wstring icon;           // "file,index" for the entry's icon; empty = none
    std::vector<Node> kids;
    bool separator = false;      // a line between entries (the Windows 11 menu, FlatMenu)
};

// Icons of the top-level entries, all resources of LycanZip.exe ("exe,-id"; app/LycanZip.rc): the LycanZip icon for adding and
// opening content, the file-type icon of the format for compressing to a format and for opening an archive (lycan/fileicons.h:
// 101..106), and a wolf with a badge for what the entry does (111..117, assets/design/tools/gen_menu_icons.py). The preset /
// format / folder lists inside a sub menu have none.
constexpr int kIconExtract = 111, kIconTest = 112, kIconImages = 113, kIconAudio = 114, kIconEncrypt = 115, kIconHash = 116, kIconTemplate = 117;

std::wstring ExeIcon(int id) { return ExePath() + L",-" + std::to_wstring(id); }
std::wstring FamilyIcon(lycan::IconFamily f) { return ExeIcon(lycan::IconResourceId(f)); }

std::wstring IconFor(const Node& n, const std::vector<std::wstring>& paths) {
    if (n.eachOpen) {  // open: the icon of the archives' type when they are all of one type
        if (paths.empty()) return ExePath() + L",0";
        const lycan::IconFamily f = lycan::IconFamilyForPath(paths[0]);
        for (auto& p : paths)
            if (lycan::IconFamilyForPath(p) != f) return ExePath() + L",0";
        return FamilyIcon(f);
    }
    const std::wstring& f = n.kids.empty() ? n.flags : n.kids[0].flags;  // a sub menu looks like its first entry
    auto starts = [&](const wchar_t* p) { return f.rfind(p, 0) == 0; };
    if (f.find(L"--template") != std::wstring::npos) return ExeIcon(kIconTemplate);
    if (starts(L"--add-encrypted")) return ExeIcon(kIconEncrypt);
    if (starts(L"--extract")) return ExeIcon(kIconExtract);
    if (starts(L"--test")) return ExeIcon(kIconTest);
    if (starts(L"--preview-images")) return ExeIcon(kIconImages);
    if (starts(L"--preview-audio")) return ExeIcon(kIconAudio);
    if (starts(L"--hash")) return ExeIcon(kIconHash);
    if (starts(L"--compress")) {  // "--compress zip", "--compress-each 7z", "--compress tar.gz": the icon of that format
        const size_t sp = f.find(L' ');
        if (sp != std::wstring::npos) {
            std::wstring fmt = f.substr(sp + 1);
            fmt = fmt.substr(0, fmt.find(L' '));
            const size_t dot = fmt.rfind(L'.');
            return FamilyIcon(lycan::IconFamilyForExtension(dot == std::wstring::npos ? fmt : fmt.substr(dot + 1)));
        }
    }
    if (starts(L"--add") || starts(L"--open-content")) return ExePath() + L",0";
    return {};
}

std::wstring Quote(const std::wstring& s) {  // as one argument, the way CommandLineToArgvW reads it back
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') { ++backslashes; out += c; continue; }
        if (c == L'"') out.append(backslashes + 1, L'\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes, L'\\');
    return out + L"\"";
}

std::wstring ExtOf(const std::wstring& fmt) { return L"." + fmt; }
std::wstring FormatLabel(const std::wstring& fmt) { return fmt == L"zip" ? L"ZIP" : fmt; }

std::vector<Node> FilesMenu(const Selection& sel, const Config& c) {
    std::vector<Node> v;
    if (c.menuOff) return v;
    auto on = [&](const char* id) { return lycan::ShellItemEnabled(c.items, id); };
    auto leaf = [&](const std::wstring& title, const std::wstring& flags) { Node n; n.title = title; n.flags = flags; return n; };
    if (on("add")) v.push_back(leaf(Tr(L"shell.add_to_archive", c), L"--add"));
    if (on("fmt1")) v.push_back(leaf(Fmt(Tr(L"shell.compress_to", c), Shorten(sel.compressName) + ExtOf(c.fmt1)), L"--compress " + c.fmt1));
    if (on("fmt2")) v.push_back(leaf(Fmt(Tr(L"shell.compress_to", c), Shorten(sel.compressName) + ExtOf(c.fmt2)), L"--compress " + c.fmt2));
    if (on("each") && sel.paths.size() >= 2) v.push_back(leaf(Fmt(Tr(L"shell.compress_each", c), FormatLabel(c.fmt1)), L"--compress-each " + c.fmt1));
    if (on("template")) {
        Node n;
        n.title = Tr(L"shell.compress_with_preset", c);
        for (size_t i = 0; i < lycan::kBuiltinPresetCount; ++i)
            if (lycan::BuiltinPresetEnabled(c.builtinPresets, i)) n.kids.push_back(leaf(Tr(kBuiltinPresetTextIds[i], c), L"--compress zip --template " + Quote(lycan::kBuiltinPresetNames[i])));
        for (auto& p : c.presets) n.kids.push_back(leaf(p, L"--compress zip --template " + Quote(p)));
        if (!n.kids.empty()) v.push_back(std::move(n));  // no presets chosen: no sub menu
    }
    if (on("more")) {
        Node n;
        n.title = Tr(L"shell.other_formats", c);
        for (const wchar_t* f : {L"tar.gz", L"tar.xz", L"tar.bz2"}) n.kids.push_back(leaf(FormatLabel(f), std::wstring(L"--compress ") + f));
        v.push_back(std::move(n));
    }
    if (on("encrypt")) v.push_back(leaf(Tr(L"shell.encrypted", c), L"--add-encrypted"));
    if (on("opencontent") && sel.contentFile) v.push_back(leaf(Tr(L"shell.open_content", c), L"--open-content"));
    if (on("hash")) v.push_back(leaf(Tr(L"shell.hash", c), L"--hash"));
    if (on("hashsave")) v.push_back(leaf(Fmt(Tr(L"shell.hash_save", c), Shorten(sel.sumName + L".sha256")), L"--hash-save"));
    return v;
}

std::vector<Node> ArchiveMenu(const Selection& sel, const Config& c) {
    std::vector<Node> v;
    if (c.menuOff) return v;
    auto on = [&](const char* id) { return lycan::ShellItemEnabled(c.items, id); };
    auto leaf = [&](const std::wstring& title, const std::wstring& flags) { Node n; n.title = title; n.flags = flags; return n; };
    const bool many = sel.paths.size() >= 2;
    if (on("open")) { Node n = leaf(Tr(L"shell.open", c), L""); n.eachOpen = true; v.push_back(std::move(n)); }
    if (on("here")) v.push_back(leaf(Tr(L"shell.extract_here", c), L"--extract-here"));
    if (on("folder"))
        v.push_back(leaf(many ? Tr(L"shell.extract_same_name", c) : Fmt(Tr(L"shell.extract_to_named", c), Shorten(sel.extractName)), L"--extract-folder"));
    if (on("to")) v.push_back(leaf(Tr(L"shell.extract_to", c), L"--extract-to"));
    if (on("recent") && !c.recent.empty()) {
        Node n;
        n.title = Tr(L"shell.extract_recent", c);
        for (auto& d : c.recent) n.kids.push_back(leaf(Shorten(d, 48), L"--extract-to --dest " + Quote(d)));
        v.push_back(std::move(n));
    }
    if (on("merge") && many) v.push_back(leaf(Tr(L"shell.extract_merge", c), L"--extract-merge"));
    if (on("test")) v.push_back(leaf(Tr(L"shell.test", c), L"--test"));
    if (on("convert")) v.push_back(leaf(Tr(L"shell.convert", c), L"--convert"));
    if (on("images") && !many) v.push_back(leaf(Tr(L"shell.image_preview", c), L"--preview-images"));
    if (on("audio") && !many) v.push_back(leaf(Tr(L"shell.audio_preview", c), L"--preview-audio"));
    if (on("hash")) v.push_back(leaf(Tr(L"shell.hash", c), L"--hash"));
    if (on("hashsave")) v.push_back(leaf(Fmt(Tr(L"shell.hash_save", c), Shorten(sel.sumName + L".sha256")), L"--hash-save"));
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------
// Starting LycanZip

bool Spawn(const std::wstring& args) {
    std::wstring cmd = L"\"" + ExePath() + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Many paths are written to a list file instead, to stay under the command-line limit (32767). The file name starts with
// LycanZip- and lives in %TEMP%; LycanZip.exe deletes it after reading.
bool Launch(const std::wstring& flags, const std::vector<std::wstring>& paths) {
    size_t total = flags.size();
    for (auto& p : paths) total += p.size() + 3;
    if (total < 8000) {
        std::wstring args = flags;
        for (auto& p : paths) args += L" \"" + p + L"\"";
        return Spawn(args);
    }
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return false;
    GUID g;
    CoCreateGuid(&g);
    wchar_t guid[64];
    StringFromGUID2(g, guid, static_cast<int>(std::size(guid)));
    std::wstring file = std::wstring(tmp) + L"LycanZip-" + std::wstring(guid + 1, wcslen(guid) - 2) + L".lst";
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    static const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    DWORD w;
    WriteFile(h, bom, 3, &w, nullptr);
    for (auto& p : paths) {
        int n = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), static_cast<int>(p.size()), nullptr, 0, nullptr, nullptr);
        std::string u(static_cast<size_t>(n) + 1, '\n');
        WideCharToMultiByte(CP_UTF8, 0, p.c_str(), static_cast<int>(p.size()), u.data(), n, nullptr, nullptr);
        WriteFile(h, u.data(), static_cast<DWORD>(u.size()), &w, nullptr);
    }
    CloseHandle(h);
    return Spawn(flags + L" --list \"" + file + L"\"");
}

// ---------------------------------------------------------------------------------------------------------------------
// COM objects

// A menu entry (a leaf that starts LycanZip, or a sub menu)
class SubCommand : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
public:
    void Init(Node node) { node_ = std::move(node); }

    IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* name) override { return SHStrDupW(node_.title.c_str(), name); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override {
        *icon = nullptr;
        return node_.icon.empty() ? E_NOTIMPL : SHStrDupW(node_.icon.c_str(), icon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tip) override { *tip = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override { *guid = GUID_NULL; return S_OK; }
    IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override { *state = ECS_ENABLED; return S_OK; }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override { *flags = node_.separator ? ECF_ISSEPARATOR : node_.kids.empty() ? ECF_DEFAULT : ECF_HASSUBCOMMANDS; return S_OK; }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** e) override;

    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        try {
            if (!node_.kids.empty() || node_.separator) return E_NOTIMPL;
            std::vector<std::wstring> paths;
            if (!GetPaths(items, paths)) return E_FAIL;
            if (node_.eachOpen) {  // several archives: one window each (a single path = open it)
                bool ok = true;
                for (auto& p : paths) ok = Launch(L"", {p}) && ok;
                return ok ? S_OK : E_FAIL;
            }
            return Launch(node_.flags, paths) ? S_OK : E_FAIL;
        } catch (...) {
            return E_FAIL;
        }
    }

private:
    Node node_;
};

class EnumCommands : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumExplorerCommand> {
public:
    void Init(std::vector<ComPtr<IExplorerCommand>> items, size_t pos = 0) { items_ = std::move(items); pos_ = pos; }

    IFACEMETHODIMP Next(ULONG count, IExplorerCommand** out, ULONG* fetched) override {
        ULONG n = 0;
        while (n < count && pos_ < items_.size()) {
            items_[pos_].CopyTo(&out[n]);
            ++pos_;
            ++n;
        }
        if (fetched) *fetched = n;
        return n == count ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Skip(ULONG n) override { pos_ = std::min(items_.size(), pos_ + n); return S_OK; }
    IFACEMETHODIMP Reset() override { pos_ = 0; return S_OK; }
    IFACEMETHODIMP Clone(IEnumExplorerCommand** e) override {
        auto c = Make<EnumCommands>();
        c->Init(items_, pos_);
        return c.CopyTo(e);
    }

private:
    std::vector<ComPtr<IExplorerCommand>> items_;
    size_t pos_ = 0;
};

HRESULT MakeEnum(const std::vector<Node>& nodes, IEnumExplorerCommand** e) {
    std::vector<ComPtr<IExplorerCommand>> subs;
    for (auto& n : nodes) {
        auto c = Make<SubCommand>();
        c->Init(n);
        subs.push_back(c);
    }
    auto en = Make<EnumCommands>();
    en->Init(std::move(subs));
    return en.CopyTo(e);
}

HRESULT SubCommand::EnumSubCommands(IEnumExplorerCommand** e) {
    *e = nullptr;
    if (node_.kids.empty()) return E_NOTIMPL;
    try { return MakeEnum(node_.kids, e); } catch (...) { return E_FAIL; }
}

// The top-level "LycanZip" entry. Explorer asks GetState (with the selection) first; the menu is built from that.
template <bool ArchiveMode>
class RootCommand : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand, IObjectWithSelection> {
public:
    IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* name) override { return SHStrDupW(L"LycanZip", name); }
    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override {
        std::wstring s = ExePath() + L",0";
        return SHStrDupW(s.c_str(), icon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tip) override { *tip = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override { *guid = GUID_NULL; return S_OK; }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override { *flags = ECF_HASSUBCOMMANDS; return S_OK; }
    IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override { return E_NOTIMPL; }

    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        *state = ECS_HIDDEN;
        BuildFrom(items);
        if (!menu_.empty()) *state = ECS_ENABLED;
        return S_OK;
    }

    // Explorer may enumerate the sub menu on an object that never saw GetState (the classic menu does): it hands the
    // selection over here instead
    IFACEMETHODIMP SetSelection(IShellItemArray* items) override {
        selection_ = items;
        BuildFrom(items);
        return S_OK;
    }
    IFACEMETHODIMP GetSelection(REFIID riid, void** out) override {
        if (!selection_) { *out = nullptr; return E_FAIL; }
        return selection_->QueryInterface(riid, out);
    }

    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** e) override {
        try { return MakeEnum(menu_, e); } catch (...) { *e = nullptr; return E_FAIL; }
    }

private:
    void BuildFrom(IShellItemArray* items) {
        try {
            std::vector<std::wstring> paths;
            if (!GetPaths(items, paths)) { menu_.clear(); return; }
            const Selection sel = Analyze(std::move(paths));
            menu_ = Build(sel);
            for (auto& n : menu_) n.icon = IconFor(n, sel.paths);
        } catch (...) {
            menu_.clear();
        }
    }
    ComPtr<IShellItemArray> selection_;
    static std::vector<Node> Build(const Selection& sel) {
        const Config c = LoadConfig();
        if (ArchiveMode) return sel.allArchives ? ArchiveMenu(sel, c) : std::vector<Node>{};
        return sel.allArchives ? std::vector<Node>{} : FilesMenu(sel, c);  // a mix of files and archives: only the files' entries
    }
    std::vector<Node> menu_;
};

// The entry of a checksum file (.sha256, .md5, .sfv ...): one command that checks every selected checksum file in one window
class VerifyCommand : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
public:
    IFACEMETHODIMP GetTitle(IShellItemArray*, PWSTR* name) override {
        try { return SHStrDupW(Tr(L"shell.verify_checksum", LoadConfig()).c_str(), name); } catch (...) { *name = nullptr; return E_FAIL; }
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override { return SHStrDupW(ExeIcon(kIconHash).c_str(), icon); }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tip) override { *tip = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override { *guid = GUID_NULL; return S_OK; }
    IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* state) override { *state = ECS_ENABLED; return S_OK; }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override { *flags = ECF_DEFAULT; return S_OK; }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** e) override { *e = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        try {
            std::vector<std::wstring> paths;
            if (!GetPaths(items, paths)) return E_FAIL;
            return Launch(L"--verify", paths) ? S_OK : E_FAIL;
        } catch (...) {
            return E_FAIL;
        }
    }
};

// The menu of a right-button drag ("Drag and drop handler": Explorer shows Copy here / Move here / Create shortcuts here and
// asks the registered handlers for more). It is an IContextMenu, not an IExplorerCommand: the dragged files come in the data
// object and the folder they were dropped on is the pidl of Initialize. Same items and settings as the context menu, but the
// archive goes into the drop folder (--dest). Registered under Directory, Drive and Folder (src/core/shellreg.cpp).
class DragDropMenu : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IShellExtInit, IContextMenu> {
public:
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE folder, IDataObject* data, HKEY) override {
        try {
            node_.clear();
            paths_.clear();
            dest_.clear();
            wchar_t dir[MAX_PATH * 2];
            if (!folder || !SHGetPathFromIDListW(folder, dir)) return E_INVALIDARG;  // not a file system folder (Recycle Bin...)
            dest_ = dir;
            if (!data) return E_INVALIDARG;
            FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM sm{};
            if (FAILED(data->GetData(&fe, &sm))) return E_INVALIDARG;
            const HDROP drop = static_cast<HDROP>(GlobalLock(sm.hGlobal));
            if (drop) {
                const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < n; ++i) {
                    const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring s(len, L'\0');
                    DragQueryFileW(drop, i, s.data(), len + 1);
                    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
                    paths_.push_back(std::move(s));
                }
                GlobalUnlock(sm.hGlobal);
            }
            ReleaseStgMedium(&sm);
            return paths_.empty() ? E_INVALIDARG : S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT idFirst, UINT idLast, UINT flags) override {
        try {
            if ((flags & CMF_DEFAULTONLY) || paths_.empty() || !menu) return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
            const Config c = LoadConfig();
            const Selection sel = Analyze(paths_);
            std::vector<Node> v = sel.allArchives ? ArchiveMenu(sel, c) : FilesMenu(sel, c);
            // only the entries that make sense with a destination folder
            static const wchar_t* const kUseful[] = {L"--extract-here", L"--extract-folder", L"--compress", L"--compress-each"};
            for (auto& n : v) {
                const bool ok = n.kids.empty() && !n.eachOpen && std::any_of(std::begin(kUseful), std::end(kUseful), [&](const wchar_t* u) {
                    return n.flags == u || n.flags.rfind(std::wstring(u) + L" ", 0) == 0;
                }) && n.flags.find(L"--template") == std::wstring::npos;
                if (ok) node_.push_back(std::move(n));
            }
            if (node_.empty() || idFirst + node_.size() > idLast + 1) { node_.clear(); return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0); }
            HMENU sub = CreatePopupMenu();
            if (!sub) { node_.clear(); return E_FAIL; }
            for (size_t i = 0; i < node_.size(); ++i) AppendMenuW(sub, MF_STRING, idFirst + static_cast<UINT>(i), node_[i].title.c_str());
            MENUITEMINFOW mi{sizeof(mi)};
            mi.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
            mi.wID = idFirst + static_cast<UINT>(node_.size());
            mi.hSubMenu = sub;
            mi.dwTypeData = const_cast<LPWSTR>(L"LycanZip");
            if (!InsertMenuItemW(menu, index, TRUE, &mi)) { DestroyMenu(sub); node_.clear(); return E_FAIL; }
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, node_.size() + 1);
        } catch (...) {
            node_.clear();
            return E_FAIL;
        }
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override {
        try {
            if (!info || HIWORD(info->lpVerb) != 0) return E_INVALIDARG;
            const size_t i = LOWORD(info->lpVerb);
            if (i >= node_.size()) return E_INVALIDARG;
            return Launch(node_[i].flags + L" --dest " + Quote(dest_), paths_) ? S_OK : E_FAIL;
        } catch (...) {
            return E_FAIL;
        }
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override { return E_NOTIMPL; }

private:
    std::vector<Node> node_;
    std::vector<std::wstring> paths_;
    std::wstring dest_;
};


// ONE command for every kind of selection (the Microsoft Store version registers only this one, for * and Directory): the entries of the files, of the
// archives, or "Verify the checksum file", chosen by what is selected. Why one: Windows 11 puts the commands of a package that apply to the same
// item (here * and the extension of the archive) in a flyout of its own named like the app, so the archives got "LycanZip > LycanZip > entries".
HBITMAP IconBitmap(const std::wstring& location) {   // "file,index" (a negative index is a resource id) as a bitmap with alpha for a menu item
    if (location.empty()) return nullptr;
    const size_t comma = location.rfind(L',');
    const std::wstring file = comma == std::wstring::npos ? location : location.substr(0, comma);
    const int index = comma == std::wstring::npos ? 0 : _wtoi(location.c_str() + comma + 1);
    const int size = GetSystemMetrics(SM_CXSMICON);
    HICON icon = nullptr;
    UINT id = 0;
    if (PrivateExtractIconsW(file.c_str(), index, size, size, &icon, &id, 1, 0) != 1 || !icon) return nullptr;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp && bits) {
        HDC mem = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(mem, bmp);
        memset(bits, 0, static_cast<size_t>(size) * size * 4);
        DrawIconEx(mem, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
        SelectObject(mem, old);
        DeleteDC(mem);
    }
    ReleaseDC(nullptr, screen);
    DestroyIcon(icon);
    return bmp;
}

std::wstring MenuText(const std::wstring& s) {   // '&' makes a mnemonic in a menu: a name with an '&' would lose it
    std::wstring out;
    for (wchar_t c : s) { out += c; if (c == L'&') out += L'&'; }
    return out;
}

bool PathsOfDataObject(IDataObject* data, std::vector<std::wstring>& out) {
    if (!data) return false;
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (FAILED(data->GetData(&fe, &sm))) return false;
    const HDROP drop = static_cast<HDROP>(GlobalLock(sm.hGlobal));
    if (drop) {
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            const UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring s(len, L'\0');
            DragQueryFileW(drop, i, s.data(), len + 1);
            while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
            out.push_back(std::move(s));
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return !out.empty();
}

// What the one command shows for a selection
struct AutoMenu {
    bool verify = false;
    std::vector<Node> nodes;   // the entries (empty with verify)
    Selection sel;
};

// Windows 11 (build 22000 or later): the new menu, and the classic handlers of a package (desktop9). Windows 10 shows the command of the package
// (desktop4) in its one, classic menu and knows no desktop9 handlers. Tests: LYCAN_TEST_WIN_BUILD=<build>.
bool WindowsElevenShell() {
    wchar_t env[16];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_TEST_WIN_BUILD", env, static_cast<DWORD>(std::size(env)));
    if (n > 0 && n < std::size(env)) return _wtoi(env) >= 22000;
    static const bool eleven = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        RTL_OSVERSIONINFOW v{sizeof(v)};
        const auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
        return fn && fn(&v) == 0 && v.dwBuildNumber >= 22000;
    }();
    return eleven;
}

// The Windows 11 menu shows no sub menu inside the LycanZip menu ("subcommands that have subcommands are not supported" for
// IExplorerCommand::EnumSubCommands; such a flyout opens empty). There the entries of a sub menu are listed in place, between
// two lines, as "Compress with a template: Smallest size", each with the icon of its own command.
std::vector<Node> FlatMenu(std::vector<Node> nodes, const std::vector<std::wstring>& paths, const Config& c) {
    std::vector<Node> v;
    auto line = [&] { if (!v.empty() && !v.back().separator) { Node s; s.separator = true; v.push_back(std::move(s)); } };
    for (auto& n : nodes) {
        if (n.kids.empty()) { v.push_back(std::move(n)); continue; }
        line();
        for (auto& k : n.kids) {
            Node e = std::move(k);
            e.title = Fmt(Fmt(Tr(L"shell.group_entry", c), n.title), e.title, L"{1}");
            e.icon = IconFor(e, paths);
            v.push_back(std::move(e));
        }
        line();
    }
    if (!v.empty() && v.back().separator) v.pop_back();
    return v;
}

AutoMenu BuildAuto(std::vector<std::wstring> paths, bool flat) {
    AutoMenu m;
    const Config c = LoadConfig();
    m.sel = Analyze(std::move(paths));
    if (c.menuOff) return m;   // nothing, not even "Verify the checksum file"
    if (AllSumFiles(m.sel.paths)) { m.verify = true; return m; }
    m.nodes = m.sel.allArchives ? ArchiveMenu(m.sel, c) : FilesMenu(m.sel, c);   // a mix of files and archives: only the files' entries
    for (auto& n : m.nodes) n.icon = IconFor(n, m.sel.paths);
    if (flat) m.nodes = FlatMenu(std::move(m.nodes), m.sel.paths, c);
    return m;
}

class RootAuto : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand, IObjectWithSelection> {
public:
    IFACEMETHODIMP GetTitle(IShellItemArray* items, PWSTR* name) override {
        try {
            if (items) Build(items);
            return SHStrDupW(m_.verify ? Tr(L"shell.verify_checksum", LoadConfig()).c_str() : L"LycanZip", name);
        } catch (...) { *name = nullptr; return E_FAIL; }
    }
    IFACEMETHODIMP GetIcon(IShellItemArray*, PWSTR* icon) override {
        const std::wstring s = m_.verify ? ExeIcon(kIconHash) : ExePath() + L",0";
        return SHStrDupW(s.c_str(), icon);
    }
    IFACEMETHODIMP GetToolTip(IShellItemArray*, PWSTR* tip) override { *tip = nullptr; return E_NOTIMPL; }
    IFACEMETHODIMP GetCanonicalName(GUID* guid) override { *guid = GUID_NULL; return S_OK; }
    IFACEMETHODIMP GetFlags(EXPCMDFLAGS* flags) override { *flags = m_.verify ? ECF_DEFAULT : ECF_HASSUBCOMMANDS; return S_OK; }

    // Newer builds of Windows 11 (26300) also put this command in "Show more options", next to the classic handler of the package
    // (ClassicAutoMenu): two LycanZip entries. There is no API that tells the two menus apart, but the order of the calls does:
    // the new menu asks GetState first, the classic menu hands the selection over with SetSelection first (and asks GetState last).
    // So a command whose first call was SetSelection hides itself, and the classic handler is the one entry there. A new menu that
    // started with SetSelection would lose the entry; the tests pin the order (tests/test_shellext.cpp).
    IFACEMETHODIMP GetState(IShellItemArray* items, BOOL, EXPCMDSTATE* state) override {
        stateAsked_ = true;
        *state = ECS_HIDDEN;
        if (inClassicMenu_) return S_OK;
        Build(items);
        if (m_.verify || !m_.nodes.empty()) *state = ECS_ENABLED;
        return S_OK;
    }
    IFACEMETHODIMP SetSelection(IShellItemArray* items) override {
        if (!stateAsked_ && WindowsElevenShell()) inClassicMenu_ = true;   // Windows 10 has no classic handler of a package (desktop9): there this command IS the menu
        selection_ = items;
        Build(items);
        return S_OK;
    }
    IFACEMETHODIMP GetSelection(REFIID riid, void** out) override {
        if (!selection_) { *out = nullptr; return E_FAIL; }
        return selection_->QueryInterface(riid, out);
    }
    IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** e) override {
        if (m_.verify) { *e = nullptr; return E_NOTIMPL; }
        try { return MakeEnum(m_.nodes, e); } catch (...) { *e = nullptr; return E_FAIL; }
    }
    IFACEMETHODIMP Invoke(IShellItemArray* items, IBindCtx*) override {
        try {
            if (!m_.verify) return E_NOTIMPL;
            std::vector<std::wstring> paths;
            if (!GetPaths(items, paths)) return E_FAIL;
            return Launch(L"--verify", paths) ? S_OK : E_FAIL;
        } catch (...) { return E_FAIL; }
    }

private:
    void Build(IShellItemArray* items) {
        try {
            std::vector<std::wstring> paths;
            if (!GetPaths(items, paths)) { m_ = AutoMenu{}; return; }
            m_ = BuildAuto(std::move(paths), WindowsElevenShell());   // the new Windows 11 menu: no sub menus; Windows 10 shows this command in its classic menu, with them
        } catch (...) { m_ = AutoMenu{}; }
    }
    ComPtr<IShellItemArray> selection_;
    AutoMenu m_;
    bool stateAsked_ = false, inClassicMenu_ = false;
};

// The same menu for the classic context menu of a PACKAGED program (the Microsoft Store version): the menu that Windows 11 shows under "Show more options"
// and the only one of a Windows 11 that has the new menu switched off. A package declares it in its manifest (desktop9:FileExplorerClassicContextMenuHandler)
// and Windows asks for an IContextMenu, not an IExplorerCommand: the selection comes in the data object of Initialize, the menu is made of HMENUs and the
// icons are bitmaps.
class ClassicAutoMenu : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IShellExtInit, IContextMenu> {
public:
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* data, HKEY) override {
        try {
            paths_.clear();
            return PathsOfDataObject(data, paths_) ? S_OK : E_INVALIDARG;
        } catch (...) { return E_FAIL; }
    }

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT idFirst, UINT idLast, UINT flags) override {
        try {
            cmds_.clear();
            if ((flags & (CMF_DEFAULTONLY | CMF_NOVERBS)) || paths_.empty() || !menu) return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
            AutoMenu m = BuildAuto(paths_, false);   // the classic menu has sub menus
            UINT next = idFirst;
            if (m.verify) {
                Node n;
                n.title = Tr(L"shell.verify_checksum", LoadConfig());
                n.flags = L"--verify";
                n.icon = ExeIcon(kIconHash);
                std::vector<Node> one;
                one.push_back(std::move(n));
                UINT pos = index;
                if (!AddNodes(menu, pos, one, next, idLast)) return Fail();
            } else {
                if (m.nodes.empty()) return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
                HMENU sub = CreatePopupMenu();
                if (!sub) return E_FAIL;
                UINT pos = 0;
                if (!AddNodes(sub, pos, m.nodes, next, idLast) || next > idLast) { DestroyMenu(sub); return Fail(); }
                MENUITEMINFOW mi{sizeof(mi)};
                mi.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID | MIIM_BITMAP;
                mi.wID = next++;
                cmds_.push_back({});
                mi.hSubMenu = sub;
                mi.hbmpItem = Bitmap(ExePath() + L",0");
                mi.dwTypeData = const_cast<LPWSTR>(L"LycanZip");
                if (!InsertMenuItemW(menu, index, TRUE, &mi)) { DestroyMenu(sub); return Fail(); }
            }
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, next - idFirst);
        } catch (...) { return Fail(); }
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override {
        try {
            if (!info || HIWORD(info->lpVerb) != 0) return E_INVALIDARG;
            const size_t i = LOWORD(info->lpVerb);
            if (i >= cmds_.size() || (cmds_[i].flags.empty() && !cmds_[i].eachOpen)) return E_INVALIDARG;
            if (cmds_[i].eachOpen) {   // several archives: one window each (a single path = open it)
                bool ok = true;
                for (auto& p : paths_) ok = Launch(L"", {p}) && ok;
                return ok ? S_OK : E_FAIL;
            }
            return Launch(cmds_[i].flags, paths_) ? S_OK : E_FAIL;
        } catch (...) { return E_FAIL; }
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override { return E_NOTIMPL; }

    ~ClassicAutoMenu() { for (HBITMAP b : bitmaps_) DeleteObject(b); }

private:
    struct Cmd { std::wstring flags; bool eachOpen = false; };
    HRESULT Fail() { cmds_.clear(); return E_FAIL; }
    HBITMAP Bitmap(const std::wstring& location) {
        HBITMAP b = IconBitmap(location);
        if (b) bitmaps_.push_back(b);   // the menu does not own it: freed with this object
        return b;
    }
    // Adds the nodes to a menu; every entry gets the next command id (the command offset is id - idFirst)
    bool AddNodes(HMENU menu, UINT& pos, const std::vector<Node>& nodes, UINT& next, UINT idLast) {
        for (const Node& n : nodes) {
            if (next > idLast) return false;
            MENUITEMINFOW mi{sizeof(mi)};
            const std::wstring title = MenuText(n.title);
            mi.fMask = MIIM_STRING | MIIM_ID;
            mi.wID = next++;
            mi.dwTypeData = const_cast<LPWSTR>(title.c_str());
            if (!n.icon.empty()) {
                mi.hbmpItem = Bitmap(n.icon);
                if (mi.hbmpItem) mi.fMask |= MIIM_BITMAP;
            }
            if (n.kids.empty()) {
                cmds_.push_back({n.flags, n.eachOpen});
            } else {
                cmds_.push_back({});
                HMENU sub = CreatePopupMenu();
                if (!sub) return false;
                UINT subPos = 0;
                if (!AddNodes(sub, subPos, n.kids, next, idLast)) { DestroyMenu(sub); return false; }
                mi.fMask |= MIIM_SUBMENU;
                mi.hSubMenu = sub;
            }
            if (!InsertMenuItemW(menu, pos, TRUE, &mi)) return false;
            ++pos;
        }
        return true;
    }
    std::vector<std::wstring> paths_;
    std::vector<Cmd> cmds_;   // by command offset
    std::vector<HBITMAP> bitmaps_;
};

}  // namespace

// CLSIDs must match the package manifests of the release (Windows 11 package, Store package) and src/core/shellreg.cpp (classic registration)
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A10")) FilesCommand : public RootCommand<false> {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A11")) ArchiveCommand : public RootCommand<true> {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A20")) ClassicFilesCommand : public RootCommand<false> {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A21")) ClassicArchiveCommand : public RootCommand<true> {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A22")) DragDropCommand : public DragDropMenu {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A12")) VerifyCommandW11 : public VerifyCommand {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A23")) ClassicVerifyCommand : public VerifyCommand {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A24")) PackagedClassicMenu : public ClassicAutoMenu {};
class __declspec(uuid("5E7C1F3A-8B44-4D6E-9A21-3C0F7D2B6A27")) AutoCommand : public RootAuto {};

CoCreatableClass(FilesCommand)
CoCreatableClass(ArchiveCommand)
CoCreatableClass(ClassicFilesCommand)
CoCreatableClass(ClassicArchiveCommand)
CoCreatableClass(DragDropCommand)
CoCreatableClass(VerifyCommandW11)
CoCreatableClass(ClassicVerifyCommand)
CoCreatableClass(PackagedClassicMenu)
CoCreatableClass(AutoCommand)
CoCreatableClassWrlCreatorMapInclude(FilesCommand)
CoCreatableClassWrlCreatorMapInclude(ArchiveCommand)
CoCreatableClassWrlCreatorMapInclude(ClassicFilesCommand)
CoCreatableClassWrlCreatorMapInclude(ClassicArchiveCommand)
CoCreatableClassWrlCreatorMapInclude(DragDropCommand)
CoCreatableClassWrlCreatorMapInclude(VerifyCommandW11)
CoCreatableClassWrlCreatorMapInclude(ClassicVerifyCommand)
CoCreatableClassWrlCreatorMapInclude(PackagedClassicMenu)
CoCreatableClassWrlCreatorMapInclude(AutoCommand)

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** out) {
    return Module<InProc>::GetModule().GetClassObject(clsid, riid, out);
}

STDAPI DllCanUnloadNow() {
    return Module<InProc>::GetModule().GetObjectCount() == 0 ? S_OK : S_FALSE;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
