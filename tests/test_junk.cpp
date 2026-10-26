// The clutter macOS leaves in zip files (__MACOSX, ._ files, .DS_Store) and Windows' Thumbs.db: what counts as junk, the
// tree that hides it, extraction that leaves it out (including the single-root decision), and compression that skips Thumbs.db.
#include "no_dialogs.h"
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/extract.h"
#include "lycan/junk.h"
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

static void Write(const fs::path& p, const char* text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << text;
}

int main() {
    // ---- what is junk ----
    EXPECT(IsMacMetadataPath(L"__MACOSX/photos/._a.jpg") && IsMacMetadataPath(L"__MACOSX") && IsMacMetadataPath(L"__macosx/x") &&
           IsMacMetadataPath(L"photos/._a.jpg") && IsMacMetadataPath(L"._a.jpg") && IsMacMetadataPath(L"a\\b\\._c") &&
           IsMacMetadataPath(L"dir/.DS_Store") && IsMacMetadataPath(L".ds_store"));
    EXPECT(!IsMacMetadataPath(L"photos/a.jpg") && !IsMacMetadataPath(L"deep/__MACOSX/x.txt") &&  // only a top-level __MACOSX
           !IsMacMetadataPath(L"a/._") && !IsMacMetadataPath(L"a/.DS_Store.txt") && !IsMacMetadataPath(L"my._file") && !IsMacMetadataPath(L""));
    EXPECT(IsThumbsDbName(L"Thumbs.db") && IsThumbsDbName(L"THUMBS.DB") && !IsThumbsDbName(L"thumbs.dbx") && !IsThumbsDbName(L"mythumbs.db"));

    // ---- the tree ----
    std::vector<Entry> es;
    auto add = [&](const wchar_t* path, bool dir = false) {
        Entry e;
        e.index = static_cast<uint32_t>(es.size());
        e.path = path;
        e.isDir = dir;
        e.size = dir ? 0 : 10;
        es.push_back(e);
    };
    add(L"proj", true);
    add(L"proj/a.jpg");
    add(L"proj/._a.jpg");
    add(L"proj/.DS_Store");
    add(L"__MACOSX", true);
    add(L"__MACOSX/proj", true);
    add(L"__MACOSX/proj/._a.jpg");
    {
        ArchiveTree all(es), clean(es, true);
        EXPECT(all.Children(all.Root()).size() == 2);      // proj and __MACOSX
        EXPECT(clean.Children(clean.Root()).size() == 1);  // proj only
        EXPECT(clean.Find(L"__MACOSX") == kNoNode && clean.Find(L"proj/._a.jpg") == kNoNode && clean.Find(L"proj/a.jpg") != kNoNode);
        EXPECT(clean.Descendants(clean.Root(), SortKey::Path, true, false).size() == 1);
        EXPECT(clean.Node(clean.Find(L"proj")).fileCount == 1);
    }

    // ---- a real zip made the way a Mac makes it ----
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path root = fs::path(tmp) / (L"lycan_test_junk_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    Write(root / L"src" / L"proj" / L"a.txt", "hello");
    Write(root / L"src" / L"proj" / L"._a.txt", "appledouble");
    Write(root / L"src" / L"proj" / L".DS_Store", "x");
    Write(root / L"src" / L"__MACOSX" / L"proj" / L"._a.txt", "appledouble");
    const fs::path zip = root / L"mac.zip";
    {
        CompressOptions co;
        co.type = ArchiveType::Zip;
        auto rep = CreateArchive(zip, {root / L"src" / L"proj", root / L"src" / L"__MACOSX"}, co);
        EXPECT(rep.ok() && rep.thumbsDbSkipped == 0);
    }
    auto count = [](const fs::path& dir) {
        size_t n = 0;
        std::error_code e;
        for (auto it = fs::recursive_directory_iterator(dir, e); !e && it != fs::recursive_directory_iterator(); it.increment(e))
            if (it->is_regular_file()) ++n;
        return n;
    };

    {   // as it is: two roots, so the smart extraction wraps everything in a folder named after the archive
        auto src = ArchiveSource::Open(zip);
        ExtractOptions eo;
        eo.smart = true;
        eo.overwrite = OverwriteMode::Overwrite;
        auto rep = ExtractTo(*src, root / L"keep", eo);
        EXPECT(rep.ok() && fs::exists(root / L"keep" / L"mac" / L"__MACOSX") && rep.macMetadataSkipped == 0);
        EXPECT(count(root / L"keep") == 4);
    }
    {   // junk left out: the one real folder is placed as it is
        auto src = ArchiveSource::Open(zip);
        ExtractOptions eo;
        eo.smart = true;
        eo.skipMacMetadata = true;
        eo.overwrite = OverwriteMode::Overwrite;
        auto rep = ExtractTo(*src, root / L"clean", eo);
        EXPECT(rep.ok() && fs::exists(root / L"clean" / L"proj" / L"a.txt") && !fs::exists(root / L"clean" / L"mac"));
        EXPECT(!fs::exists(root / L"clean" / L"__MACOSX") && !fs::exists(root / L"clean" / L"proj" / L"._a.txt") && !fs::exists(root / L"clean" / L"proj" / L".DS_Store"));
        EXPECT(count(root / L"clean") == 1 && rep.macMetadataSkipped >= 3 && rep.files == 1);
    }
    {   // only metadata asked for: nothing is written
        auto src = ArchiveSource::Open(zip);
        const ArchiveTree all(src->Entries());
        std::vector<uint32_t> ids = all.EntryIndices({all.Find(L"__MACOSX")});
        ExtractOptions eo;
        eo.skipMacMetadata = true;
        auto rep = ExtractTo(*src, root / L"none", eo, ids);
        EXPECT(rep.ok() && rep.files == 0 && !fs::exists(root / L"none"));
    }

    // ---- compression: Thumbs.db in folders ----
    Write(root / L"photos" / L"a.jpg", "jpg");
    Write(root / L"photos" / L"Thumbs.db", "cache");
    Write(root / L"photos" / L"sub" / L"THUMBS.DB", "cache");
    Write(root / L"photos" / L"sub" / L"b.jpg", "jpg");
    {
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.skipThumbsDb = true;
        auto rep = CreateArchive(root / L"skip.zip", {root / L"photos"}, co);
        EXPECT(rep.ok() && rep.thumbsDbSkipped == 2 && rep.skipped.empty() && rep.files == 2);
        auto src = ArchiveSource::Open(root / L"skip.zip");
        bool hasThumbs = false;
        for (auto& e : src->Entries()) if (IsThumbsDbName(fs::path(e.path).filename().wstring())) hasThumbs = true;
        EXPECT(!hasThumbs);
        co.skipThumbsDb = false;
        auto rep2 = CreateArchive(root / L"all.zip", {root / L"photos"}, co);
        EXPECT(rep2.ok() && rep2.thumbsDbSkipped == 0 && rep2.files == 4);
        // a Thumbs.db chosen on its own is kept
        co.skipThumbsDb = true;
        auto rep3 = CreateArchive(root / L"one.zip", {root / L"photos" / L"Thumbs.db"}, co);
        EXPECT(rep3.ok() && rep3.files == 1);
    }

    fs::remove_all(root, ec);
    if (failures == 0) std::puts("test_junk: all passed");
    return failures ? 1 : 0;
}
