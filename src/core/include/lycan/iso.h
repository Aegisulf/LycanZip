#pragma once
// Writing an ISO image (ISO 9660 with the Joliet extension: long names, Unicode). 7z.dll cannot write ISO files, so this is
// our own writer. No UDF and no boot image yet (see the to-do list): a file of 4 GiB or more is refused.
#include "lycan/message.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/vfs.h"

namespace lycan {

struct IsoItem {
    std::filesystem::path source;   // the file on the disk (unused for folders)
    std::wstring path;              // the path in the image, separated by '\' (parents that are not listed are made)
    bool isDir = false;
    uint64_t size = 0;
    uint64_t mtime = 0;             // FILETIME value (100 ns since 1601, UTC); 0 = now
};

struct IsoOptions {
    std::wstring label;             // volume label (the name shown for the disc); a-z, 0-9 and _ in the ISO 9660 part, 16 characters in Joliet
    bool keepTimes = true;          // false: every item gets the time of the writing
    ProgressFn progress;            // false = cancel (throws Error E_ABORT)
};

struct IsoResult {
    uint64_t bytes = 0;                  // size of the image
    std::vector<Message> errors;    // files that could not be read (their place is filled with zeros)
};

// Writes the image to the open file handle `out` (from its current position, which must be the start). Throws lycan::Error.
IsoResult WriteIso(void* out, const std::vector<IsoItem>& items, const IsoOptions& options);

}  // namespace lycan
