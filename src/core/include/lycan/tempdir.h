#pragma once
// Where Core puts its scratch files (the tar of a tar.gz while it is opened or created), and what happens when there is
// not enough room there. Scratch files are created with FILE_FLAG_DELETE_ON_CLOSE, so the system removes them even when
// the process dies.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/message.h"

namespace lycan {

// `custom` when it is an existing (or creatable) folder, otherwise the system temp folder.
std::filesystem::path ScratchDirectory(const std::filesystem::path& custom);

// Free bytes on the volume of `dir` (what the scratch precheck sees: honours SetTestFreeSpace and the environment
// variable LYCAN_TEST_FREE_SPACE=<bytes>); false when it cannot be determined.
bool ScratchFreeBytes(const std::filesystem::path& dir, uint64_t& freeBytes);

struct LocalVolume {
    std::filesystem::path root;  // e.g. D:\ 
    uint64_t freeBytes = 0;
};
// Fixed local disks (no network shares, no removable media) with their free space
std::vector<LocalVolume> LocalFixedVolumes();
// A network share or removable / optical drive: scratch files there would be slow
bool IsSlowLocation(const std::filesystem::path& dir);

// The error thrown (lycan::Error with kTempSpace) when the scratch folder is too small. `need` = 0: not known in advance
// (the disk filled up while writing).
Message MakeTempSpaceMessage(uint64_t need, uint64_t have, const std::filesystem::path& dir);
// Reads the numbers back out of the message of such an error (they are arguments of the message, after the ones that are shown).
bool ParseTempSpaceMessage(const Message& message, uint64_t& need, uint64_t& have, std::wstring& dir);

}  // namespace lycan
