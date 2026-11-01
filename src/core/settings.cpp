#include <algorithm>
#include "lycan/hash.h"
#include "lycan/settings.h"

#include <windows.h>
#include <shlobj.h>

#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

#include "lycan/datalocation.h"
#include "lycan/error.h"
#include "lycan/shellitems.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

template <typename E>
struct Name { E value; const char* text; };

constexpr Name<ThemeMode> kThemes[] = {{ThemeMode::System, "system"}, {ThemeMode::Light, "light"}, {ThemeMode::Dark, "dark"}, {ThemeMode::ContrastBlack, "contrast-black"}};
constexpr Name<AccentTheme> kAccents[] = {{AccentTheme::Moonlight, "moonlight"}, {AccentTheme::BloodMoon, "blood_moon"},
                                          {AccentTheme::Amber, "amber"}, {AccentTheme::Aurora, "aurora"},
                                          {AccentTheme::Forest, "forest"}, {AccentTheme::Night, "night"}, {AccentTheme::System, "system"}};
constexpr Name<PreviewKey> kPreviewKeys[] = {{PreviewKey::Shift, "shift"}, {PreviewKey::Ctrl, "ctrl"}, {PreviewKey::Off, "off"}};
constexpr Name<AudioLoop> kAudioLoops[] = {{AudioLoop::None, "none"}, {AudioLoop::All, "all"}, {AudioLoop::One, "one"}, {AudioLoop::Shuffle, "shuffle"}};
constexpr Name<BackgroundStyle> kBackgrounds[] = {{BackgroundStyle::Full, "full"}, {BackgroundStyle::Gradient, "gradient"}, {BackgroundStyle::Off, "off"}};
constexpr Name<SoundSet> kSoundSets[] = {{SoundSet::Synth, "synth"}, {SoundSet::Wave, "wave"}};
constexpr Name<AddMode> kAddModes[] = {{AddMode::Replace, "replace"}, {AddMode::UpdateAdd, "update"}, {AddMode::Freshen, "freshen"}, {AddMode::Synchronize, "sync"}};
constexpr Name<ExistingOutput> kExistingOutputs[] = {{ExistingOutput::Rename, "rename"}, {ExistingOutput::Overwrite, "overwrite"}, {ExistingOutput::Update, "update"}};
constexpr Name<ConflictPolicy> kConflicts[] = {{ConflictPolicy::Ask, "ask"}, {ConflictPolicy::Skip, "skip"},
                                               {ConflictPolicy::Overwrite, "overwrite"}, {ConflictPolicy::Rename, "rename"}};
constexpr Name<ViewMode> kViewModes[] = {{ViewMode::Details, "details"}, {ViewMode::List, "list"}, {ViewMode::Icons, "icons"}};
constexpr Name<ExtractMode> kExtractModes[] = {{ExtractMode::Auto, "auto"}, {ExtractMode::NoFolder, "no_folder"}, {ExtractMode::Folder, "folder"}};
constexpr Name<OpenScope> kOpenScopes[] = {{OpenScope::Folder, "folder"}, {OpenScope::FileOnly, "file_only"}};
constexpr Name<TaskPriority> kTaskPriorities[] = {{TaskPriority::Normal, "normal"}, {TaskPriority::BelowNormal, "below_normal"}, {TaskPriority::Background, "background"}};
constexpr Name<OpenPolicy> kOpenPolicies[] = {{OpenPolicy::NewWindow, "new"}, {OpenPolicy::Replace, "replace"}};
constexpr Name<ArchiveType> kFormats[] = {{ArchiveType::Zip, "zip"},       {ArchiveType::SevenZip, "7z"},
                                          {ArchiveType::TarGz, "tar.gz"},  {ArchiveType::TarXz, "tar.xz"},
                                          {ArchiveType::TarBz2, "tar.bz2"}, {ArchiveType::Tar, "tar"}, {ArchiveType::Iso, "iso"}};
constexpr Name<Profile> kProfiles[] = {{Profile::Balanced, "balanced"}, {Profile::Compatible, "compatible"},
                                       {Profile::Smallest, "smallest"}, {Profile::Fastest, "fastest"}};

