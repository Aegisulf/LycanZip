#pragma once
// Hash values (checksums) of files and of the files inside an archive: CRC-32, MD5, SHA-1, SHA-256, SHA-384, SHA-512.
// A file is read once, however many algorithms are asked for. MD5 and the SHA family come from Windows (CNG, hardware
// accelerated where the CPU has it); CRC-32 is our own. A hash value only tells that a file is the same as the one the
// value was made from; whether the value itself can be trusted depends on where it came from. MD5 and SHA-1 are fine to
// notice a damaged download, but they do not keep anyone from changing a file on purpose.
#include "lycan/message.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "lycan/archive_source.h"

namespace lycan {

// A set of algorithms (bit mask). The order of the results is the order of the bits.
enum HashAlgo : uint32_t {
    kHashCrc32 = 1, kHashMd5 = 2, kHashSha1 = 4, kHashSha256 = 8, kHashSha384 = 16, kHashSha512 = 32,
};
inline constexpr uint32_t kHashAll = 63;
inline constexpr uint32_t kHashDefault = kHashCrc32 | kHashMd5 | kHashSha1 | kHashSha256;

const wchar_t* HashAlgoName(uint32_t algo);                // one bit: "CRC-32", "MD5", "SHA-1", "SHA-256" ...
size_t HashHexLength(uint32_t algo);                       // one bit: 8, 32, 40, 64, 96, 128
std::vector<uint32_t> HashAlgoList(uint32_t mask);         // the set bits, lowest first
uint32_t NormalizeHashAlgos(uint32_t mask);                // unknown bits dropped; nothing left = kHashDefault

// Feeds data to several algorithms at once.
class MultiHasher {
public:
    explicit MultiHasher(uint32_t algos);
    ~MultiHasher();
    MultiHasher(const MultiHasher&) = delete;
    MultiHasher& operator=(const MultiHasher&) = delete;
    void Update(const void* data, size_t size);
    std::vector<std::string> Finish();                     // lower case hex, one per algorithm in the order of the bits
private:
    struct Impl;
    Impl* impl_;
};

// Hex of the hash of a block of memory (tests, small things)
std::string HashHex(uint32_t algo, const void* data, size_t size);

struct HashFileResult {
    std::wstring path;                  // the file on disk, or the path inside the archive ('/')
    uint64_t size = 0;
    std::vector<std::string> hex;       // one per algorithm of the report (empty when it failed)
    Message error;                 // empty = fine
    bool ok() const { return error.empty(); }
};

struct HashReport {
    uint32_t algos = kHashDefault;
    std::vector<HashFileResult> files;
    uint64_t bytes = 0;                 // what was read
    uint32_t failed = 0;
};

struct HashOptions {
    uint32_t algos = kHashDefault;
    ProgressFn progress;                // false = cancel (throws lycan::Error with cancelled())
    PasswordFn password;                // archive entries
};

// Files on disk, one after the other. A file that cannot be read gets an error and the others go on.
HashReport HashFiles(const std::vector<std::filesystem::path>& files, const HashOptions& options = {});

// Files inside an archive (one pass over the archive, solid ones too; nothing is written to disk). `indices` are entry
// indices; folders in the list are ignored.
HashReport HashEntries(ArchiveSource& src, const std::vector<uint32_t>& indices, const HashOptions& options = {});

// The lines of a checksum file for ONE algorithm ("<hex> *<name>", UTF-8, one line per file that has a value). Names of files on
// disk are made relative to `base` when that is not empty (and '/' is the separator); names inside an archive stay as they are.
std::string HashReportText(const HashReport& report, uint32_t algo, const std::filesystem::path& base = {});
// All algorithms of the report, one line each: "SHA256 (<name>) = <hex>" (the BSD form; ParseSumLine reads it back).
std::string HashReportTextAll(const HashReport& report, const std::filesystem::path& base = {});

// ---- comparing and checksum files --------------------------------------------------------------------------------------

// Which algorithms give a value of that many hex digits (8 = CRC-32, 32 = MD5, 40 = SHA-1, 64 = SHA-256 ...); 0 = none.
uint32_t HashAlgosForLength(size_t hexDigits);

// What the user pasted, reduced to its hex digits in lower case: spaces, tabs, dashes, colons and quotes are dropped and a
// leading "0x" is ignored. Anything else (a letter that is not a hex digit ...) gives "".
std::string NormalizeHashText(std::wstring_view text);

// One line of a checksum file: "<hex> *<name>" or "<hex>  <name>" (sha256sum, md5sum) or "SHA256 (<name>) = <hex>" (BSD).
struct SumLine {
    std::string hex;                    // lower case
    std::wstring name;
    uint32_t algo = 0;                  // set by the BSD form ("SHA256 (..)"); 0 = by the length of the value
};
bool ParseSumLine(std::string_view utf8Line, SumLine& out);
std::string FormatSumLine(const std::string& hex, const std::wstring& name);   // "<hex> *<name>" (binary mode), UTF-8, no newline

}  // namespace lycan
