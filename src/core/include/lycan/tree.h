#pragma once
// Folder tree of an archive: puts the flat list of Entry into the structure that "one level browsing / tree / search" needs.
// Pure logic, no UI. An archive often has no explicit folder items (only a/b/c.txt); the implied folders are added here.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "lycan/vfs.h"

namespace lycan {

constexpr uint32_t kNoNode = 0xFFFFFFFFu;
constexpr uint32_t kNoEntry = 0xFFFFFFFFu;

struct TreeNode {
    uint32_t id = 0;
    uint32_t parent = kNoNode;
    std::wstring name;            // last part of the name
    std::wstring path;            // full path ('/' separated, no trailing '/'); the root is an empty string
    uint32_t entryIndex = kNoEntry;  // the matching Entry; kNoEntry for implied folders and the root
    bool isDir = false;
    bool isEncrypted = false;
    uint64_t size = 0;            // file: original size
    uint64_t packedSize = 0;
    uint64_t mtimeFileTime = 0;
    uint32_t attributes = 0;
    bool hasCrc = false;          // file: the archive stores its CRC-32
    uint32_t crc = 0;
    uint16_t methodIdx = 0;       // file: the method of the item (the names: ArchiveTree::MethodName)
    uint64_t totalSize = 0;       // folder: total original size of all files below it
    uint32_t fileCount = 0;       // folder: number of all files below it (recursive)
    std::vector<uint32_t> children;
};

// The values of Ratio .. Method are the Tag of the column headers of the details view (MainWindow.xaml)
enum class SortKey { Name, Size, PackedSize, Modified, Type, Path, Ratio, Encrypted, Crc, Method };

class ArchiveTree {
public:
    // hideMacMetadata: leave out the clutter macOS adds (junk.h), so neither browsing nor the previews see it
    explicit ArchiveTree(const std::vector<Entry>& entries, bool hideMacMetadata = false);

    uint32_t Root() const { return 0; }
    const TreeNode& Node(uint32_t id) const { return nodes_[id]; }
    size_t NodeCount() const { return nodes_.size(); }
    // Finds a folder or file by its full path (a trailing '/' is allowed); kNoNode if not found
    uint32_t Find(const std::wstring& path) const;

    // The direct children of a folder. Folders are always before files (not changed by the sort direction); names are compared case-insensitively, numbers by value (natural order).
    std::vector<uint32_t> Children(uint32_t dir, SortKey key = SortKey::Name, bool ascending = true) const;
    // All descendants of a folder (recursive), for "list everything"; dir = Root() is the whole archive. With includeDirs = false only files are listed ("list everything" does not show folders).
    // Name sorting compares the file name only (not the path); to sort by path use SortKey::Path.
    std::vector<uint32_t> Descendants(uint32_t dir, SortKey key = SortKey::Name, bool ascending = true, bool includeDirs = true) const;
    // Descendants whose name contains query (case-insensitive). An empty query = an empty result.
    std::vector<uint32_t> Search(uint32_t dir, const std::wstring& query, SortKey key = SortKey::Name,
                                 bool ascending = true) const;

    // Turns the selected nodes into Entry indices for extraction: a folder with everything below it (including the folder item itself). The result is sorted without duplicates.
    std::vector<uint32_t> EntryIndices(const std::vector<uint32_t>& nodes) const;
    // The folder path of a node (without itself; an item directly under the root gives an empty string)
    std::wstring ParentPath(uint32_t node) const;

    // The names of the methods (ArchiveSource::MethodNames): needed to sort by method; without them that sort uses the order of first appearance
    void SetMethodNames(std::vector<std::wstring> names) { methodNames_ = std::move(names); }
    std::wstring MethodName(uint16_t idx) const { return idx < methodNames_.size() ? methodNames_[idx] : std::wstring(); }

private:
    // dirsFirst = false (list everything / search): folders are not put first, and the name key compares the full path
    void Sort(std::vector<uint32_t>& ids, SortKey key, bool ascending, bool dirsFirst) const;
    void Collect(uint32_t dir, std::vector<uint32_t>& out) const;

    std::vector<TreeNode> nodes_;
    std::vector<std::wstring> methodNames_;
    std::unordered_map<std::wstring, uint32_t> byPath_;  // folders and files (for several files with the same path the first one)
};

}  // namespace lycan
