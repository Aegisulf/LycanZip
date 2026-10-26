#include "lycan/tree.h"
#include "lycan/junk.h"

#include <windows.h>

#include <algorithm>
#include <set>

namespace lycan {
namespace {

constexpr size_t kMaxDepth = 64;  // levels shown in the tree; deeper parts are folded into the last name

std::vector<std::wstring> SplitPath(const std::wstring& p) {
    std::vector<std::wstring> out;
    size_t i = 0;
    while (i <= p.size()) {
        size_t j = p.find(L'/', i);
        if (j == std::wstring::npos) j = p.size();
        std::wstring part = p.substr(i, j - i);
        if (!part.empty() && part != L".") out.push_back(std::move(part));
        i = j + 1;
    }
    return out;
}

// Natural order (numbers by value), case-insensitive
int NaturalCompare(const std::wstring& a, const std::wstring& b) {
    int r = CompareStringEx(LOCALE_NAME_USER_DEFAULT, LINGUISTIC_IGNORECASE | SORT_DIGITSASNUMBERS, a.c_str(),
                            static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), nullptr, nullptr, 0);
    return r == 0 ? 0 : r - CSTR_EQUAL;  // CSTR_LESS_THAN=1, EQUAL=2, GREATER=3 → -1/0/1
}

std::wstring Extension(const TreeNode& n) {
    if (n.isDir) return {};
    auto pos = n.name.rfind(L'.');
    return pos == std::wstring::npos || pos == 0 ? std::wstring() : n.name.substr(pos + 1);
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    return FindNLSStringEx(LOCALE_NAME_USER_DEFAULT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, hay.c_str(),
                           static_cast<int>(hay.size()), needle.c_str(), static_cast<int>(needle.size()), nullptr,
                           nullptr, nullptr, 0) >= 0;
}

}  // namespace

ArchiveTree::ArchiveTree(const std::vector<Entry>& entries, bool hideMacMetadata) {
    nodes_.emplace_back();
    nodes_[0].isDir = true;
    std::unordered_map<std::wstring, uint32_t> dirs;  // path -> folder node
    dirs[L""] = 0;

    auto addNode = [&](uint32_t parent, const std::wstring& name, bool isDir) {
        TreeNode n;
        n.id = static_cast<uint32_t>(nodes_.size());
        n.parent = parent;
        n.name = name;
        n.path = nodes_[parent].path.empty() ? name : nodes_[parent].path + L"/" + name;
        n.isDir = isDir;
        nodes_.push_back(std::move(n));
        nodes_[parent].children.push_back(nodes_.back().id);
        return nodes_.back().id;
    };
    auto dirFor = [&](uint32_t parent, const std::wstring& name) {
        std::wstring path = nodes_[parent].path.empty() ? name : nodes_[parent].path + L"/" + name;
        auto it = dirs.find(path);
        if (it != dirs.end()) return it->second;
        uint32_t id = addNode(parent, name, true);
        dirs[path] = id;
        return id;
    };

    for (const Entry& e : entries) {
        if (hideMacMetadata && IsMacMetadataPath(e.path)) continue;
        auto parts = SplitPath(e.path);
        if (parts.empty()) continue;
        // Every node stores its full path, so memory grows with the square of the depth: one 60 KB name made of
        // 30000 components would need gigabytes. Fold anything below kMaxDepth into the last folder level's name.
        if (parts.size() > kMaxDepth) {
            std::wstring rest = parts[kMaxDepth - 1];
            for (size_t i = kMaxDepth; i < parts.size(); ++i) rest += L"/" + parts[i];
            parts.resize(kMaxDepth);
            parts.back() = std::move(rest);
        }
        uint32_t cur = 0;
        for (size_t i = 0; i + 1 < parts.size(); ++i) cur = dirFor(cur, parts[i]);
        if (e.isDir) {
            uint32_t d = dirFor(cur, parts.back());
            TreeNode& n = nodes_[d];
            if (n.entryIndex == kNoEntry) {
                n.entryIndex = e.index;
                n.mtimeFileTime = e.mtimeFileTime;
                n.attributes = e.attributes;
                n.isEncrypted = e.isEncrypted;
            }
        } else {
            uint32_t f = addNode(cur, parts.back(), false);
            TreeNode& n = nodes_[f];
            n.entryIndex = e.index;
            n.size = e.size;
            n.packedSize = e.packedSize;
            n.mtimeFileTime = e.mtimeFileTime;
            n.attributes = e.attributes;
            n.isEncrypted = e.isEncrypted;
            n.hasCrc = e.hasCrc;
            n.crc = e.crc;
            n.methodIdx = e.methodIdx;
            for (uint32_t p = cur;; p = nodes_[p].parent) {
                nodes_[p].totalSize += e.size;
                nodes_[p].fileCount += 1;
                if (p == 0) break;
            }
        }
    }

    // Lookup table: folders first; for files the first one with the same path
    for (const TreeNode& n : nodes_) {
        if (n.id == 0) continue;
        if (n.isDir) byPath_[n.path] = n.id;
    }
    for (const TreeNode& n : nodes_) {
        if (n.id == 0 || n.isDir) continue;
        byPath_.emplace(n.path, n.id);
    }
}

uint32_t ArchiveTree::Find(const std::wstring& path) const {
    auto parts = SplitPath(path);
    std::wstring key;
    for (auto& p : parts) key += (key.empty() ? L"" : L"/") + p;
    if (key.empty()) return 0;
    auto it = byPath_.find(key);
    return it == byPath_.end() ? kNoNode : it->second;
}