template <typename E, size_t N>
const char* ToText(const Name<E> (&table)[N], E v) {
    for (auto& n : table) if (n.value == v) return n.text;
    return table[0].text;
}
template <typename E, size_t N>
E FromText(const Name<E> (&table)[N], const std::string& s, E dflt) {
    for (auto& n : table) if (s == n.text) return n.value;
    return dflt;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
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

bool ToBool(const std::string& s, bool dflt) {
    if (s == "true" || s == "1" || s == "yes") return true;
    if (s == "false" || s == "0" || s == "no") return false;
    return dflt;
}

std::string JoinDirs(const std::vector<std::wstring>& dirs) {
    std::string out;
    for (auto& d : dirs) {
        if (d.find(L'|') != std::wstring::npos) continue;  // the separator cannot be part of a stored path
        if (!out.empty()) out += '|';
        out += ToUtf8(d);
    }
    return out;
}

}  // namespace

void RememberExtractDir(std::vector<std::wstring>& list, const std::wstring& dir) {
    if (dir.empty()) return;
    for (auto it = list.begin(); it != list.end();)
        if (_wcsicmp(it->c_str(), dir.c_str()) == 0) it = list.erase(it);
        else ++it;
    list.insert(list.begin(), dir);
    if (list.size() > kMaxRecentExtract) list.resize(kMaxRecentExtract);
}

fs::path DefaultSettingsPath() {
    wchar_t over[MAX_PATH * 2];  // tests: LYCAN_SETTINGS_DIR puts the settings (and the presets) in a folder of their own, whatever the machine has
    if (const DWORD o = GetEnvironmentVariableW(L"LYCAN_SETTINGS_DIR", over, static_cast<DWORD>(std::size(over))); o && o < std::size(over))
        return fs::path(std::wstring(over, o)) / L"settings.ini";
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    fs::path dir = fs::path(std::wstring(exe, n)).parent_path();
    std::error_code ec;
    if (fs::exists(dir / L"LycanZip.portable", ec)) return dir / L"settings.ini";
    return UserDataDir() / L"settings.ini";   // %LOCALAPPDATA%\LycanZip, or the LocalState of the Store package (datalocation.h)
}

unsigned DefaultPrefetchMB(uint64_t totalPhysBytes) {
    return totalPhysBytes != 0 && totalPhysBytes < (6ull << 30) + (512ull << 20) ? 128u : 256u;  // 4 / 6 GB machines report ~3.9 / ~5.9 GB; 8 GB ones ~7+ GB
}

unsigned DefaultPrefetchMB() {
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    return DefaultPrefetchMB(GlobalMemoryStatusEx(&m) ? m.ullTotalPhys : 0);
}

