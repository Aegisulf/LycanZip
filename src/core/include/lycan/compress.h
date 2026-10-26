#pragma once
// Creating archives: zip / 7z / tar / tar.gz / tar.xz / tar.bz2, passwords, volumes, profiles.
#include "lycan/message.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/vfs.h"

namespace lycan {

enum class ArchiveType { Zip, SevenZip, Tar, TarGz, TarXz, TarBz2, Iso };   // Iso: ISO 9660 + Joliet, written by lycan::WriteIso (iso.h); no compression, password or volumes

// Ready-made combinations for everybody; the user does not have to understand the parameters.
enum class Profile {
    Compatible,  // compatibility first: zip uses Deflate, encryption uses ZipCrypto (any system opens it, but it is weak)
    Balanced,    // balanced (default)
    Smallest,    // smallest size: highest compression level, slower
    Fastest,     // fastest
};

// How the paths inside the archive are recorded
enum class PathMode {
    Relative,  // each input is put at the top level under its name (default)
    Full,      // full path (without the drive letter), for example Users\me\docs.txt
};

// Special values of the solid block size (CompressOptions::solidSize)
constexpr int64_t kSolidAuto = -1;                       // 7-Zip decides from the level
constexpr int64_t kSolidOff = 0;                         // not solid: every file is compressed on its own (can be unpacked alone, larger)
constexpr int64_t kSolidAll = INT64_MAX;                 // everything in one block (smallest; unpacking one file has to read what comes before it)

struct CompressOptions {
    ArchiveType type = ArchiveType::Zip;
    Profile profile = Profile::Balanced;
    int level = -1;                  // -1 = from the profile; 0 (store only) .. 9
    std::wstring password;           // empty = not encrypted. zip uses AES-256 by default (ZipCrypto for Compatible)
    bool encryptFileNames = false;   // 7z only: encrypt the file names too
    uint64_t volumeSize = 0;         // > 0 = split into volumes (bytes). Makes <name>.001, .002 ... (zip / 7z)
    bool overwrite = false;          // false: fail when the target file exists already
    unsigned threads = 0;            // 0 = automatic

    // ---- Advanced (a profile is enough normally; an empty value / 0 / -1 all mean "automatic") ----
    std::wstring method;             // compression method: zip = Deflate / Deflate64 / BZip2 / LZMA / PPMd / Copy; 7z = LZMA2 / LZMA / PPMd / BZip2 / Deflate / Copy
    uint64_t dictSize = 0;           // dictionary size (bytes; for PPMd the memory size). LZMA / LZMA2 / PPMd only
    unsigned wordSize = 0;           // word size (for PPMd the order). LZMA / LZMA2 / PPMd / Deflate only
    int64_t solidSize = kSolidAuto;  // 7z only: solid block size
    std::wstring zipEncryption;      // zip only: L"AES256" or L"ZipCrypto"; empty = from the profile
    PathMode pathMode = PathMode::Relative;
    std::wstring customParams;       // custom 7-Zip parameters: name=value separated by spaces (a -m prefix is allowed), for example "mx=9 fb=64"; fails when not understood
    std::filesystem::path tempDir;   // tar.* only: where the uncompressed tar intermediate goes; empty = the system temp folder
    std::wstring isoLabel;           // ISO only: the volume label (empty = the name of the output file)
    bool isoKeepTimes = true;        // ISO only: false = every item gets the time of the writing
    bool skipThumbsDb = false;       // leave Thumbs.db out of folders that are added (a file chosen on its own is kept)
    // "Smart store" (zip / 7z): files that are compressed already (incompressible.h, 64 KB and up) go in as they are instead
    // of being packed again. Costs a second pass over the stored data when stored and packed files are mixed. Not used with
    // level 0 (everything is stored anyway). The "smallest" preset switches it off.
    bool storeCompressed = true;
    ProgressFn progress;             // false cancels; nothing is left behind when cancelled or failed
};

struct CompressReport {
    uint64_t files = 0, dirs = 0;
    uint64_t bytesIn = 0;                 // original total
    uint64_t bytesOut = 0;                // total size of the output files (all volumes)
    std::vector<std::filesystem::path> outputs;  // the files really made (the volumes when split)
    std::vector<Message> skipped;    // symbolic links, the output file itself
    uint32_t thumbsDbSkipped = 0;         // Thumbs.db files left out (CompressOptions::skipThumbsDb); not a problem, so not in skipped
    uint32_t storedFiles = 0;             // files put in as they are (CompressOptions::storeCompressed)
    std::vector<Message> errors;     // files that could not be read etc.
    std::vector<Message> warnings;   // for example ZipCrypto is weak
    bool ok() const { return errors.empty(); }
};

// inputs can be files or folders; each input goes to the top level of the archive under its name. Throws lycan::Error on failure.
CompressReport CreateArchive(const std::filesystem::path& output, const std::vector<std::filesystem::path>& inputs,
                             const CompressOptions& options = {});

// The compression methods you can choose for this format (the first is the default); the tar family has none to choose.
std::vector<std::wstring> MethodsFor(ArchiveType type);
// Whether this method has a dictionary / word size (for the UI to enable or disable those fields)
bool MethodHasDict(const std::wstring& method);
bool MethodHasWordSize(const std::wstring& method);

// Guesses the type from the file extension (.zip .7z .tar .tar.gz .tgz .tar.xz .txz .tar.bz2 .tbz2); returns false when not recognised.
bool GuessArchiveType(const std::filesystem::path& file, ArchiveType& type);

}  // namespace lycan
