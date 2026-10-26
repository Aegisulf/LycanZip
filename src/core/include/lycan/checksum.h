#pragma once
// Checksum files (.sha256, .sha1, .sha512, .md5, .sfv, SHA256SUMS ...): reading them, checking the files they list (on disk or
// inside an archive) and making them (for a folder, and next to a new archive). The hash values themselves come from hash.h.
//
// Names in a checksum file are relative to the folder the checksum file is in. The report of a check lists only what is wrong
// (a different value, a file that is not there, a file that could not be read) and counts the rest: a long list of "OK" lines
// hides the one line that matters.
#include "lycan/message.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/hash.h"
#include "lycan/vfs.h"

namespace lycan {

// The algorithm a checksum file name stands for: .sha256 / .sha1 / .sha384 / .sha512 / .md5 / .sfv (CRC-32) and the names that
// sha256sum style tools write (SHA256SUMS, MD5SUMS ...). 0 = not a checksum file.
uint32_t SumAlgoForPath(const std::filesystem::path& file);
inline bool IsSumFile(const std::filesystem::path& file) { return SumAlgoForPath(file) != 0; }

struct SumEntry {
    std::wstring name;      // as written in the file
    std::string hex;        // lower case
    uint32_t algo = 0;      // one HashAlgo bit (BSD lines name it, otherwise it follows from the number of digits)
};

enum class SumFileError { None, Unreadable, TooBig, NoLines };  // the caller turns this into a message

struct SumFile {
    std::filesystem::path path;
    std::vector<SumEntry> entries;
    uint32_t badLines = 0;          // lines that are neither a comment nor a checksum line
    SumFileError error = SumFileError::None;   // the file could not be read, or has no checksum line at all
};

// sha256sum / md5sum lines ("<hex> *name"), BSD lines ("SHA256 (name) = <hex>") and .sfv lines ("name <crc>", ';' = comment).
// UTF-8 (with or without BOM); a line that is not valid UTF-8 is read as the system's ANSI code page (older tools).
SumFile ReadSumFile(const std::filesystem::path& file);

enum class SumStatus { Match, Mismatch, Missing, Error };

struct SumProblem {
    std::wstring name;              // as written in the checksum file
    SumStatus status = SumStatus::Mismatch;
    std::string expected, actual;   // Mismatch
    Message error;             // Error
};

struct SumVerifyReport {
    std::filesystem::path sumPath;
    SumFileError error = SumFileError::None;   // the checksum file itself could not be used
    uint32_t total = 0, matched = 0, mismatched = 0, missing = 0, errors = 0;
    std::vector<SumProblem> problems;       // everything that is not a match
    std::vector<std::wstring> extra;        // archive: files the checksum file does not mention
    uint64_t bytes = 0;
    int sumSkip = 0, entrySkip = 0;         // archive: leading folders dropped to make the names meet (the automatic guess)
    bool confident = true;                  // archive: at least half of the names were found
    bool allMatch() const { return error == SumFileError::None && total > 0 && matched == total; }
};

struct SumVerifyOptions {
    ProgressFn progress;            // false = cancel (throws lycan::Error with cancelled())
    PasswordFn password;            // archive entries
};

// Checks the files next to the checksum file.
SumVerifyReport VerifySumFile(const SumFile& sums, const SumVerifyOptions& options = {});

// ---- a checksum file against an archive ---------------------------------------------------------------------------------

// How the names of a checksum file are turned into archive paths. Automatic (the default) tries the names as they are, then with
// one or two leading folders dropped on either side (a checksum file made one level above the archive's content, or the other way
// round) and takes the best; when less than half of the names are found that way the match is not "confident" and the caller
// should ask for a prefix.
struct ArchiveSumOptions {
    std::wstring stripPrefix;       // removed from the start of the names of the checksum file (when they start with it)
    std::wstring addPrefix;         // put in front of the names of the checksum file (after stripping)
    bool manual = false;            // the two prefixes above are used instead of the automatic guess
};

struct ArchiveSumMatch {
    std::vector<int64_t> entryOf;   // per entry of the checksum file: the index of the archive entry, -1 = not found
    uint32_t found = 0;
    bool confident = false;         // at least half of the names were found
    int sumSkip = 0, entrySkip = 0; // the automatic guess: leading folders dropped from the checksum file's names / the entries' paths
};

ArchiveSumMatch MatchSumToArchive(const SumFile& sums, const std::vector<Entry>& entries, const ArchiveSumOptions& options = {});

// Reads the files of the archive that the checksum file lists (one pass, nothing is written to disk) and compares.
SumVerifyReport VerifySumAgainstArchive(const SumFile& sums, ArchiveSource& archive, const SumVerifyOptions& options = {},
                                        const ArchiveSumOptions& match = {});

// The checksum files that sit next to an archive: "<archive file name>.<ext>" (the archive itself: name.zip.sha256) and
// "<name without the archive extension>.<ext>" (the files inside: name.sha256). Empty = not there.
struct ArchiveSidecars {
    std::filesystem::path archiveSum;
    std::filesystem::path contentsSum;
};
ArchiveSidecars FindArchiveSidecars(const std::filesystem::path& archive);

// ---- making checksum files ----------------------------------------------------------------------------------------------

// Files stay as they are, folders are replaced by the files in them (all levels, sorted; links are not followed).
std::vector<std::filesystem::path> ExpandForHash(const std::vector<std::filesystem::path>& paths);

struct SumWriteOptions {
    uint32_t algo = kHashSha256;    // one algorithm
    ProgressFn progress;
};

struct SumWriteResult {
    std::vector<std::filesystem::path> written;
    uint32_t skipped = 0;           // files that could not be read (not in the checksum file)
};

// "name.zip.sha256" for the archive file and / or "name.sha256" for the files that went into it (names as in the archive: relative
// to the folder of each source). Written after the archive is complete; on cancel nothing is left behind. An archive in volumes:
// `archive` is the first volume, `parts` all of them (the checksum file of the archive then has one line per volume).
SumWriteResult WriteArchiveSums(const std::filesystem::path& archive, const std::vector<std::filesystem::path>& sources, bool archiveSum, bool filesSum,
                                const SumWriteOptions& options = {}, const std::vector<std::filesystem::path>& parts = {});

// A checksum file for the files of `paths` (folders expanded): names relative to `base` with '/'. Written as UTF-8 without BOM.
SumWriteResult WriteSumFile(const std::filesystem::path& out, const std::vector<std::filesystem::path>& paths, const std::filesystem::path& base,
                            const SumWriteOptions& options = {});

// The extension for a checksum file of that algorithm: L".sha256" ... L".md5", L".sfv"
const wchar_t* SumExtensionFor(uint32_t algo);

}  // namespace lycan
