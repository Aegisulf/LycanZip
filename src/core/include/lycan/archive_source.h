#pragma once
#include <filesystem>
#include <functional>
#include <memory>

#include "lycan/vfs.h"

namespace lycan {

struct NameEncodingInfo {
    bool applicable = false;     // this archive has names without an encoding mark (ZIP / TAR with non-ASCII bytes)
    unsigned codepage = 0;       // the code page in use
    bool autoDetected = false;   // true = found by the detection; false = chosen by the user
    bool ambiguous = false;      // the detection is not sure (for example Big5 and GBK score close); the user should confirm
};

struct OpenOptions {
    unsigned nameCodepage = 0;  // 0 = detect the file name encoding automatically; otherwise force this code page (950, 936, 932, 949, 65001 ...)
    PasswordFn password;  // may be empty
    ProgressFn progress;  // progress for double-layer formats (tar.gz etc.) when the outer layer has to be unpacked first; false cancels
    std::filesystem::path tempDir;  // where the scratch file of a double-layer format (tar.gz) goes; empty = the system temp folder
};

// What the archive says about itself (the handler's own properties; each one may be missing)
struct ArchiveDetails {
    std::wstring method;           // e.g. "LZMA2:24 BCJ", "Deflate"
    std::wstring comment;          // the archive comment (zip)
    uint64_t physicalSize = 0;     // the size of the archive data (without what is in front of it, e.g. an SFX stub); 0 = not told
    uint64_t headersSize = 0;
    uint32_t blocks = 0;           // 7z: number of solid blocks (folders); 0 = not told
    uint32_t volumes = 0;          // number of volumes (1 = not split)
};

// Path of the 7z.dll that is really loaded (shown on the About page); throws lycan::Error when loading fails.
std::filesystem::path SevenZipDllPath();

// The base name after the archive extension is removed: foo.tar.gz / foo.tgz -> foo, foo.7z.001 -> foo, foo.part1.rar -> foo
std::wstring ArchiveBaseName(const std::filesystem::path& file);

class ArchiveSource final : public IFileSource {
public:
    // Throws lycan::Error on failure.
    static std::unique_ptr<ArchiveSource> Open(const std::filesystem::path& file,
                                               const OpenOptions& options = {});
    ~ArchiveSource() override;

    std::wstring FormatName() const override;
    Capabilities Caps() const override;
    const std::vector<Entry>& Entries() const override;
    void ReadEntry(uint32_t index, ByteSink& sink, const ProgressFn& progress = {}) override;

    // Several items in ONE pass over the archive. Reading them one by one from a solid archive would unpack the same data again and again
    // (the 100th item needs the 99 before it). The items come in the order of the archive, not in the order of `indices`.
    //   begin(index): the sink of the item (nullptr = this one is not wanted); end(index, ok): the item is done (ok = false: its data is damaged).
    // Both are called on the thread that calls this function; a sink may block (that holds the pass back). Throws lycan::Error; progress
    // returning false ends the pass with E_ABORT.
    void ReadEntries(const std::vector<uint32_t>& indices, const std::function<ByteSink*(uint32_t)>& begin,
                     const std::function<void(uint32_t, bool)>& end, const ProgressFn& progress = {});

    const std::filesystem::path& File() const;

    // The properties of the archive as a whole (see ArchiveDetails)
    ArchiveDetails Details() const;

    // What an item says about itself besides the columns of the list (for the Properties window): read from the archive when asked
    struct EntryTech {
        uint64_t ctimeFileTime = 0, atimeFileTime = 0;   // created / accessed (0 = the format has none)
        std::wstring method, comment, hostOs, characteristics, linkTarget;
        bool hasUnixMode = false;
        uint32_t unixMode = 0;                           // the permission bits (0644 ...)
        bool hasOffset = false;
        uint64_t offset = 0;                             // where the item starts in the archive file (zip)
        bool hasBlock = false;
        uint32_t block = 0;                              // the solid block (7z)
    };
    EntryTech EntryTechInfo(uint32_t index) const;

    // The compression method of an item ("Deflate", "LZMA2:24"; empty = not known): Entry::methodIdx is an index into this list (the list is short)
    std::wstring MethodName(uint16_t index) const;
    std::vector<std::wstring> MethodNames() const;

    // Verifies the password (test-reads the smallest encrypted file). Returns true when there is no encrypted file. The call blocks; use it on a background thread.
    bool CheckPassword(const std::wstring& password);

    // File name encoding: the UI can show the detection result and let the user switch it, which updates Entries() right away.
    NameEncodingInfo NameEncoding() const;
    void SetNameCodepage(unsigned codepage);
    // Internal implementation used by extract.cpp (pimpl)
    struct Impl;
    Impl& impl() { return *impl_; }

private:
    ArchiveSource();
    std::unique_ptr<Impl> impl_;
};

}  // namespace lycan
