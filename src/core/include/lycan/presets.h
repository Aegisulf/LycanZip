#pragma once
// Compression presets: a set of compression settings that can be applied at once. The built-in ones are in the program (read only), the ones of the user are in presets.ini.
// A preset holds only "how to compress", not the file name, the output place, the source files or the password (a password is never written to a file).
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/compress.h"

namespace lycan {

struct CompressPreset {
    std::wstring name;
    bool builtin = false;
    ArchiveType type = ArchiveType::Zip;
    Profile profile = Profile::Balanced;
    int level = -1;
    std::wstring method;
    uint64_t dictSize = 0;
    unsigned wordSize = 0;
    int64_t solidSize = kSolidAuto;
    std::wstring zipEncryption;
    bool encryptFileNames = false;
    uint64_t volumeSize = 0;
    unsigned threads = 0;
    PathMode pathMode = PathMode::Relative;
    std::wstring customParams;
    bool storeCompressed = true;  // smart store: files that are compressed already go in as they are (CompressOptions::storeCompressed)
    bool sumsArchive = false;     // after compressing: write name.zip.sha256 next to the archive (checksum.h WriteArchiveSums)
    bool sumsFiles = false;       // ... and name.sha256 with the files that went in
    bool inMenu = true;           // listed in the Explorer context menu "Compress with a preset" (user presets; not a compression setting,
                                  // so SameSettings ignores it; the built-in ones are switched by settings.ini shell_presets)

    // Compares the content of the settings (without the name and builtin): used to tell "modified"
    bool SameSettings(const CompressPreset& o) const;
};

// Built-in presets: Normal ZIP, Normal 7z, Smallest, Fast, Mail / share, Backup, Store only
std::vector<CompressPreset> BuiltinPresets();

// presets.ini in the same folder as settings.ini
std::filesystem::path DefaultPresetsPath();

// Reads and writes only the presets of the user. When loading, sections with an invalid / duplicate name or the name of a built-in preset are skipped, and damaged or out-of-range values go back to the default.
std::vector<CompressPreset> LoadPresets(const std::filesystem::path& file);
bool SavePresets(const std::filesystem::path& file, const std::vector<CompressPreset>& userPresets);

// A name must not be empty, must not contain [ ] or control characters, must not be longer than 40 characters, and must not start or end with a space
bool IsValidPresetName(const std::wstring& name);

// Applies a preset to the compression options (the password and progress / overwrite are not touched)
void ApplyPreset(const CompressPreset& p, CompressOptions& o);

}  // namespace lycan
