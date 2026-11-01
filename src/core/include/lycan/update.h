#pragma once
// Changing an existing archive in place (zip / 7z / tar): add files and folders, delete, rename or move, make a
// folder, replace the content of a file.
//
// Everything that is not touched is copied through without being unpacked (same method, CRC, times, encryption), so
// a change costs about one read and one write of the archive. The result is built in a temporary file next to the
// archive and swapped in when it is complete; on failure or cancel the archive is left exactly as it was.
// Not supported (UpdateArchive throws): volumes, double-layer formats (tar.gz ...), and formats 7z.dll cannot write.
// ArchiveSource::Caps().canModify says whether an opened archive can be changed.
//
// A solid 7z block that loses an item (delete, replace) is recompressed as a whole: EstimateUpdate tells how much.
#include "lycan/message.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "lycan/compress.h"

namespace lycan {

struct UpdateOp {
    enum class Kind {
        Add,      // path = the folder to add into ("" = top level); inputs = files and folders on disk
        Delete,   // path = the item; a folder takes everything below it
        Rename,   // path -> newPath (full paths; also moves an item to another folder); a folder takes everything below it
        MakeDir,  // path = the new folder
        Replace,  // path = an existing file; file = the new content
    };
    Kind kind = Kind::Add;
    std::wstring path;                          // archive paths use '/' or '\\'
    std::wstring newPath;
    std::vector<std::filesystem::path> inputs;
    std::filesystem::path file;

    static UpdateOp Add(std::wstring intoFolder, std::vector<std::filesystem::path> inputs);
    static UpdateOp Delete(std::wstring path);
    static UpdateOp Rename(std::wstring from, std::wstring to);
    static UpdateOp MakeDir(std::wstring path);
    static UpdateOp Replace(std::wstring path, std::filesystem::path file);
};

// What an Add does with an item of the archive that has the same name as a file on the disk (and with the rest):
//   AddReplace  : the file on the disk always replaces it; new files are added; the rest is left alone (the default)
//   UpdateAdd   : replaced only when the file on the disk is newer; new files are added ("update and add")
//   Freshen     : replaced only when the file on the disk is newer; new files are NOT added ("freshen existing")
//   Synchronize : the archive becomes what the folder is: new files are added, a file that differs (another time or size) is replaced, and what is in the
//                 archive below the folders that were added but no longer on the disk is deleted (nothing is deleted when a file could not be read)
enum class UpdateMode { AddReplace, UpdateAdd, Freshen, Synchronize };

struct UpdateOptions {
    // Applies to the NEW data only (profile, level, method, threads, zip encryption, skipThumbsDb ...); `type` is
    // ignored (the archive decides). `password` is the archive's password: needed to open an archive whose headers are
    // encrypted, and new items are encrypted with it. An archive that has encrypted items cannot be changed without it.
    CompressOptions compress;
    bool keepBackup = false;         // leave the old archive next to the new one as "<name>.bak"
    bool replaceSameName = true;     // Add (mode AddReplace): an item that already exists is replaced; false = fail
    UpdateMode mode = UpdateMode::AddReplace;
    unsigned nameCodepage = 0;       // see OpenOptions::nameCodepage
    // "Upgrade the archive": an archive whose names have no charset mark (old zip / tar) is written again even without operations,
    // every name as it was decoded (nameCodepage), now in UTF-8 and marked; the data is copied as it is.
    bool rewriteNames = false;
    bool verify = false;             // test the new archive before it replaces the old one; it fails (nothing changes) when the test does
    ProgressFn progress;             // false = cancel
};

struct UpdateReport {
    uint32_t added = 0, replaced = 0, deleted = 0, renamed = 0, dirsCreated = 0;  // counts of items (folders below a folder included)
    uint32_t unchanged = 0;          // files left as they were because the file on the disk is not newer (modes UpdateAdd, Freshen) or the same (Synchronize)
    uint32_t notAdded = 0;           // new files that were not added (mode Freshen)
    std::vector<std::wstring> deletedPaths;   // what Synchronize deletes (for the question before it is done)
    uint64_t bytesIn = 0;            // size of the new data
    uint64_t archiveBefore = 0, archiveAfter = 0;
    uint32_t thumbsDbSkipped = 0;
    uint32_t storedFiles = 0;        // new files put in as they are (CompressOptions::storeCompressed)
    std::vector<Message> skipped, errors, warnings;
    bool changed = false;            // false: the operations changed nothing and the archive was not rewritten
    bool ok() const { return errors.empty(); }
};

struct UpdateCost {
    uint64_t archiveBytes = 0;       // the archive is rewritten: read and written once
    uint64_t newBytes = 0;           // new data that will be compressed
    uint64_t recompressBytes = 0;    // old data that has to be unpacked and compressed again (solid 7z blocks that lose an item)
};

// What the operations would cost; the archive is not changed. Throws what UpdateArchive throws for a bad operation.
UpdateCost EstimateUpdate(const std::filesystem::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& options = {});

// What the operations would do (the counts and the names of what Synchronize deletes); the archive is not changed. Throws what UpdateArchive throws for a bad operation.
UpdateReport PreviewUpdate(const std::filesystem::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& options = {});

// Applies the operations in order. A bad operation (missing item, name already taken, unsupported format ...) throws
// lycan::Error before anything is written. Files that cannot be read are listed in errors and left out.
UpdateReport UpdateArchive(const std::filesystem::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& options = {});

}  // namespace lycan
