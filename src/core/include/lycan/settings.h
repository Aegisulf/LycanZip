#pragma once
// User settings: the UI, the command line and the context menu extension share one file.
// The format is simple key=value (UTF-8, a line that starts with # is a comment); an unknown key is ignored and an invalid value goes back to the default, so old and new versions can share one file.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/compress.h"
#include "lycan/taskexec.h"
#include "lycan/extract.h"

namespace lycan {

enum class ThemeMode { System, Light, Dark, ContrastBlack };   // ContrastBlack: the program's own high contrast look (white on black), whatever Windows is set to

// Accent themes: Moonlight (default), Blood moon, Amber eyes, Aurora, Forest, follow the system accent colour
enum class AccentTheme { Moonlight, BloodMoon, Amber, Aurora, Forest, Night, System };   // Night: a near-black background and a silver accent

// What the audio preview does when a track ends: play on and stop after the last one, go round, repeat the track, or pick another one by chance
enum class AudioLoop { None, All, One, Shuffle };
enum class PreviewKey { Shift, Ctrl, Off };  // the key that, held while an archive is opened, goes straight to the picture preview

enum class ConflictPolicy { Ask, Skip, Overwrite, Rename };

// Background: full (gradient + home scene), gradient only, off (Mica only)
enum class BackgroundStyle { Full, Gradient, Off };

// Sound sets: Synth (default, made by the owner), Wave (the first sine wave tones); the files are in the folder Assets/Sounds/<set>/
enum class SoundSet { Synth, Wave };

// What to do with an item of the same name when adding to an existing archive (same order as lycan::UpdateMode); and what to do when the output file exists already when creating an archive
enum class AddMode { Replace, UpdateAdd, Freshen, Synchronize };
enum class ExistingOutput { Rename, Overwrite, Update };

// When a window already shows an archive and another one is opened: a new window / replace the content of the current window
enum class OpenPolicy { NewWindow, Replace };

// What "Extract here" does: Auto = a single root folder is placed as is, otherwise a folder named after the archive
// is created; NoFolder = contents go straight into the destination; Folder = always a folder named after the archive.
enum class ExtractMode { Auto, NoFolder, Folder };

// What is extracted when an executable inside an archive is opened: Folder = the folder it is in (with subfolders), so
// the program finds its dependencies; FileOnly = just that file.
enum class OpenScope { Folder, FileOnly };

enum class ViewMode { Details, List, Icons };  // How the content of an archive is shown: details / list / icons

inline constexpr size_t kMaxRecentExtract = 5;

// `dir` becomes the newest entry of the list (a repeated one moves up; the list is cut to kMaxRecentExtract)
void RememberExtractDir(std::vector<std::wstring>& list, const std::wstring& dir);

// Default image-preview prefetch budget (MB) for a machine with the given physical memory:
// 256 MB normally, 128 MB below 6.5 GB (i.e. 4 and 6 GB machines). Both are steps of the settings-page combo.
unsigned DefaultPrefetchMB(uint64_t totalPhysBytes);
unsigned DefaultPrefetchMB();  // for this machine

struct Settings {
    ThemeMode theme = ThemeMode::System;
    AccentTheme accent = AccentTheme::Moonlight;
    BackgroundStyle background = BackgroundStyle::Full;

    // Extraction
    ConflictPolicy conflict = ConflictPolicy::Ask;  // when the target already has a file of the same name
    ExtractMode extractMode = ExtractMode::Auto;    // what "Extract here" does (the main button and Ctrl+E)
    bool openFolderAfterExtract = false;
    bool lockOpenArchive = true;                     // the file of the archive a window shows cannot be changed or deleted by others while it is open
    bool warnExecutable = true;                     // ask before opening an executable / installer / script from an archive
    OpenScope execScope = OpenScope::Folder;        // what to extract for it (see OpenScope)
    bool bombCheck = true;                          // before extracting, check for an archive that is unusually large when expanded (like a decompression bomb) and ask whether to go on
    bool keepDamaged = true;                        // extraction: files with a data error are kept (not deleted) and listed in the message at the end; off = they are deleted
    bool skipMacMetadata = true;                    // extraction leaves out __MACOSX, ._ files and .DS_Store (junk.h)
    bool testAfterCompress = false;                 // test the integrity of the new archive automatically after compressing
    bool sumsArchive = false;                       // after compressing: write name.zip.sha256 (the checksum of the archive) next to it
    bool sumsFiles = false;                         // after compressing: write name.sha256 (the checksums of the files that went in) next to it

