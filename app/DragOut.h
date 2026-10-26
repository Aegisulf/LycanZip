#pragma once
// Dragging files out of an archive into Explorer without unpacking them first.
//
// The drag carries "virtual files" (CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS): the names and sizes are known at once, so
// the cursor is never a "no drop" sign; the bytes are decompressed while Explorer copies them (a stream per file, filled by
// a background thread through a bounded buffer). Nothing is written to a temporary folder.
//
// Not every drag can be a virtual one (see PlanVirtualDrag); those keep using the temporary folder (MainWindow's
// ProvideDragItems).
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "lycan/archive_source.h"

namespace lycan_app {

struct DragEntry {
    uint32_t index = 0;        // entry of the archive
    std::wstring name;         // path below the drop folder, '\' separated (already made safe)
    uint64_t size = 0;
    uint64_t mtimeFileTime = 0;
    uint32_t attributes = 0;
    bool isDir = false;
};

struct VirtualDragRequest {
    std::filesystem::path archive;
    lycan::OpenOptions open;
    std::vector<DragEntry> entries;
    uint64_t totalBytes = 0;   // of the files (not the folders)
    // Several files of a solid archive: the entries are in the order of the archive and ONE pass over the archive feeds all of them (one
    // by one, every file would unpack the data before it again). What Explorer does not read yet waits in memory, then on disk.
    bool onePass = false;

};

// Decides whether this selection can be dragged as virtual files and fills `out`.
//   entries / indices : the archive's entries and the (sorted) entries to drag, folders included
//   stripPrefix       : same meaning as ExtractOptions::stripPrefix
//   wrapName          : when not empty, everything goes into a folder of this name (dragging the archive's root)
//   skipMacMetadata   : leave out __MACOSX, ._ files and .DS_Store
//   randomAccess      : the archive can read one file without unpacking the ones before it (solid archives cannot; with
//                       several files that would unpack the same data again and again, so such drags use the temp folder)
// Returns false when the drag should use the temporary folder instead (nothing to drag, a name Explorer cannot take
// (cFileName holds 259 characters)). Several files of an archive that cannot read one file alone (solid) set `onePass`.
bool PlanVirtualDrag(const std::vector<lycan::Entry>& entries, const std::vector<uint32_t>& indices, const std::wstring& stripPrefix,
                     const std::wstring& wrapName, bool skipMacMetadata, bool randomAccess, VirtualDragRequest& out);

// One file, by its entry and name (the picture preview drags the picture of a tile). The name is made safe like the others;
// false when it cannot be a file name Explorer takes.
bool PlanSingleFileDrag(uint32_t index, const std::wstring& name, uint64_t size, uint64_t mtimeFileTime, VirtualDragRequest& out);

// Runs the OLE drag and returns when the files were dropped or the drag was cancelled (the left mouse button must be down).
// UI thread only; call it from the event that starts the drag. Decompression runs on background threads; while the UI thread
// waits for data it keeps handling messages. Returns the DROPEFFECT.
DWORD RunVirtualDrag(VirtualDragRequest request);

}  // namespace lycan_app