Settings LoadSettings(const fs::path& file) {
    Settings s;
    std::ifstream in(file, std::ios::binary);
    if (!in) return s;
    std::string line;
    bool sawWelcome = false;
    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        if (k == "welcome_done") { s.welcomeDone = ToBool(v, false); sawWelcome = true; }
        else if (k == "open_preview_key") s.openPreviewKey = FromText(kPreviewKeys, v, s.openPreviewKey);
        else if (k == "audio_loop") s.audioLoop = FromText(kAudioLoops, v, s.audioLoop);
        else if (k == "dblclick_audio_preview") s.doubleClickAudioPreview = ToBool(v, s.doubleClickAudioPreview);
        else if (k == "theme") s.theme = FromText(kThemes, v, s.theme);
        else if (k == "accent") s.accent = FromText(kAccents, v, s.accent);
        else if (k == "background") s.background = FromText(kBackgrounds, v, s.background);
        else if (k == "conflict") s.conflict = FromText(kConflicts, v, s.conflict);
        else if (k == "extract_mode") s.extractMode = FromText(kExtractModes, v, s.extractMode);
        else if (k == "open_folder_after_extract") s.openFolderAfterExtract = ToBool(v, s.openFolderAfterExtract);
        else if (k == "lock_open_archive") s.lockOpenArchive = ToBool(v, s.lockOpenArchive);
        else if (k == "warn_executable") s.warnExecutable = ToBool(v, s.warnExecutable);
        else if (k == "executable_scope") s.execScope = FromText(kOpenScopes, v, s.execScope);
        else if (k == "keep_damaged") s.keepDamaged = ToBool(v, s.keepDamaged);
        else if (k == "bomb_check") s.bombCheck = ToBool(v, s.bombCheck);
        else if (k == "skip_mac_metadata") s.skipMacMetadata = ToBool(v, s.skipMacMetadata);
        else if (k == "skip_thumbs_db") s.skipThumbsDb = ToBool(v, s.skipThumbsDb);
        else if (k == "confirm_delete_archive") s.confirmDeleteArchive = ToBool(v, s.confirmDeleteArchive);
        else if (k == "confirm_delete_entries") s.confirmDeleteEntries = ToBool(v, s.confirmDeleteEntries);
        else if (k == "modify_keep_backup") s.keepBackupOnModify = ToBool(v, s.keepBackupOnModify);
        else if (k == "confirm_write_back") s.confirmWriteBack = ToBool(v, s.confirmWriteBack);
        else if (k == "editor_path") {
            const std::wstring w = FromUtf8(v);
            bool ok = w.size() <= 260;
            for (wchar_t c : w) if (c < 0x20 || c == L'"') ok = false;
            s.editorPath = ok ? w : std::wstring();
        }
        else if (k == "drag_out_virtual") s.dragOutVirtual = ToBool(v, s.dragOutVirtual);
        else if (k == "hide_mac_metadata") s.hideMacMetadata = ToBool(v, s.hideMacMetadata);
        else if (k == "test_after_compress") s.testAfterCompress = ToBool(v, s.testAfterCompress);
        else if (k == "sums_archive") s.sumsArchive = ToBool(v, s.sumsArchive);
        else if (k == "sums_files") s.sumsFiles = ToBool(v, s.sumsFiles);
        else if (k == "default_format") s.defaultFormat = FromText(kFormats, v, s.defaultFormat);
        else if (k == "default_profile") s.defaultProfile = FromText(kProfiles, v, s.defaultProfile);
        else if (k == "open_archive_in") s.openIn = FromText(kOpenPolicies, v, s.openIn);
        else if (k == "view_mode") s.viewMode = FromText(kViewModes, v, s.viewMode);
        else if (k == "browse_flat") s.browseFlat = ToBool(v, s.browseFlat);
        else if (k == "shell_icons") s.shellIcons = ToBool(v, s.shellIcons);
        else if (k == "compact_rows") s.compactRows = ToBool(v, s.compactRows);
        else if (k == "show_folder_tree") s.showFolderTree = ToBool(v, s.showFolderTree);
        else if (k == "preview_pane") s.previewPane = ToBool(v, s.previewPane);
        else if (k == "preview_pane_height") {
            try {
                const long n = std::stol(v);
                if (n >= 120 && n <= 1200) s.previewPaneHeight = static_cast<unsigned>(n);
            } catch (...) {}
        }
        else if (k == "preview_window_rect") {
            // four numbers separated by commas (x and y may be negative on a monitor left of / above the main one)
            bool ok = v.size() <= 60 && std::count(v.begin(), v.end(), ',') == 3;
            for (char c : v) if (!std::isdigit(static_cast<unsigned char>(c)) && c != ',' && c != '-') ok = false;
            if (ok) s.previewWindowRect = v;
        }
        else if (k == "stale_menu_ignored") { auto w = FromUtf8(v); if (w.size() <= 520) s.ignoredMenuPath = w; }
        else if (k == "two_copies_notice_shown") s.twoCopiesNoticeShown = ToBool(v, s.twoCopiesNoticeShown);
        else if (k == "update_mode") s.addMode = FromText(kAddModes, v, s.addMode);
        else if (k == "existing_output") s.existingOutput = FromText(kExistingOutputs, v, s.existingOutput);
        else if (k == "last_preset") { auto w = FromUtf8(v); if (!w.empty() && w.size() <= 40) s.lastPreset = CanonicalBuiltinPresetName(w); }
        else if (k == "compress_advanced_open") s.compressAdvancedOpen = ToBool(v, s.compressAdvancedOpen);
        else if (k == "dblclick_image_preview") s.doubleClickPreview = ToBool(v, s.doubleClickPreview);
        else if (k == "comic_reading_mode") s.comicReading = ToBool(v, s.comicReading);
        else if (k == "auto_try_passwords") s.autoTryPasswords = ToBool(v, s.autoTryPasswords);
        else if (k == "remember_run_passwords") s.rememberRunPasswords = ToBool(v, s.rememberRunPasswords);
        else if (k == "remember_password_default") s.rememberPasswordDefault = ToBool(v, s.rememberPasswordDefault);
        else if (k == "list_columns") {
            try {
                const long n = std::stol(v);
                if (n >= 0 && n <= 255) s.listColumns = static_cast<unsigned>(n);
            } catch (...) {}
        }
        else if (k == "list_col_widths") {
            // only letters, digits, '=' and ',' (a list of name=number); anything else is not ours
            bool ok = v.size() <= 200;
            for (char c : v) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '=' && c != ',') ok = false;
            if (ok) s.listColWidths = v;
        }
        else if (k == "tree_width") {
            try {
                const long n = std::stol(v);
                if (n >= 140 && n <= 420) s.treeWidth = static_cast<unsigned>(n);
            } catch (...) {}
        }
        else if (k == "remember_view_mode") s.rememberViewMode = ToBool(v, s.rememberViewMode);
        else if (k == "icon_thumbnails") s.iconThumbnails = ToBool(v, s.iconThumbnails);
        else if (k == "thumb_size") {
            try {
                const long n = std::stol(v);
                if (n >= 0 && n <= 3) s.thumbSize = static_cast<unsigned>(n);
            } catch (...) {}
        }
        else if (k == "preview_enlarge_small") s.previewEnlargeSmall = ToBool(v, s.previewEnlargeSmall);
        else if (k == "image_prefetch_mb") {
            try {
                int n = std::stoi(v);
                if (n >= 0 && n <= 2048) s.prefetchMB = static_cast<unsigned>(n);
            } catch (...) {}
        }
        else if (k == "language") {
            // only a value that looks like a language code is accepted (letters / digits / hyphen); anything else goes back to auto
            bool ok = !v.empty() && v.size() <= 16;
            for (char c : v) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-') ok = false;
            if (ok) s.language = FromUtf8(v);
        }
        else if (k == "temp_dir") {
            const std::wstring w = FromUtf8(v);
            if (w.size() >= 3 && w.size() <= 240 && fs::path(w).is_absolute()) s.tempDir = w;  // anything else = the system temp folder
        }
        else if (k == "shell_items") s.shellItems = NormalizeShellItems(v);
        else if (k == "shell_menu") s.shellMenu = ToBool(v, s.shellMenu);
        else if (k == "shell_presets") s.shellPresets = NormalizeBuiltinPresets(v);
        else if (k == "check_updates") s.checkUpdates = ToBool(v, s.checkUpdates);
        else if (k == "last_update_check") { try { s.lastUpdateCheck = std::max(0LL, std::stoll(v)); } catch (...) {} }
        else if (k == "skip_update_version") { if (v.size() <= 32) s.skipUpdateVersion = v; }
        else if (k == "debug_log") s.debugLog = ToBool(v, s.debugLog);
        else if (k == "log_full_paths") s.logFullPaths = ToBool(v, s.logFullPaths);
        else if (k == "hash_algos") {
            try {
                const long n = std::stol(v);
                if (n >= 0 && n < (1 << 20)) s.hashAlgos = NormalizeHashAlgos(static_cast<uint32_t>(n));
            } catch (...) {}
        }
        else if (k == "shell_format_1") s.shellFormat1 = FromText(kFormats, v, s.shellFormat1);
        else if (k == "shell_format_2") s.shellFormat2 = FromText(kFormats, v, s.shellFormat2);
        else if (k == "recent_extract_dirs") {
            // '|'-separated absolute folder paths
            s.recentExtract.clear();
            size_t pos = 0;
            while (pos <= v.size() && s.recentExtract.size() < kMaxRecentExtract) {
                size_t end = v.find('|', pos);
                if (end == std::string::npos) end = v.size();
                const std::wstring w = FromUtf8(v.substr(pos, end - pos));
                if (w.size() >= 3 && w.size() <= 240 && fs::path(w).is_absolute()) s.recentExtract.push_back(w);
                pos = end + 1;
            }
        }
        else if (k == "animations") s.animations = ToBool(v, s.animations);
        else if (k == "sound_effects") s.soundEffects = ToBool(v, s.soundEffects);
        else if (k == "sound_set") s.soundSet = FromText(kSoundSets, v, s.soundSet);
        else if (k == "compact_close_when_done") s.closeCompactWhenDone = ToBool(v, s.closeCompactWhenDone);
        else if (k == "reveal_after_compress") s.revealAfterCompress = ToBool(v, s.revealAfterCompress);
        else if (k == "task_priority") s.taskPriority = FromText(kTaskPriorities, v, s.taskPriority);
        else if (k == "notifications") s.notifications = ToBool(v, s.notifications);
        else if (k == "parallel_tasks") {
            try {
                int n = std::stoi(v);
                if (n >= 1 && n <= 4) s.parallelTasks = static_cast<unsigned>(n);
            } catch (...) {}
        }
    }
    if (!sawWelcome) s.welcomeDone = true;   // a settings file from before the welcome window: this user has used the program already
    return s;
}

