#pragma once
// VFS abstraction. Folders and archives share one interface; the UI does not need to know where the files come from.
// M0 implements ArchiveSource; LocalFolderSource comes in v1.3.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lycan {

struct Entry {
    uint32_t index = 0;          // stable index inside the source
    std::wstring path;           // relative path separated by '/' (the raw value, not filtered for safety)
    uint64_t size = 0;
    uint64_t packedSize = 0;
    uint64_t mtimeFileTime = 0;  // Windows FILETIME, 0 = unknown
    uint32_t attributes = 0;     // Win32 attributes; the unix mode is in the high 16 bits (as in 7-Zip)
    bool isDir = false;
    bool isEncrypted = false;
    bool isSymlink = false;
    bool isHardLink = false;     // tar hard link: the entry itself has no content (it names another entry)
    bool hasCrc = false;         // the archive stores the CRC-32 of the content (zip, 7z, rar ...); no need to unpack to know it
    uint32_t crc = 0;
    uint16_t methodIdx = 0;      // the compression method of this item as an index into ArchiveSource::MethodName (0 = not known: tar, folders ...)
};

// Capability flags: the UI decides which functions are available from them (for example whether a gallery needs read-ahead).
struct Capabilities {
    bool randomAccess = true;    // reading a single file does not need unpacking what comes before it
    bool solid = false;
    bool encrypted = false;      // at least one item is encrypted
    bool canModify = false;      // can add / delete (always false in M0)
};

// false means the user asked to cancel.
using ProgressFn = std::function<bool(uint64_t done, uint64_t total)>;
// false means the user gave no password.
using PasswordFn = std::function<bool(std::wstring& password)>;

class ByteSink {
public:
    virtual ~ByteSink() = default;
    virtual void Write(const void* data, size_t size) = 0;
};

class IFileSource {
public:
    virtual ~IFileSource() = default;
    virtual std::wstring FormatName() const = 0;
    virtual Capabilities Caps() const = 0;
    virtual const std::vector<Entry>& Entries() const = 0;
    // Streams the content of one item to a sink (the basis of the gallery / preview).
    virtual void ReadEntry(uint32_t index, ByteSink& sink, const ProgressFn& progress = {}) = 0;
};

}  // namespace lycan
