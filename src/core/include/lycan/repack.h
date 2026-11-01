#pragma once
// Repacking an archive (DESIGN.md "重新封裝"): its content is unpacked to a scratch folder and packed again as a new archive,
// in another format, without the password or with another one. Used for "Convert the archive format" (a new file next to the old
// one) and "Remove the password" (the archive itself is replaced: built next to it, tested, then swapped in).
// "Upgrade the archive" (names in UTF-8) does not unpack anything: it is UpdateArchive with UpdateOptions::rewriteNames.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/message.h"

namespace lycan {

struct RepackOptions {
    CompressOptions compress;       // the new archive: type, profile, password (empty = not encrypted) ...; its progress is not used
    OpenOptions open;               // the old archive: its password, the code page of its names
    bool inPlace = false;           // replace the archive itself (`output` is ignored; compress.type should be its own format)
    bool skipDamaged = false;       // items that cannot be read are left out (RepackReport::skipped) instead of failing
    bool test = true;               // test the new archive afterwards (always done when inPlace: it is swapped in only when it passes)
    std::filesystem::path scratch;  // where the content is unpacked (empty = the system temp folder; see tempdir.h)
    ProgressFn progress;            // false cancels; nothing is left behind (the old archive is never changed)
};

struct RepackReport {
    uint64_t files = 0, dirs = 0, bytes = 0;      // what was unpacked and packed again
    std::filesystem::path output;                 // the new archive (the first volume when split); the archive itself when inPlace
    std::vector<std::filesystem::path> outputs;   // every file made (volumes)
    uint64_t archiveBefore = 0, archiveAfter = 0; // sizes of the old and the new archive (all volumes)
    bool tested = false;                          // the new archive was tested
    std::vector<Message> skipped;                 // items left out (damaged with skipDamaged, or refused by the safety rules)
    std::vector<Message> warnings;                // from packing (ZipCrypto is weak ...), or the counts do not match
    std::vector<Message> errors;                  // the test of the new archive failed (not inPlace: the file is kept)
    bool ok() const { return errors.empty(); }
};

// Throws lycan::Error: cancelled, wrong password, a damaged item (without skipDamaged), too little room in the scratch folder
// (Error::tempSpace), the target exists, or (inPlace) the new archive did not pass the test.
RepackReport RepackArchive(const std::filesystem::path& archive, const std::filesystem::path& output, const RepackOptions& options);

// The format of an opened archive as a type that can be written (zip, 7z, tar); false when it cannot be written (rar, cab ...).
bool WritableTypeOf(const ArchiveSource& src, ArchiveType& type);

}  // namespace lycan
