// Folder tree model (one level browsing / list everything / search / selection to extraction indices) and prefix stripping at extraction
#include "no_dialogs.h"
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/extract.h"
#include "lycan/tree.h"

using namespace lycan;
namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static Entry E(uint32_t idx, const wchar_t* path, bool dir, uint64_t size = 0, uint64_t packed = 0, uint64_t mtime = 0) {
    Entry e;
    e.index = idx;
    e.path = path;
    e.isDir = dir;
    e.size = size;
    e.packedSize = packed;
    e.mtimeFileTime = mtime;
    return e;
}
static std::vector<std::wstring> Names(const ArchiveTree& t, const std::vector<uint32_t>& ids) {
    std::vector<std::wstring> r;
    for (auto id : ids) r.push_back(t.Node(id).name);
    return r;
}
static void Write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static std::string Read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

int main() {
    // An archive without explicit folder items (very common): a/b/c.txt, a/x.txt, readme.txt; "a/b/" has an explicit item
    std::vector<Entry> es = {
        E(0, L"readme.txt", false, 10, 5, 300),
        E(1, L"a/x.txt", false, 100, 40, 200),
        E(2, L"a/b/", true),
        E(3, L"a/b/c.txt", false, 1000, 300, 100),
        E(4, L"a/b/file10.txt", false, 5, 5, 50),
        E(5, L"a/b/file2.txt", false, 7, 7, 60),
        E(6, L"empty/", true),
        E(7, L"Z.TXT", false, 1, 1, 10),
    };
    ArchiveTree t(es);

    // The implied folder a is added; under the root: folders a, empty first, then the files
    auto root = t.Children(t.Root());
    EXPECT((Names(t, root) == std::vector<std::wstring>{L"a", L"empty", L"readme.txt", L"Z.TXT"}));  // case-insensitive
    uint32_t a = t.Find(L"a");
    EXPECT(a != kNoNode && t.Node(a).isDir && t.Node(a).entryIndex == kNoEntry);  // an implied folder has no Entry
    uint32_t b = t.Find(L"a/b/");                                                   // a trailing '/' is accepted
    EXPECT(b != kNoNode && t.Node(b).entryIndex == 2);                              // an explicit folder item keeps its index
    EXPECT(t.Node(a).fileCount == 4 && t.Node(a).totalSize == 100 + 1000 + 5 + 7);  // recursive statistics
    EXPECT(t.Find(L"nope") == kNoNode && t.Find(L"") == t.Root());

    // Natural order: file2 before file10
    EXPECT((Names(t, t.Children(b)) == std::vector<std::wstring>{L"c.txt", L"file2.txt", L"file10.txt"}));
    // Sort key: size descending (folders still first)
    EXPECT((Names(t, t.Children(t.Root(), SortKey::Size, false)) == std::vector<std::wstring>{L"a", L"empty", L"readme.txt", L"Z.TXT"}));
    EXPECT((Names(t, t.Children(b, SortKey::Size, false)) == std::vector<std::wstring>{L"c.txt", L"file2.txt", L"file10.txt"}));
    EXPECT((Names(t, t.Children(b, SortKey::Modified, true)) == std::vector<std::wstring>{L"file10.txt", L"file2.txt", L"c.txt"}));

    // List everything: 9 nodes in the whole archive (3 folders a, a/b, empty + 6 files)
    EXPECT(t.Descendants(t.Root()).size() == 3 + 6);
    EXPECT(t.Descendants(b).size() == 3);
    // List everything has no folders; name sorting compares the file name only, path sorting goes by path
    EXPECT(t.Descendants(t.Root(), SortKey::Name, true, false).size() == 6);
    EXPECT((Names(t, t.Descendants(t.Root(), SortKey::Name, true, false)) == std::vector<std::wstring>{L"c.txt", L"file2.txt", L"file10.txt", L"readme.txt", L"x.txt", L"Z.TXT"}));
    EXPECT((Names(t, t.Descendants(t.Root(), SortKey::Path, true, false)) == std::vector<std::wstring>{L"c.txt", L"file2.txt", L"file10.txt", L"x.txt", L"readme.txt", L"Z.TXT"}));
    EXPECT((Names(t, t.Descendants(t.Root(), SortKey::Name, false, false)).front() == L"Z.TXT"));

    // Search: case-insensitive, includes subfolders
    EXPECT(Names(t, t.Search(t.Root(), L"TXT")).size() == 6);
    EXPECT((Names(t, t.Search(a, L"file")) == std::vector<std::wstring>{L"file2.txt", L"file10.txt"}));
    EXPECT(t.Search(t.Root(), L"").empty() && t.Search(t.Root(), L"zzz").empty());

    // Selection -> extraction indices: a folder with all its content; an implied folder has no index itself
    auto idx = t.EntryIndices({a});
    EXPECT((idx == std::vector<uint32_t>{1, 2, 3, 4, 5}));
    EXPECT((t.EntryIndices({t.Find(L"readme.txt"), t.Find(L"a/x.txt"), a}) == std::vector<uint32_t>{0, 1, 2, 3, 4, 5}));
    EXPECT(t.ParentPath(b) == L"a" && t.ParentPath(a).empty() && t.ParentPath(t.Find(L"a/b/c.txt")) == L"a/b");

    // Odd paths: repeated slashes, "./", the same file name twice, a file and a folder with the same name
    std::vector<Entry> odd = {E(0, L"./d//f.txt", false, 1), E(1, L"d/f.txt", false, 2), E(2, L"d", false, 3), E(3, L"", false)};
    ArchiveTree o(odd);
    EXPECT(o.Node(o.Find(L"d")).isDir);  // folders first
    EXPECT(o.Children(o.Find(L"d")).size() == 2);  // both files of the same name are kept
    EXPECT(o.Children(o.Root()).size() == 2);      // folder d + file d

    // Empty archive
    ArchiveTree empty({});
    EXPECT(empty.Children(empty.Root()).empty() && empty.NodeCount() == 1);

    // ---- The columns ratio / encrypted / CRC / method: the data of the items reach the nodes, and each one sorts ----
    {
        std::vector<Entry> es = {E(0, L"a.txt", false, 1000, 500), E(1, L"b.txt", false, 1000, 100), E(2, L"c.txt", false, 1000, 0), E(3, L"d.txt", false, 0, 0), E(4, L"e.txt", false, 100, 90)};
        es[0].isEncrypted = true;
        es[0].hasCrc = true; es[0].crc = 0x7DCBF9A2; es[0].methodIdx = 2;
        es[1].hasCrc = true; es[1].crc = 0x00000010; es[1].methodIdx = 1;
        es[4].hasCrc = true; es[4].crc = 0xFFFFFFFF; es[4].methodIdx = 1;
        ArchiveTree t(es);
        t.SetMethodNames({L"", L"Deflate", L"LZMA2:24"});
        uint32_t na = t.Find(L"a.txt");
        EXPECT(t.Node(na).hasCrc && t.Node(na).crc == 0x7DCBF9A2 && t.Node(na).methodIdx == 2 && t.Node(na).isEncrypted);
        EXPECT(t.MethodName(2) == L"LZMA2:24" && t.MethodName(0).empty() && t.MethodName(99).empty());
        // ratio: b (10%), a (50%), e (90%); c has no packed size and d no size: they go last, by name
        EXPECT((Names(t, t.Children(t.Root(), SortKey::Ratio, true)) == std::vector<std::wstring>{L"b.txt", L"a.txt", L"e.txt", L"c.txt", L"d.txt"}));
        EXPECT((Names(t, t.Children(t.Root(), SortKey::Ratio, false)) == std::vector<std::wstring>{L"c.txt", L"d.txt", L"e.txt", L"a.txt", L"b.txt"}));
        // encrypted: the plain ones first, then the encrypted one
        EXPECT(Names(t, t.Children(t.Root(), SortKey::Encrypted, true)).back() == L"a.txt");
        EXPECT(Names(t, t.Children(t.Root(), SortKey::Encrypted, false)).front() == L"a.txt");
        // CRC: no CRC first (by name), then 00000010, 7DCBF9A2, FFFFFFFF
        EXPECT((Names(t, t.Children(t.Root(), SortKey::Crc, true)) == std::vector<std::wstring>{L"c.txt", L"d.txt", L"b.txt", L"a.txt", L"e.txt"}));
        // method: none, then Deflate (b, e), then LZMA2:24 (a)
        EXPECT((Names(t, t.Children(t.Root(), SortKey::Method, true)) == std::vector<std::wstring>{L"c.txt", L"d.txt", L"b.txt", L"e.txt", L"a.txt"}));
    }

    // ---- Prefix stripping at extraction: items selected in a subfolder go straight into the target folder ----
    fs::path root2 = fs::temp_directory_path() / (L"lycan_tree_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root2);
    Write(root2 / "src" / "top.txt", "top");
    Write(root2 / "src" / "sub" / "one.txt", "one");
    Write(root2 / "src" / "sub" / "deeper" / "two.txt", "two");
    CreateArchive(root2 / "t.zip", {root2 / "src"}, {});
    auto src = ArchiveSource::Open(root2 / "t.zip");
    ArchiveTree at(src->Entries());
    uint32_t sub = at.Find(L"src/sub");
    EXPECT(sub != kNoNode);

    ExtractOptions eo;
    eo.stripPrefix = L"src/";
    eo.overwrite = OverwriteMode::Overwrite;
    auto rep = ExtractTo(*src, root2 / "out1", eo, at.EntryIndices({sub}));
    EXPECT(rep.ok());
    EXPECT(Read(root2 / "out1" / "sub" / "one.txt") == "one" && Read(root2 / "out1" / "sub" / "deeper" / "two.txt") == "two");
    EXPECT(!fs::exists(root2 / "out1" / "src"));

    eo.stripPrefix = L"src/sub/";  // select deeper and one.txt inside sub
    rep = ExtractTo(*src, root2 / "out2", eo, at.EntryIndices({at.Find(L"src/sub/one.txt"), at.Find(L"src/sub/deeper")}));
    EXPECT(rep.ok());
    EXPECT(Read(root2 / "out2" / "one.txt") == "one" && Read(root2 / "out2" / "deeper" / "two.txt") == "two");
    EXPECT(!fs::exists(root2 / "out2" / "sub") && !fs::exists(root2 / "out2" / "top.txt"));

    // Select "the prefix folder itself": only that one item is skipped, the content is still extracted
    rep = ExtractTo(*src, root2 / "out3", eo, at.EntryIndices({sub}));
    EXPECT(rep.ok() && Read(root2 / "out3" / "one.txt") == "one");

    // Without a prefix nothing changes (including smart extraction)
    eo.stripPrefix.clear();
    rep = ExtractTo(*src, root2 / "out4", eo, {});
    EXPECT(rep.ok() && Read(root2 / "out4" / "src" / "sub" / "one.txt") == "one");

    src.reset();
    std::error_code ec;
    fs::remove_all(root2, ec);

    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    else std::printf("tree: all passed\n");
    return failures ? 1 : 0;
}
