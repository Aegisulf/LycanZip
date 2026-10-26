// libFuzzer target: SanitizeEntryPath invariants and ArchiveTree operations on arbitrary names.
// The input is read as UTF-16LE text; every line (split on U+0000 or newline) becomes an archive entry.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <string>
#include <vector>

#include "lycan/security.h"
#include "lycan/tree.h"

using namespace lycan;

static void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "INVARIANT VIOLATED: %s\n", what);
        std::abort();
    }
}

static bool Reserved(std::wstring n) {
    for (auto& c : n) c = static_cast<wchar_t>(towupper(c));
    auto dot = n.find(L'.');
    if (dot != std::wstring::npos) n.resize(dot);
    while (!n.empty() && n.back() == L' ') n.pop_back();
    if (n == L"CON" || n == L"PRN" || n == L"AUX" || n == L"NUL") return true;
    if (n.size() == 4 && (n.compare(0, 3, L"COM") == 0 || n.compare(0, 3, L"LPT") == 0) && n[3] >= L'1' && n[3] <= L'9') return true;
    return false;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::wstring text(size / 2, L'\0');
    for (size_t i = 0; i < size / 2; ++i) text[i] = static_cast<wchar_t>(data[2 * i] | (data[2 * i + 1] << 8));

    // 1. one name: the sanitiser must either reject it or produce a safe relative path
    SafePath sp = SanitizeEntryPath(text);
    if (!sp.rejected) {
        Check(!sp.relative.empty(), "accepted path is empty");
        Check(sp.relative.is_relative() && !sp.relative.has_root_name() && !sp.relative.has_root_directory(), "accepted path is not relative");
        for (auto& part : sp.relative) {
            std::wstring s = part.wstring();
            Check(!s.empty() && s != L"." && s != L"..", "accepted path has an empty / dot / dotdot component");
            for (wchar_t c : s) {
                Check(c >= 32 && c != L'<' && c != L'>' && c != L':' && c != L'"' && c != L'|' && c != L'?' && c != L'*' && c != L'\\' && c != L'/',
                      "accepted component has an illegal character");
            }
            Check(s.back() != L'.' && s.back() != L' ', "accepted component ends with a dot or space");
            Check(!Reserved(s), "accepted component is a reserved device name");
        }
    }

    // 2. a tree built from many names
    std::vector<Entry> entries;
    std::wstring cur;
    uint32_t flags = 0;
    for (wchar_t c : text) {
        if (c == 0 || c == L'\n') {
            Entry e;
            e.index = static_cast<uint32_t>(entries.size());
            e.path = cur;
            e.isDir = (flags++ % 3) == 0;
            e.size = cur.size();
            entries.push_back(e);
            cur.clear();
            if (entries.size() > 2000) break;
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) {
        Entry e;
        e.index = static_cast<uint32_t>(entries.size());
        e.path = cur;
        entries.push_back(e);
    }
    ArchiveTree tree(entries);
    for (uint32_t id = 0; id < tree.NodeCount(); ++id) {
        const TreeNode& n = tree.Node(id);
        (void)tree.ParentPath(id);
        if (n.isDir) {
            for (SortKey k : {SortKey::Name, SortKey::Size, SortKey::Modified, SortKey::Type, SortKey::Path}) {
                (void)tree.Children(id, k, false);
                if (id == tree.Root()) {
                    (void)tree.Descendants(id, k, true, false);
                    (void)tree.Search(id, L"a", k, true);
                }
            }
        }
        (void)tree.Find(n.path);
    }
    std::vector<uint32_t> all;
    for (uint32_t id = 0; id < tree.NodeCount() && id < 50; ++id) all.push_back(id);
    for (uint32_t i : tree.EntryIndices(all)) Check(i < entries.size(), "EntryIndices returned an index out of range");
    return 0;
}
