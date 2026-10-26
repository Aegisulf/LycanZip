#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>

#include "lycan/datalocation.h"
#include "lycan/hash.h"
#include "lycan/exectype.h"
#include "lycan/settings.h"
#include "lycan/shellitems.h"

using namespace lycan;
namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int main() {
    fs::path dir = fs::temp_directory_path() / (L"lycan_settings_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);

    // does not exist -> defaults
    EXPECT(LoadSettings(dir / "nope.ini") == Settings{});

    // round trip
    Settings s;
    s.theme = ThemeMode::Dark;
    s.conflict = ConflictPolicy::Rename;
    s.extractMode = ExtractMode::Folder;
    s.closeCompactWhenDone = false;
    s.taskPriority = TaskPriority::Background;
    s.revealAfterCompress = true;
    s.openFolderAfterExtract = true;
    s.warnExecutable = false;
    s.tempDir = L"D:\\Scratch\\中文";
    s.execScope = OpenScope::FileOnly;
    s.testAfterCompress = true;
    s.sumsArchive = true;
    s.sumsFiles = true;
    s.audioLoop = AudioLoop::Shuffle;
    s.openPreviewKey = PreviewKey::Ctrl;
    s.defaultFormat = ArchiveType::SevenZip;
    s.defaultProfile = Profile::Smallest;
    s.parallelTasks = 3;
    s.accent = AccentTheme::Night;   // the newest one (it sits before "follow the system": the names in the file, not the numbers, are what is saved)
    s.background = BackgroundStyle::Gradient;
    s.viewMode = ViewMode::Details;
    s.openIn = OpenPolicy::Replace;
    s.browseFlat = true;
    s.compactRows = false;
    s.showFolderTree = false;
    s.animations = false;
    s.lastPreset = L"我的備份 \u6A21\u677F";
    s.compressAdvancedOpen = true;
    s.doubleClickPreview = false;
    s.previewEnlargeSmall = true;
    s.rememberViewMode = false;
    s.listColumns = 5;
    s.treeWidth = 260;
    s.listColWidths = "name=320,type=70";
    s.iconThumbnails = false;
    s.doubleClickAudioPreview = true;
    s.bombCheck = false;
    s.welcomeDone = true;
    s.twoCopiesNoticeShown = true;
    s.shellMenu = false;
    s.thumbSize = 2;
    s.prefetchMB = 1024;
    s.soundEffects = true;
    s.addMode = AddMode::Synchronize;
    s.existingOutput = ExistingOutput::Update;
    s.soundSet = SoundSet::Wave;
    s.notifications = false;
    s.language = L"en-US";
    s.skipMacMetadata = false;
    s.skipThumbsDb = false;
    s.hideMacMetadata = false;
    s.dragOutVirtual = false;
    s.confirmDeleteArchive = false;
    s.confirmDeleteEntries = false;
    s.keepBackupOnModify = true;
    s.confirmWriteBack = false;
    s.editorPath = L"C:\\Program Files\\Notepad++\\notepad++.exe";
    s.shellItems = ShellItemsWith(ShellItemsWith("", "audio", true), "test", false);
    s.shellFormat1 = ArchiveType::TarGz;
    s.shellFormat2 = ArchiveType::Zip;
    s.recentExtract = {L"D:\\Projects\\one", L"E:\\中文 資料夾"};
    // Defaults: one level browsing, compact row height, show the folder tree
    EXPECT(Settings{}.accent == AccentTheme::Moonlight && Settings{}.background == BackgroundStyle::Full && Settings{}.viewMode == ViewMode::Icons && Settings{}.openIn == OpenPolicy::NewWindow && !Settings{}.browseFlat && Settings{}.compactRows && Settings{}.showFolderTree);
    EXPECT(Settings{}.doubleClickPreview && Settings{}.prefetchMB == DefaultPrefetchMB());  // defaults: double-click opens the preview; prefetch depends on RAM
    EXPECT(DefaultPrefetchMB(4ull << 30) == 128 && DefaultPrefetchMB(6ull << 30) == 128 && DefaultPrefetchMB(7900ull << 20) == 256 && DefaultPrefetchMB(32ull << 30) == 256 && DefaultPrefetchMB(0) == 256);
    EXPECT(Settings{}.listColumns == 11 && Settings{}.treeWidth == 200 && Settings{}.rememberViewMode && !Settings{}.previewEnlargeSmall && Settings{}.iconThumbnails && Settings{}.thumbSize == 1);  // thumbnails on by default, medium (2026-10-13)
    EXPECT(!Settings{}.doubleClickAudioPreview && !Settings{}.welcomeDone && Settings{}.bombCheck);  // preview: small pictures are not enlarged by default
    EXPECT(Settings{}.warnExecutable && Settings{}.execScope == OpenScope::Folder);  // executables: warn, extract the folder
    EXPECT(IsExecutableName(L"Setup.EXE") && IsExecutableName(L"a/b/run.bat") && IsExecutableName(L"x.ps1") && IsExecutableName(L"link.lnk") &&
           !IsExecutableName(L"notes.txt") && !IsExecutableName(L"exe") && !IsExecutableName(L"archive.exe.txt") && !IsExecutableName(L"trailing."));
    EXPECT(Settings{}.skipMacMetadata && Settings{}.skipThumbsDb && Settings{}.hideMacMetadata && Settings{}.dragOutVirtual && Settings{}.confirmDeleteArchive);  // clutter is left out by default
    EXPECT(Settings{}.confirmDeleteEntries && !Settings{}.keepBackupOnModify && Settings{}.confirmWriteBack && Settings{}.editorPath.empty());  // deleting from an archive asks; no backup by default
    EXPECT(Settings{}.tempDir.empty());  // temporary files: the system temp folder by default
    EXPECT(Settings{}.language == L"auto");  // the language follows the system by default
    EXPECT(Settings{}.addMode == AddMode::Replace && Settings{}.existingOutput == ExistingOutput::Rename);   // the old behaviour is the default
    EXPECT(Settings{}.shellIcons);   // the lists show the icons of Windows by default
    EXPECT(Settings{}.keepDamaged);  // an item with a data error is kept (and reported) by default
    EXPECT(Settings{}.soundSet == SoundSet::Synth);  // the Synth sounds are the default
    EXPECT(Settings{}.animations && !Settings{}.soundEffects && Settings{}.notifications);  // animations on by default, sound off, notifications on
    EXPECT(SaveSettings(dir / "sub" / "a.ini", s));  // the parent folder is made automatically
    EXPECT(LoadSettings(dir / "sub" / "a.ini") == s);
    EXPECT(!fs::exists(dir / "sub" / "a.ini.tmp"));

    {   // the name of a built-in preset that the first versions wrote (Chinese) is read as the current name
        std::ofstream f(dir / "legacy.ini", std::ios::binary);
        f << "last_preset=\xE5\x82\x99\xE4\xBB\xBD\r\n";
    }
    EXPECT(LoadSettings(dir / "legacy.ini").lastPreset == L"Backup");

    // Damaged / unknown / out of range values: go back to the default, the rest loads as usual
    {
        std::ofstream f(dir / "bad.ini", std::ios::binary);
        f << "\xEF\xBB\xBF" "# comment\r\ntheme=purple\r\nconflict = skip \r\nunknown_key=1\r\nparallel_tasks=99\r\n"
             "language=../evil\r\ndefault_format=rar\r\nextract_mode=maybe\r\n garbage line \r\ntest_after_compress=yes\r\n";
    }
    Settings b = LoadSettings(dir / "bad.ini");
    EXPECT(b.theme == ThemeMode::System);          // invalid
    EXPECT(b.welcomeDone);   // a file without the key is from before the welcome window: counts as shown
    EXPECT(!LoadSettings(fs::path(L"no_such_dir") / L"none.ini").welcomeDone);   // no file at all: the welcome window is due
    EXPECT(b.conflict == ConflictPolicy::Skip);    // valid, with spaces
    EXPECT(b.parallelTasks == 1);                  // out of range
    EXPECT(b.language == L"auto");                 // not like a language code
    EXPECT(b.defaultFormat == ArchiveType::Zip);   // invalid
    EXPECT(b.extractMode == ExtractMode::Auto);
    EXPECT(!Settings{}.closeCompactWhenDone && !Settings{}.revealAfterCompress && Settings{}.taskPriority == TaskPriority::Normal);                // invalid -> the default stays
    EXPECT(b.testAfterCompress == true);
    EXPECT(b.audioLoop == AudioLoop::None && b.accent == AccentTheme::Moonlight && b.openPreviewKey == PreviewKey::Shift);   // missing / unknown: the defaults
    {
        std::ofstream f(dir / "night.ini", std::ios::binary);
        f << "accent=night\naudio_loop=all\nopen_preview_key=off\n";
        f.close();
        const Settings n = LoadSettings(dir / "night.ini");
        EXPECT(n.accent == AccentTheme::Night && n.audioLoop == AudioLoop::All && n.openPreviewKey == PreviewKey::Off);
        EXPECT(AccentTheme::Night < AccentTheme::System);   // follow-the-system stays the last one (the settings page counts on it)
    }
    EXPECT(!Settings{}.sumsArchive && !Settings{}.sumsFiles);   // the checksum files are off until asked for

    // Context-menu items (shellitems.h)
    {
        EXPECT(Settings{}.shellItems.empty() && Settings{}.shellFormat1 == ArchiveType::Zip && Settings{}.shellFormat2 == ArchiveType::SevenZip);
        // Defaults: compress / extract / test / image preview / open content on; audio preview, encryption, other formats off.
        EXPECT(ShellItemEnabled("", "add") && ShellItemEnabled("", "fmt1") && ShellItemEnabled("", "images") && ShellItemEnabled("", "opencontent"));
        EXPECT(!ShellItemEnabled("", "audio") && !ShellItemEnabled("", "more") && !ShellItemEnabled("", "encrypt") && !ShellItemEnabled("", "merge"));
        EXPECT(!ShellItemEnabled("", "nonsense") && !ShellItemEnabled("add", "nonsense"));
        EXPECT(ShellItemEnabled("add,test", "test") && !ShellItemEnabled("add,test", "tes") && !ShellItemEnabled("add,test", "here"));  // exact id match
        EXPECT(!ShellItemEnabled("-", "add"));
        // Toggling: switching an item to its default gives back "" (follows future default changes).
        EXPECT(ShellItemsWith("", "add", true).empty());
        const std::string noAdd = ShellItemsWith("", "add", false);
        EXPECT(!noAdd.empty() && !ShellItemEnabled(noAdd, "add") && ShellItemEnabled(noAdd, "fmt1"));
        EXPECT(ShellItemsWith(noAdd, "add", true).empty());
        EXPECT(ShellItemEnabled(ShellItemsWith("", "audio", true), "audio") && ShellItemEnabled(ShellItemsWith("", "audio", true), "open"));
        // Everything off = "-"; everything off then one on keeps only that one.
        std::string none = "";
        for (const auto& it : kShellItems) none = ShellItemsWith(none, it.id, false);
        EXPECT(none == "-");
        EXPECT(ShellItemsWith(none, "here", true) == "here");
        // Normalising: unknown ids dropped, table order, only-unknown = none.
        EXPECT(NormalizeShellItems("test,bogus,add") == "add,test");
        EXPECT(NormalizeShellItems("bogus") == "-");
        EXPECT(NormalizeShellItems("").empty());
        // The ids are unique.
        for (size_t i = 0; i < kShellItemCount; ++i)
            for (size_t j = i + 1; j < kShellItemCount; ++j) EXPECT(std::string(kShellItems[i].id) != kShellItems[j].id);
        // The folder name an archive gives (shared by extraction and the menu titles)
        EXPECT(ArchiveBaseNameOf(L"foo.tar.gz") == L"foo" && ArchiveBaseNameOf(L"foo.TGZ") == L"foo" && ArchiveBaseNameOf(L"foo.7z.001") == L"foo" &&
               ArchiveBaseNameOf(L"foo.part1.rar") == L"foo" && ArchiveBaseNameOf(L"foo.part01.rar") == L"foo" && ArchiveBaseNameOf(L"a.b.zip") == L"a.b" &&
               ArchiveBaseNameOf(L"archive.zip") == L"archive" && ArchiveBaseNameOf(L"noext") == L"noext" && ArchiveBaseNameOf(L".zip") == L".zip" &&
               ArchiveBaseNameOf(L"x.tar") == L"x" && ArchiveBaseNameOf(L"x.tar.zst") == L"x");
        // Recently used extraction folders: newest first, no duplicates (case-insensitive), at most 5
        {
            std::vector<std::wstring> r;
            for (const wchar_t* d : {L"C:\\a", L"C:\\b", L"C:\\c", L"C:\\d", L"C:\\e", L"C:\\f"}) RememberExtractDir(r, d);
            EXPECT(r.size() == kMaxRecentExtract && r.front() == L"C:\\f" && r.back() == L"C:\\b");
            RememberExtractDir(r, L"c:\\C");
            EXPECT(r.size() == kMaxRecentExtract && r.front() == L"c:\\C" && r[1] == L"C:\\f" && r.back() == L"C:\\b");
            RememberExtractDir(r, L"");
            EXPECT(r.size() == kMaxRecentExtract);
        }
        // File round trip and bad values.
        {
            std::ofstream f(dir / "shell.ini", std::ios::binary);
            f << "shell_items=test,bogus,add\r\nshell_format_1=tar.xz\r\nshell_format_2=rar\r\n";
        }
        Settings sh = LoadSettings(dir / "shell.ini");
        EXPECT(sh.shellItems == "add,test" && sh.shellFormat1 == ArchiveType::TarXz && sh.shellFormat2 == ArchiveType::SevenZip);
    }

    // Switching between the program folder and the user's folder (datalocation.h): copy, flag, .bak, a folder that cannot be written
    {
        const fs::path exeDir = dir / "dl_exe", userDir = dir / "dl_user";
        fs::create_directories(exeDir);
        fs::create_directories(userDir);
        auto write = [](const fs::path& p, const std::string& t) { std::ofstream(p, std::ios::binary | std::ios::trunc) << t; };
        auto read = [](const fs::path& p) { std::ifstream f(p, std::ios::binary); return std::string(std::istreambuf_iterator<char>(f), {}); };
        write(userDir / "settings.ini", "a=1\n");
        write(userDir / "presets.ini", "[x]\n");
        EXPECT(SwitchDataLocation(exeDir, userDir, false) == SwitchStatus::Ok && !fs::exists(exeDir / "LycanZip.portable"));  // already that mode: nothing happens
        EXPECT(!fs::exists(exeDir / "settings.ini"));
        // to the program folder: copied, flag made, the old ones stay
        EXPECT(SwitchDataLocation(exeDir, userDir, true) == SwitchStatus::Ok);
        EXPECT(fs::exists(exeDir / "LycanZip.portable") && read(exeDir / "settings.ini") == "a=1\n" && read(exeDir / "presets.ini") == "[x]\n");
        EXPECT(read(userDir / "settings.ini") == "a=1\n");
        EXPECT(SwitchDataLocation(exeDir, userDir, true) == SwitchStatus::Ok);  // again: nothing
        // changed there, then back: the copy wins, what was in the user's folder is kept as .bak, the flag is gone
        write(exeDir / "settings.ini", "a=2\n");
        EXPECT(SwitchDataLocation(exeDir, userDir, false) == SwitchStatus::Ok);
        EXPECT(!fs::exists(exeDir / "LycanZip.portable") && read(userDir / "settings.ini") == "a=2\n" && read(userDir / "settings.ini.bak") == "a=1\n");
        EXPECT(read(exeDir / "settings.ini") == "a=2\n");  // the program folder keeps its files too
        // no presets.ini at the source: fine
        fs::remove(userDir / "presets.ini");
        fs::remove(exeDir / "presets.ini");
        EXPECT(SwitchDataLocation(exeDir, userDir, true) == SwitchStatus::Ok && fs::exists(exeDir / "LycanZip.portable"));
        EXPECT(SwitchDataLocation(exeDir, userDir, false) == SwitchStatus::Ok);
        // a target that cannot be written (a file where the folder should be): nothing changes
        write(dir / "dl_blocker", "x");
        EXPECT(SwitchDataLocation(dir / "dl_blocker" / "sub", userDir, true) == SwitchStatus::NotWritable);
        EXPECT(!fs::exists(dir / "dl_blocker" / "sub" / "LycanZip.portable"));
        EXPECT(DirWritable(exeDir) && !DirWritable(dir / "dl_blocker" / "sub"));
        const DataLocation now = GetDataLocation();
        EXPECT(!now.exeDir.empty() && !now.userDir.empty() && now.userDir.filename() == L"LycanZip" && now.dataDir == (now.portable ? now.exeDir : now.userDir));
        EXPECT(!now.packaged && now.userDir == UserDataDir());

        // a package (the Store version) has its own folder; the first time, what it saw in %LOCALAPPDATA%\LycanZip is copied there once
        const fs::path local = dir / "dl_local", pkg = local / "Packages" / "Aegisulf.LycanZip_9f2w5eb76dr46";
        EXPECT(UserDataDirFor(local, {}) == local / "LycanZip" && UserDataDirFor(local, pkg) == pkg / "LocalState");
        fs::create_directories(local / "LycanZip");
        write(local / "LycanZip" / "settings.ini", "theme=dark\n");
        write(local / "LycanZip" / "presets.ini", "[mine]\n");
        const fs::path state = UserDataDirFor(local, pkg);
        EXPECT(ImportIntoPackageData(local / "LycanZip", state));
        EXPECT(read(state / "settings.ini") == "theme=dark\n" && read(state / "presets.ini") == "[mine]\n" && fs::exists(state / "imported"));
        write(local / "LycanZip" / "settings.ini", "theme=light\n");   // later changes of the other copy are not taken over
        fs::remove(state / "settings.ini");                               // also not when the package's own file is gone (reset)
        EXPECT(!ImportIntoPackageData(local / "LycanZip", state) && !fs::exists(state / "settings.ini"));
        const fs::path state2 = UserDataDirFor(local, local / "Packages" / "other");   // nothing to copy: only the marker
        EXPECT(!ImportIntoPackageData(dir / "dl_nothing", state2) && fs::exists(state2 / "imported") && !fs::exists(state2 / "settings.ini"));
        const fs::path state3 = UserDataDirFor(local, local / "Packages" / "third");   // the package has settings already: not replaced
        fs::create_directories(state3);
        write(state3 / "settings.ini", "theme=system\n");
        EXPECT(!ImportIntoPackageData(local / "LycanZip", state3) && read(state3 / "settings.ini") == "theme=system\n");

        // is the Store version installed (the test override; the real query must not fail)
        SetEnvironmentVariableW(L"LYCAN_TEST_STORE_INSTALLED", L"1");
        EXPECT(IsStorePackageInstalled());
        SetEnvironmentVariableW(L"LYCAN_TEST_STORE_INSTALLED", L"0");
        EXPECT(!IsStorePackageInstalled());
        SetEnvironmentVariableW(L"LYCAN_TEST_STORE_INSTALLED", nullptr);
        (void)IsStorePackageInstalled();
    }

    // The file is grouped in [sections]; the loader does not care about them (a file without sections loads as well)
    {
        Settings g;
        g.theme = ThemeMode::ContrastBlack;   // the program's own high contrast look
        g.debugLog = true;
        g.hashAlgos = kHashSha512;
        g.lastPreset = L"我的範本";
        g.ignoredMenuPath = L"D:\\Old\\LycanZip.exe";
        g.recentExtract = {L"D:\\Out"};
        EXPECT(SaveSettings(dir / "sections.ini", g));
        std::vector<std::string> sections;
        std::map<std::string, std::string> where;  // key -> section
        std::string first, line, cur;
        {
            std::ifstream in(dir / "sections.ini", std::ios::binary);
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                if (line[0] == '[') { cur = line.substr(1, line.find(']') - 1); sections.push_back(cur); continue; }
                if (first.empty()) first = line;
                const auto eq = line.find('=');
                EXPECT(eq != std::string::npos);
                const std::string key = line.substr(0, eq);
                EXPECT(where.find(key) == where.end());   // a key is written once
                where[key] = cur;
            }
        }
        EXPECT(first == "settings_format=1");
        EXPECT(sections == (std::vector<std::string>{"appearance", "view", "preview", "extract", "compress", "modify", "safety", "tasks", "explorer", "hash", "diagnostics", "updates", "state"}));
        EXPECT(where.size() == 73);  // 72 settings and the format line
        EXPECT(where["two_copies_notice_shown"] == "state" && where["shell_menu"] == "explorer");
        EXPECT(where["theme"] == "appearance" && where["update_mode"] == "state" && where["welcome_done"] == "state" && where["bomb_check"] == "extract" && where["dblclick_audio_preview"] == "preview" && where["existing_output"] == "state" && where["shell_icons"] == "view" && where["keep_damaged"] == "extract" && where["sound_set"] == "appearance" && where["view_mode"] == "view" && where["image_prefetch_mb"] == "preview" && where["conflict"] == "extract" &&
               where["default_format"] == "compress" && where["audio_loop"] == "preview" && where["open_preview_key"] == "preview" && where["lock_open_archive"] == "safety" && where["sums_archive"] == "compress" && where["sums_files"] == "compress" && where["editor_path"] == "modify" && where["warn_executable"] == "safety" && where["parallel_tasks"] == "tasks" &&
               where["shell_items"] == "explorer" && where["hash_algos"] == "hash" && where["debug_log"] == "diagnostics" && where["last_preset"] == "state" && where["stale_menu_ignored"] == "state" &&
               where["settings_format"].empty());
        const Settings back = LoadSettings(dir / "sections.ini");
        EXPECT(back.theme == ThemeMode::ContrastBlack && back.debugLog && back.hashAlgos == kHashSha512 && back.lastPreset == L"我的範本" && back.ignoredMenuPath == L"D:\\Old\\LycanZip.exe" && back.recentExtract.size() == 1);
        // sections in any order, an unknown section and key, comments after a section name: all fine
        {
            std::ofstream f(dir / "mixed.ini", std::ios::binary);
            f << "[state]  # remembered\r\ntree_width=222\r\n[from_the_future]\r\nsomething=1\r\n[appearance]\r\naccent=forest\r\n";
        }
        const Settings m = LoadSettings(dir / "mixed.ini");
        EXPECT(m.treeWidth == 222 && m.accent == AccentTheme::Forest);
    }

    // The algorithms of the hash dialog (hash.h): a mask, never empty, nothing unknown
    {
        EXPECT(Settings{}.hashAlgos == kHashDefault);
        for (auto [text, want] : std::vector<std::pair<const char*, uint32_t>>{{"2", 2}, {"36", 36}, {"63", 63}, {"0", kHashDefault}, {"64", kHashDefault}, {"abc", kHashDefault}, {"-3", kHashDefault}, {"66", 2}}) {
            std::ofstream f(dir / "hash.ini", std::ios::binary | std::ios::trunc);
            f << "hash_algos=" << text << "\r\n";
            f.close();
            EXPECT(LoadSettings(dir / "hash.ini").hashAlgos == want);
        }
        Settings hs;
        hs.hashAlgos = kHashSha1 | kHashSha512;
        SaveSettings(dir / "hash2.ini", hs);
        EXPECT(LoadSettings(dir / "hash2.ini").hashAlgos == (kHashSha1 | kHashSha512));
    }

    // Built-in presets in the context menu (shellitems.h): by default smallest, backup and store only
    {
        EXPECT(Settings{}.shellPresets.empty());
        for (size_t i = 0; i < kBuiltinPresetCount; ++i) EXPECT(BuiltinPresetEnabled("", i) == kBuiltinPresetDefaultOn[i]);
        EXPECT(!BuiltinPresetEnabled("", 0) && !BuiltinPresetEnabled("", 1) && BuiltinPresetEnabled("", 2) && BuiltinPresetEnabled("", 5) && BuiltinPresetEnabled("", 6));
        EXPECT(!BuiltinPresetEnabled("-", 2) && !BuiltinPresetEnabled("", 99));
        std::string v = BuiltinPresetsWith("", 0, true);   // zip on: not the defaults any more
        EXPECT(v == "zip,smallest,backup,store" && BuiltinPresetEnabled(v, 0) && BuiltinPresetEnabled(v, 2));
        EXPECT(BuiltinPresetsWith(v, 0, false).empty());     // back to the defaults: the value is empty again
        std::string none = "";
        for (size_t i = 0; i < kBuiltinPresetCount; ++i) none = BuiltinPresetsWith(none, i, false);
        EXPECT(none == "-");
        EXPECT(NormalizeBuiltinPresets("store,bogus,zip") == "zip,store");
        {
            std::ofstream f(dir / "shellp.ini", std::ios::binary);
            f << "shell_presets=fast,bogus,7z\r\n";
        }
        Settings sp = LoadSettings(dir / "shellp.ini");
        EXPECT(sp.shellPresets == "7z,fast");
        sp.shellPresets = BuiltinPresetsWith(sp.shellPresets, 3, false);
        SaveSettings(dir / "shellp2.ini", sp);
        EXPECT(LoadSettings(dir / "shellp2.ini").shellPresets == "7z");
    }

    // Conversion
    EXPECT(ToOverwriteMode(ConflictPolicy::Ask) == OverwriteMode::Ask);
    EXPECT(ToOverwriteMode(ConflictPolicy::Rename) == OverwriteMode::Rename);

    // Default path
    EXPECT(!DefaultSettingsPath().empty() && DefaultSettingsPath().filename() == L"settings.ini");

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures == 0) std::puts("test_settings: all passed");
    return failures ? 1 : 0;
}