bool SaveSettings(const fs::path& file, const Settings& s) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        // Grouped by what the settings are for (the loader reads key=value lines and skips the [section] lines: the keys are unique, so
        // a file without sections loads as well).
        auto yn = [](bool b) { return b ? "true" : "false"; };
        out << "settings_format=1\n"

            << "\n[appearance]\n"
            << "theme=" << ToText(kThemes, s.theme) << "\n"
            << "accent=" << ToText(kAccents, s.accent) << "\n"
            << "background=" << ToText(kBackgrounds, s.background) << "\n"
            << "language=" << ToUtf8(s.language) << "\n"
            << "animations=" << yn(s.animations) << "\n"
            << "sound_effects=" << yn(s.soundEffects) << "\n"
            << "sound_set=" << ToText(kSoundSets, s.soundSet) << "\n"

            << "\n[view]\n"
            << "view_mode=" << ToText(kViewModes, s.viewMode) << "\n"
            << "browse_flat=" << yn(s.browseFlat) << "\n"
            << "compact_rows=" << yn(s.compactRows) << "\n"
            << "shell_icons=" << yn(s.shellIcons) << "\n"
            << "show_folder_tree=" << yn(s.showFolderTree) << "\n"
            << "preview_pane=" << yn(s.previewPane) << "\n"
            << "remember_view_mode=" << yn(s.rememberViewMode) << "\n"
            << "icon_thumbnails=" << yn(s.iconThumbnails) << "\n"
            << "thumb_size=" << s.thumbSize << "\n"

            << "\n[preview]\n"
            << "dblclick_image_preview=" << yn(s.doubleClickPreview) << "\n"
            << "comic_reading_mode=" << yn(s.comicReading) << "\n"
            << "open_preview_key=" << ToText(kPreviewKeys, s.openPreviewKey) << "\n"
            << "audio_loop=" << ToText(kAudioLoops, s.audioLoop) << "\n"
            << "dblclick_audio_preview=" << yn(s.doubleClickAudioPreview) << "\n"
            << "preview_enlarge_small=" << yn(s.previewEnlargeSmall) << "\n"
            << "image_prefetch_mb=" << s.prefetchMB << "\n"

            << "\n[passwords]\n"
            << "auto_try_passwords=" << yn(s.autoTryPasswords) << "\n"
            << "remember_run_passwords=" << yn(s.rememberRunPasswords) << "\n"
            << "remember_password_default=" << yn(s.rememberPasswordDefault) << "\n"

            << "\n[extract]\n"
            << "conflict=" << ToText(kConflicts, s.conflict) << "\n"
            << "extract_mode=" << ToText(kExtractModes, s.extractMode) << "\n"
            << "open_folder_after_extract=" << yn(s.openFolderAfterExtract) << "\n"
            << "open_archive_in=" << ToText(kOpenPolicies, s.openIn) << "\n"
            << "drag_out_virtual=" << yn(s.dragOutVirtual) << "\n"
            << "keep_damaged=" << yn(s.keepDamaged) << "\n"
            << "bomb_check=" << yn(s.bombCheck) << "\n"

            << "\n[compress]\n"
            << "default_format=" << ToText(kFormats, s.defaultFormat) << "\n"
            << "default_profile=" << ToText(kProfiles, s.defaultProfile) << "\n"
            << "test_after_compress=" << yn(s.testAfterCompress) << "\n"
            << "sums_archive=" << yn(s.sumsArchive) << "\n"
            << "sums_files=" << yn(s.sumsFiles) << "\n"
            << "reveal_after_compress=" << yn(s.revealAfterCompress) << "\n"

            << "\n[modify]\n"
            << "confirm_delete_entries=" << yn(s.confirmDeleteEntries) << "\n"
            << "modify_keep_backup=" << yn(s.keepBackupOnModify) << "\n"
            << "confirm_write_back=" << yn(s.confirmWriteBack) << "\n"
            << "editor_path=" << ToUtf8(s.editorPath) << "\n"

            << "\n[safety]\n"
            << "warn_executable=" << yn(s.warnExecutable) << "\n"
            << "lock_open_archive=" << yn(s.lockOpenArchive) << "\n"
            << "executable_scope=" << ToText(kOpenScopes, s.execScope) << "\n"
            << "skip_mac_metadata=" << yn(s.skipMacMetadata) << "\n"
            << "skip_thumbs_db=" << yn(s.skipThumbsDb) << "\n"
            << "hide_mac_metadata=" << yn(s.hideMacMetadata) << "\n"
            << "confirm_delete_archive=" << yn(s.confirmDeleteArchive) << "\n"

            << "\n[tasks]\n"
            << "parallel_tasks=" << s.parallelTasks << "\n"
            << "task_priority=" << ToText(kTaskPriorities, s.taskPriority) << "\n"
            << "notifications=" << yn(s.notifications) << "\n"
            << "compact_close_when_done=" << yn(s.closeCompactWhenDone) << "\n"
            << "temp_dir=" << ToUtf8(s.tempDir) << "\n"

            << "\n[explorer]\n"
            << "shell_menu=" << yn(s.shellMenu) << "\n"
            << "shell_items=" << s.shellItems << "\n"
            << "shell_presets=" << s.shellPresets << "\n"
            << "shell_format_1=" << ToText(kFormats, s.shellFormat1) << "\n"
            << "shell_format_2=" << ToText(kFormats, s.shellFormat2) << "\n"

            << "\n[hash]\n"
            << "hash_algos=" << s.hashAlgos << "\n"

            << "\n[diagnostics]\n"
            << "debug_log=" << yn(s.debugLog) << "\n"
            << "log_full_paths=" << yn(s.logFullPaths) << "\n"

            << "\n[updates]\n"
            << "check_updates=" << yn(s.checkUpdates) << "\n"
            << "last_update_check=" << s.lastUpdateCheck << "\n"
            << "skip_update_version=" << s.skipUpdateVersion << "\n"

            << "\n[state]\n"
            << "stale_menu_ignored=" << ToUtf8(s.ignoredMenuPath) << "\n"
            << "two_copies_notice_shown=" << yn(s.twoCopiesNoticeShown) << "\n"
            << "update_mode=" << ToText(kAddModes, s.addMode) << "\n"
            << "existing_output=" << ToText(kExistingOutputs, s.existingOutput) << "\n"
            << "welcome_done=" << yn(s.welcomeDone) << "\n"
            << "last_preset=" << ToUtf8(s.lastPreset) << "\n"
            << "compress_advanced_open=" << yn(s.compressAdvancedOpen) << "\n"
            << "list_columns=" << s.listColumns << "\n"
            << "list_col_widths=" << s.listColWidths << "\n"
            << "tree_width=" << s.treeWidth << "\n"
            << "preview_pane_height=" << s.previewPaneHeight << "\n"
            << "preview_window_rect=" << s.previewWindowRect << "\n"
            << "recent_extract_dirs=" << JoinDirs(s.recentExtract) << "\n";
        if (!out.good()) return false;
    }
    fs::remove(file, ec);
    fs::rename(tmp, file, ec);
    return !ec;
}

OverwriteMode ToOverwriteMode(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::Skip: return OverwriteMode::Skip;
        case ConflictPolicy::Overwrite: return OverwriteMode::Overwrite;
        case ConflictPolicy::Rename: return OverwriteMode::Rename;
        default: return OverwriteMode::Ask;
    }
}

}  // namespace lycan