    // Compress
    bool skipThumbsDb = true;                       // folders that are added leave out Thumbs.db
    ArchiveType defaultFormat = ArchiveType::Zip;
    Profile defaultProfile = Profile::Balanced;

    // Several windows
    OpenPolicy openIn = OpenPolicy::NewWindow;      // how another archive is opened when a window shows one already

    // Archive view
    ViewMode viewMode = ViewMode::Icons;            // the default way of showing (icons; since 2026-10-13 new users start with icons + medium thumbnails)
    bool browseFlat = false;                        // default browsing mode: false = one level (folder navigation), true = list everything
    bool shellIcons = true;                         // the list and the folder tree use the Windows file icons (false = the coloured icons of LycanZip)
    bool compactRows = true;                        // row height of the list: compact (default) / comfortable
    bool showFolderTree = true;
    bool confirmDeleteArchive = true;               // ask before the folder tree's "delete the archive" moves it to the Recycle Bin
    bool confirmDeleteEntries = true;               // ask before items are deleted from inside an archive (the archive file itself is changed)
    bool confirmWriteBack = true;                   // after a file of an archive was edited: ask before it goes back into the archive
    std::wstring editorPath;                        // the program "edit" opens files in; empty = Notepad for text, the associated program for the rest
    bool keepBackupOnModify = false;                // changing an archive leaves the old file next to it as "<name>.bak"
    bool dragOutVirtual = true;                     // dragging files out hands them over while they are unpacked (virtual files); off = unpack to a temporary folder first (every target takes that)
    bool hideMacMetadata = true;                    // browsing and previews do not show __MACOSX, ._ files and .DS_Store

    // Page "New archive"
    std::wstring ignoredMenuPath;                   // LycanZip.exe whose stale menu / association registrations the user chose not to be reminded of (empty = remind)
    bool twoCopiesNoticeShown = false;              // "LycanZip is installed twice" (the Store version and another copy) was said once
    AddMode addMode = AddMode::Replace;             // the update mode chosen last time (replace and add / update and add / only update existing / synchronize)
    ExistingOutput existingOutput = ExistingOutput::Rename;   // when the output file exists already while creating an archive: the last choice (save as a new file / replace / update the existing archive)
    bool welcomeDone = false;                       // the welcome window was shown (finished or skipped); a settings file from before the window existed counts as done
    std::wstring lastPreset;                        // name of the preset used last (empty = pick a built-in preset by the "default format")
    bool compressAdvancedOpen = false;              // whether "Advanced options" is expanded (remembers the choice of the user)

    // Picture preview
    PreviewKey openPreviewKey = PreviewKey::Shift;   // opening an archive with this key held = the picture preview (Off: never)
    AudioLoop audioLoop = AudioLoop::None;           // the playback mode of the audio preview
    bool doubleClickAudioPreview = false;           // double-click an audio file in an archive: true = play it in the audio preview, false = open it with the associated program
    bool doubleClickPreview = true;                 // double-click a picture in an archive: true = open it in the picture preview, false = open it with the associated program
    unsigned listColumns = 11;                      // columns of the details view (bits: 1 type, 2 size, 4 packed, 8 modified, 16 ratio, 32 encrypted, 64 CRC, 128 method); the default has no packed column
    std::string listColWidths;                      // widths (DIP) that the user dragged the columns to: "name=320,type=70" (empty = the default of every column); read by the program, which knows the names
    unsigned treeWidth = 200;                       // width of the folder tree (DIP, 140-420)
    bool rememberViewMode = true;                   // remember the way of showing chosen last in the "View" menu (it decides viewMode when opening)
    bool iconThumbnails = true;                     // icon view: pictures show thumbnails (only for archives with random access that are not solid)
    unsigned thumbSize = 1;                         // icon size of the icon view: 0 small / 1 medium / 2 large / 3 huge (thumbnails of pictures are as large as the icons of other files)
    bool previewEnlargeSmall = false;               // whether "fit window / width / height" in the preview also enlarges small pictures (default: only shrinks)
    unsigned prefetchMB = DefaultPrefetchMB();      // upper limit of the read-ahead cache for paging in the preview (MB); 0 = no read-ahead; at most 2048; the default depends on the physical memory (DefaultPrefetchMB)

