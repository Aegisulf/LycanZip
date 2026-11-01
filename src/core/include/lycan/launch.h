#pragma once
// Launch requests: what the command line (context menu, file associations, drag onto the exe) and the single-instance
// hand-over ask LycanZip to do.
//   LycanZip.exe <archive>                          open
//   LycanZip.exe <paths...>                         several paths = create an archive (shows the page)
//   LycanZip.exe --add <paths...>                   create an archive (shows the page)
//   LycanZip.exe --compress <fmt> <paths...>        compress right away (no page); fmt = zip 7z tar.gz tar.xz tar.bz2
//        [--template <name>]                        ... with a preset from presets.ini
//   LycanZip.exe --compress-each <fmt> <paths...>   one archive per path
//   LycanZip.exe --add-encrypted <paths...>         ask for a password, then compress
//   LycanZip.exe --extract-here <archives...>       extract next to the archive (smart extraction)
//   LycanZip.exe --extract-folder <archives...>     extract into a folder named after the archive
//   LycanZip.exe --extract-to <archives...>         ask for a folder, then extract there
//        [--dest <folder>]                          ... or extract there without asking
//   --dest <folder> also works with --extract-here / --extract-folder (extract into that folder instead of next to the
//   archive) and --compress / --compress-each (create the archive there): the right-drag menu uses it
//   LycanZip.exe --extract-merge <archives...>      extract all archives into one folder (asked for)
//   LycanZip.exe --test <archives...>               test integrity
//   LycanZip.exe --preview-images <archive>         image preview only (no main window unless something is wrong)
//   LycanZip.exe --preview-audio <archive>          audio preview only
//   LycanZip.exe --hash-save <files or folders...>  SHA-256 of them into name.sha256 next to them (one item: its name; several: the folder they are in);
//                                                   no window unless it takes long or the file is there already
//   LycanZip.exe --verify <checksum files...>       check .sha256 / .md5 / .sfv ... files (one file or several; a checksum file given alone or
//                                                   with other checksum files only does this too)
//   LycanZip.exe --open-content <file>              open an .exe / .iso / .docx ... as an archive
//   LycanZip.exe --convert <archives...>            convert the archives to another format (shows the new archive page)
//   Option --list <file>: read the paths from a text file (UTF-8, one per line); avoids the command-line length limit
#include "lycan/message.h"
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/compress.h"

namespace lycan {

enum class LaunchAction {
    None, Open, Add,
    Compress,  // format / preset in the request
    ExtractHere, ExtractFolder,
    CompressEach, AddEncrypted, ExtractTo, ExtractMerge, Test, PreviewImages, PreviewAudio, OpenContent, Hash, Verify, HashSave,
    Convert,
    Last = Convert
};

struct LaunchRequest {
    LaunchAction action = LaunchAction::None;
    std::vector<std::filesystem::path> paths;
    ArchiveType format = ArchiveType::Zip;  // Compress / CompressEach
    std::wstring preset;                    // Compress: name of a preset (empty = none)
    std::filesystem::path dest;             // ExtractTo / ExtractMerge: the folder (empty = ask); Compress / CompressEach: the folder
                                            // for the archive (empty = next to the source; the right-drag menu sets it)
    bool valid = true;  // false: the arguments could not be parsed (e.g. --compress without a format)
    Message error;

    // Requests that are carried out in the compact window (not the main window): the context-menu actions
    bool IsQuickAction() const {
        switch (action) {
            case LaunchAction::Add: case LaunchAction::Compress: case LaunchAction::CompressEach: case LaunchAction::AddEncrypted:
            case LaunchAction::ExtractHere: case LaunchAction::ExtractFolder: case LaunchAction::ExtractTo:
            case LaunchAction::ExtractMerge: case LaunchAction::Test: case LaunchAction::Convert:
                return true;
            default: return false;
        }
    }
    // Requests that show only a preview window (the main window stays hidden unless something needs the user)
    bool IsPreviewOnly() const { return action == LaunchAction::PreviewImages || action == LaunchAction::PreviewAudio; }
    // Quick actions that read archives (the compact window shows the extraction options row)
    bool IsArchiveQuickAction() const {
        return action == LaunchAction::ExtractHere || action == LaunchAction::ExtractFolder || action == LaunchAction::ExtractTo ||
               action == LaunchAction::ExtractMerge || action == LaunchAction::Test;
    }
};

// "zip" "7z" "tar.gz" "tar.xz" "tar.bz2" (case-insensitive) <-> ArchiveType. ParseFormatName returns false for anything else.
bool ParseFormatName(const std::wstring& name, ArchiveType& out);
const wchar_t* FormatName(ArchiveType t);

// args do not include the exe name.
LaunchRequest ParseLaunchArgs(const std::vector<std::wstring>& args);

// Serialization for the hand-over between instances (line 1: action;format;preset, line 2: dest, then one path per line).
std::wstring SerializeLaunchRequest(const LaunchRequest& r);
LaunchRequest DeserializeLaunchRequest(const std::wstring& s);

}  // namespace lycan