void ArchiveTree::Sort(std::vector<uint32_t>& ids, SortKey key, bool ascending, bool dirsFirst) const {
    auto cmp = [&](uint32_t x, uint32_t y) {
        const TreeNode& a = nodes_[x];
        const TreeNode& b = nodes_[y];
        if (dirsFirst && a.isDir != b.isDir) return a.isDir;  // folders always first, whatever the sort direction
        int c = 0;
        switch (key) {
            case SortKey::Name: c = NaturalCompare(a.name, b.name); break;
            case SortKey::Path: c = NaturalCompare(a.path, b.path); break;
            case SortKey::Size: {
                uint64_t sa = a.isDir ? a.totalSize : a.size, sb = b.isDir ? b.totalSize : b.size;
                c = sa < sb ? -1 : sa > sb ? 1 : 0;
                break;
            }
            case SortKey::PackedSize: c = a.packedSize < b.packedSize ? -1 : a.packedSize > b.packedSize ? 1 : 0; break;
            case SortKey::Modified: c = a.mtimeFileTime < b.mtimeFileTime ? -1 : a.mtimeFileTime > b.mtimeFileTime ? 1 : 0; break;
            case SortKey::Type: c = NaturalCompare(Extension(a), Extension(b)); break;
            case SortKey::Ratio: {
                // packed / size in 1/10000; an item without a size or a packed size (a solid archive does not say) goes last
                auto ratio = [](const TreeNode& n) { return n.isDir || n.size == 0 || n.packedSize == 0 ? ~0ull : n.packedSize * 10000 / n.size; };
                const uint64_t ra = ratio(a), rb = ratio(b);
                c = ra < rb ? -1 : ra > rb ? 1 : 0;
                break;
            }
            case SortKey::Encrypted: c = (a.isEncrypted ? 1 : 0) - (b.isEncrypted ? 1 : 0); break;
            case SortKey::Crc: {
                const uint64_t ca = a.hasCrc ? a.crc + 1ull : 0, cb = b.hasCrc ? b.crc + 1ull : 0;   // no CRC first
                c = ca < cb ? -1 : ca > cb ? 1 : 0;
                break;
            }
            case SortKey::Method: {
                const std::wstring ma = MethodName(a.methodIdx), mb = MethodName(b.methodIdx);
                c = methodNames_.empty() ? (a.methodIdx < b.methodIdx ? -1 : a.methodIdx > b.methodIdx ? 1 : 0) : NaturalCompare(ma, mb);
                break;
            }
        }
        if (c != 0) return ascending ? c < 0 : c > 0;
        int n = NaturalCompare(a.name, b.name);  // the secondary key is always the name ascending, so the result is stable
        if (n != 0) return n < 0;
        n = NaturalCompare(a.path, b.path);  // files with the same name (in different folders) go by path
        if (n != 0) return n < 0;
        return x < y;
    };
    std::stable_sort(ids.begin(), ids.end(), cmp);
}

std::vector<uint32_t> ArchiveTree::Children(uint32_t dir, SortKey key, bool ascending) const {
    std::vector<uint32_t> out = nodes_[dir].children;
    Sort(out, key, ascending, true);
    return out;
}

void ArchiveTree::Collect(uint32_t dir, std::vector<uint32_t>& out) const {
    // Pre-order walk with an explicit stack: a hostile archive can nest folders thousands of levels deep, and
    // recursion would overflow the (1 MB) UI thread stack.
    std::vector<uint32_t> stack;
    auto pushChildren = [&](uint32_t d) {
        const auto& ch = nodes_[d].children;
        for (auto it = ch.rbegin(); it != ch.rend(); ++it) stack.push_back(*it);
    };
    pushChildren(dir);
    while (!stack.empty()) {
        const uint32_t c = stack.back();
        stack.pop_back();
        out.push_back(c);
        if (nodes_[c].isDir) pushChildren(c);
    }
}

std::vector<uint32_t> ArchiveTree::Descendants(uint32_t dir, SortKey key, bool ascending, bool includeDirs) const {
    std::vector<uint32_t> out;
    Collect(dir, out);
    if (!includeDirs) out.erase(std::remove_if(out.begin(), out.end(), [&](uint32_t id) { return nodes_[id].isDir; }), out.end());
    Sort(out, key, ascending, false);
    return out;
}

std::vector<uint32_t> ArchiveTree::Search(uint32_t dir, const std::wstring& query, SortKey key, bool ascending) const {
    std::vector<uint32_t> out;
    if (query.empty()) return out;
    std::vector<uint32_t> all;
    Collect(dir, all);
    for (uint32_t id : all)
        if (ContainsNoCase(nodes_[id].name, query)) out.push_back(id);
    Sort(out, key, ascending, false);
    return out;
}

std::vector<uint32_t> ArchiveTree::EntryIndices(const std::vector<uint32_t>& nodes) const {
    std::set<uint32_t> set;
    for (uint32_t id : nodes) {
        if (id >= nodes_.size()) continue;
        std::vector<uint32_t> stack{id};
        while (!stack.empty()) {
            uint32_t cur = stack.back();
            stack.pop_back();
            const TreeNode& n = nodes_[cur];
            if (n.entryIndex != kNoEntry) set.insert(n.entryIndex);
            if (n.isDir) stack.insert(stack.end(), n.children.begin(), n.children.end());
        }
    }
    return {set.begin(), set.end()};
}

std::wstring ArchiveTree::ParentPath(uint32_t node) const {
    if (node == 0 || node >= nodes_.size()) return {};
    return nodes_[nodes_[node].parent].path;
}

}  // namespace lycan