    // Where LycanZip puts its temporary files (opened files, dragged files, the tar of a tar.gz): empty = the system temp
    // folder; otherwise an absolute folder path (LycanZip creates a "LycanZip" folder inside it).
    std::wstring tempDir;

    // Explorer context menu (see shellitems.h): which items are shown, and the formats of the two "Compress to" items.
    bool shellMenu = true;                          // the Store version's switch of its context menu (its registration is part of the package and stays;
                                                    // the menu DLL shows nothing when this is off). The other versions register / remove the menu instead
    std::string shellItems;                         // empty = defaults, "-" = none, otherwise a comma-separated id list
    bool checkUpdates = false;                      // ask the web site once in seven days whether a newer version exists (versioncheck.h); off until the user turns it on
    long long lastUpdateCheck = 0;                  // seconds since 1970 of the last answer
    std::string skipUpdateVersion;                  // "Skip this version": no notice for this version (a newer one shows again)
    bool debugLog = false;                          // the diagnostic log writes the detailed messages too (diag.h); errors and warnings are always written
    bool logFullPaths = false;                      // the log shows whole paths instead of file names
    uint32_t hashAlgos = 15;                        // the hash dialog: the algorithms (a mask, see hash.h; 15 = CRC-32, MD5, SHA-1, SHA-256)
    std::string shellPresets;                       // the built-in presets in the context menu (shellitems.h): same form as shellItems
    ArchiveType shellFormat1 = ArchiveType::Zip;
    ArchiveType shellFormat2 = ArchiveType::SevenZip;
    std::vector<std::wstring> recentExtract;        // folders chosen with "Extract to...", newest first (at most kMaxRecentExtract); the menu offers them

    // Language: auto = follow the system; otherwise a language code (zh-TW / en-US / zh-CN ...). Loaded at start; a change needs a restart
    std::wstring language = L"auto";

    // Motion / sound
    bool animations = true;                         // animations of the interface (never played when the system "show animations" is off)
    SoundSet soundSet = SoundSet::Synth;            // which sound set
    bool soundEffects = false;                      // sound effects: off by default
    bool closeCompactWhenDone = false;              // compact window of a context menu action: close by itself after everything succeeded (default: no, the Done button closes it)
    bool revealAfterCompress = false;               // show the new file in Explorer after compressing
    bool notifications = true;                      // system notifications: when a task ends and the window is not in the foreground (failures always) show a balloon notification

    // Performance
    TaskPriority taskPriority = TaskPriority::Normal;  // priority of compress / extract / test tasks (worker processes honour it)
    unsigned parallelTasks = 1;                     // number of tasks running at the same time (1..4)

    bool operator==(const Settings&) const = default;
};

// Place of the settings file: if LycanZip.portable is next to the exe it is portable mode (settings.ini next to the exe),
// otherwise UserDataDir()\settings.ini (%LOCALAPPDATA%\LycanZip, or the LocalState of the Store package; datalocation.h).
std::filesystem::path DefaultSettingsPath();

Settings LoadSettings(const std::filesystem::path& file);   // file missing / damaged -> defaults
bool SaveSettings(const std::filesystem::path& file, const Settings& s);  // writes a temporary file first and renames it, so a file cut off in the middle cannot happen

OverwriteMode ToOverwriteMode(ConflictPolicy p);

}  // namespace lycan
