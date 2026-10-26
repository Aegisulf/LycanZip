#pragma once
// Internals of ArchiveSource, shared by archive_source.cpp and extract.cpp.
#include <filesystem>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/message.h"
#include "sevenzip.h"

namespace lycan {

struct ArchiveSource::Impl {
    std::filesystem::path file;
    std::wstring format;
    Capabilities caps;
    std::vector<Entry> entries;
    std::vector<std::wstring> methodNames{std::wstring()};   // the methods of the items (Entry::methodIdx); index 0 = none
    CMyComPtr<IInArchive> arc;
    std::vector<std::string> rawNames;  // parallel to entries; empty string = this item has no raw bytes that can be decoded again
    NameEncodingInfo nameInfo;
    bool passwordAsked = false;  // a handler asked for a password while opening (tells "wrong password" from "format not recognised")
    bool rawCp437 = false;  // the handler was set to cp=437 before opening, so GetProperty(kpidPath) gives back the original bytes unchanged
    CMyComPtr<IInArchive> outer;   // split volumes: the Split handler (owns the volume streams)
    CMyComPtr<IInStream> backing;  // double-layer formats: stream of the temporary file of the inner tar (the file is deleted when it is released)
    PasswordFn password;
};

// Extraction target: the caller decides where each item goes (disk / memory / discarded).
// Note: these methods are called from callbacks of 7z.dll and may throw lycan::Error;
// RunExtract catches it and throws it again when Extract returns.
class ExtractTarget {
public:
    virtual ~ExtractTarget() = default;
    // return a null pointer = nothing is written for this item (directory, refused, already exists and skipped ...)
    virtual CMyComPtr<ISequentialOutStream> Begin(uint32_t index) = 0;
    virtual void End(uint32_t index, int opResult) = 0;
};

Message OpResultMessage(int opResult);  // "OK", "CRC error" ... as a message
// The item came out complete: OK, or "data after the end of the payload" (the compressed stream ended before the packed size said it would:
// Windows and Bandizip do not mind it; 7-Zip reports it after the CRC matched). Such an item is kept and only noted.
inline bool OpResultUsable(int opResult) { return opResult == NArchive::NExtract::NOperationResult::kOK || opResult == NArchive::NExtract::NOperationResult::kDataAfterEnd; }

// util.cpp: unique scratch file in %TEMP%, deleted when the last handle closes (INVALID_HANDLE_VALUE on failure)
HANDLE CreateScratchFile(const wchar_t* prefix, const std::filesystem::path& dir = {});  // dir empty = the system temp folder

// indices empty = all; with test = true Begin is not called.
void RunExtract(ArchiveSource::Impl& impl, const std::vector<uint32_t>& indices, bool test,
                ExtractTarget& target, const ProgressFn& progress, const PasswordFn& password);

}  // namespace lycan
