#pragma once
#include "lycan/message.h"
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "lycan/archive_source.h"

namespace lycan {

enum class OverwriteMode {
    Fail,       // exists -> recorded as an error and the file is skipped
    Skip,       // exists -> skip
    Overwrite,  // exists -> overwrite
    Rename,     // exists -> rename automatically to "name (2).extension"
    Ask,        // exists -> call onConflict; without a callback it is the same as Fail
};

enum class ConflictAction { Overwrite, Skip, Rename, Cancel };

struct ConflictInfo {
    std::filesystem::path existing;  // the file that is on the disk
    std::wstring entryPath;          // path inside the archive
    uint64_t newSize = 0, existingSize = 0;
    uint64_t newMTime = 0, existingMTime = 0;  // Windows FILETIME, 0 = unknown
};

struct ConflictAnswer {
    ConflictAction action = ConflictAction::Skip;
    bool applyToAll = false;  // true: later conflicts use this choice
};

// Called on the worker thread, may block and wait for the user. Returning Cancel stops the whole extraction (throws E_ABORT).
using ConflictFn = std::function<ConflictAnswer(const ConflictInfo&)>;

// An archive that would unpack to far more than it is (what a compression bomb looks like). Asked before anything is written; false = cancel.
struct LargeExtractInfo {
    uint64_t declaredBytes = 0;   // the sizes the archive says its items have (of the items that are extracted)
    uint64_t packedBytes = 0;     // the size of the archive file
};
using LargeFn = std::function<bool(const LargeExtractInfo&)>;

struct ExtractOptions {
    OverwriteMode overwrite = OverwriteMode::Fail;
    ConflictFn onConflict;
    PasswordFn password;
    ProgressFn progress;
    // Compression bomb check that asks (onLarge) instead of failing (maxTotalBytes / maxRatio): when the declared size is `largeBytes` or more and at least
    // `largeRatio` times the archive, or `hugeBytes` or more whatever the ratio. An archive that fits the disk and is a normal one (a few times its size) never asks.
    LargeFn onLarge;
    uint64_t largeBytes = 1ull << 30;
    uint32_t largeRatio = 500;
    uint64_t hugeBytes = 1ull << 40;
    // An item that came out with a data error (CRC mismatch, damaged or cut-off data) is kept, under its name, instead of being deleted, so that what can be
    // read of it is not lost; the error is reported anyway (ExtractReport::kept, errors). A wrong password, an unsupported method, a cancel, a full disk
    // and the like still delete it. An existing file is never replaced by a damaged one: that one is kept beside it as "name (damaged).ext".
    bool keepDamaged = false;      // (the library default is the old behaviour; the program and the command line turn it on)
    bool skipMacMetadata = false;  // leave out __MACOSX, ._ files and .DS_Store (junk.h); they are not counted in the report
    bool propagateMotw = true;     // inherit the Zone.Identifier of the source archive
    uint64_t maxTotalBytes = 0;    // decompression bomb limit: upper limit of the total expanded size, 0 = none
    uint32_t maxRatio = 0;         // upper limit of the expansion / compression ratio, 0 = none
    // Smart extraction: when the content has only one root folder (or one file) it goes into destDir directly;
    // with several top-level items destDir/<wrapName> is made so that nothing is scattered.
    bool smart = false;
    std::wstring wrapName;         // empty = derived from the archive name (ArchiveBaseName)
    // When items are selected from a folder inside the archive, this prefix (like L"a/b/", ending with '/') is removed from the output path,
    // so the selected items go straight into the target folder. Items that do not start with the prefix keep their path; the prefix folder itself is skipped.
    std::wstring stripPrefix;
};

struct ExtractReport {
    uint64_t files = 0, dirs = 0, bytes = 0;
    std::filesystem::path outputDir;      // the folder where the content really is (for "open the folder when done")
    std::vector<Message> rejected;    // original paths that were refused by the safety rules
    std::vector<std::wstring> renamed;    // "original -> cleaned"
    std::vector<Message> skipped;    // skipped because they exist / skipped symbolic links
    std::vector<Message> errors;     // data errors, CRC errors etc.
    std::vector<Message> kept;       // "path : reason": extracted with an error and kept (ExtractOptions::keepDamaged); also in errors. "name (damaged).ext" when the real name was taken
    std::vector<Message> warnings;   // items whose packed data goes on after the end of the compressed stream (a quirk of the archive: the content is complete and its CRC matched, the file is kept)
    bool wrongPassword = false;           // an encrypted item failed to extract and the reason looks like a wrong password
    uint32_t macMetadataSkipped = 0;      // items left out because of ExtractOptions::skipMacMetadata
    bool ok() const { return errors.empty(); }
};

struct TestReport {
    uint64_t files = 0;                   // number of files that passed
    std::vector<Message> errors;     // "path : reason"
    std::vector<Message> warnings;   // see ExtractReport::warnings (the test passes)
    bool wrongPassword = false;
    bool ok() const { return errors.empty(); }
};

// Integrity test: runs once with the native test mode of 7-Zip (a solid archive is unpacked only once; ReadEntry for each item is O(n^2) on a solid archive).
// Uses only options.password and options.progress. Throws lycan::Error (E_ABORT) when cancelled.
TestReport TestArchive(ArchiveSource& src, const ExtractOptions& options = {});

// indices empty = all. May throw lycan::Error (cancelled, bomb limit, serious error).
ExtractReport ExtractTo(ArchiveSource& src, const std::filesystem::path& destDir,
                        const ExtractOptions& options = {},
                        const std::vector<uint32_t>& indices = {});

}  // namespace lycan
