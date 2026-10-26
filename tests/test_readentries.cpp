// ArchiveSource::ReadEntries: several items of a solid archive in one pass (the drag out of a solid archive uses it): the items come in the
// order of the archive, the content is whole, an item that is not wanted is skipped, the pass can be cancelled, a failing sink ends it.
#include "no_dialogs.h"
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"

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

static std::string Body(int n) {
    std::string s;
    for (int i = 0; i < 4000 + n * 700; ++i) s += static_cast<char>('a' + (i * (n + 3) + i / 7) % 26);
    return s;
}

class StringSink final : public ByteSink {
public:
    std::string d;
    void Write(const void* p, size_t n) override { d.append(static_cast<const char*>(p), n); }
};

int main() {
    const fs::path root = fs::temp_directory_path() / (L"lycan_readentries_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root / "src");
    for (int i = 0; i < 6; ++i) std::ofstream(root / "src" / ("f" + std::to_string(i) + ".txt"), std::ios::binary) << Body(i);
    CompressOptions co;
    co.type = ArchiveType::SevenZip;
    CreateArchive(root / "solid.7z", {root / "src"}, co);

    auto src = ArchiveSource::Open(root / "solid.7z");
    EXPECT(src->Caps().solid);
    std::map<std::string, uint32_t> index;   // file name -> entry index
    for (const Entry& e : src->Entries())
        if (!e.isDir) index[ToUtf8(e.path.substr(e.path.find_last_of(L'/') + 1))] = e.index;
    EXPECT(index.size() == 6);

    // three items, asked for in a jumbled order: they come in the order of the archive, once each, whole
    {
        const std::vector<uint32_t> want = {index["f4.txt"], index["f1.txt"], index["f3.txt"]};
        std::map<uint32_t, StringSink> sinks;
        std::vector<uint32_t> began, ended;
        src->ReadEntries(
            want, [&](uint32_t i) { began.push_back(i); return &sinks[i]; }, [&](uint32_t i, bool ok) { EXPECT(ok); ended.push_back(i); });
        EXPECT(began.size() == 3 && ended.size() == 3);
        EXPECT(std::is_sorted(began.begin(), began.end()) && began == ended);
        EXPECT(sinks[index["f1.txt"]].d == Body(1) && sinks[index["f3.txt"]].d == Body(3) && sinks[index["f4.txt"]].d == Body(4));
    }
    // an item that is not wanted (begin gives nullptr) is skipped, the others are whole
    {
        StringSink s5;
        int ends = 0;
        src->ReadEntries(
            {index["f2.txt"], index["f5.txt"]}, [&](uint32_t i) -> ByteSink* { return i == index["f5.txt"] ? &s5 : nullptr; }, [&](uint32_t, bool) { ++ends; });
        EXPECT(s5.d == Body(5) && ends == 1);
    }
    // cancelled: E_ABORT
    {
        bool cancelled = false;
        StringSink s;
        try {
            src->ReadEntries(
                {index["f0.txt"], index["f1.txt"], index["f2.txt"]}, [&](uint32_t) { return &s; }, [](uint32_t, bool) {}, [](uint64_t, uint64_t) { return false; });
        } catch (const Error& e) {
            cancelled = e.cancelled();
        }
        EXPECT(cancelled);
    }
    // a sink that throws ends the pass with that error; the archive can be read again afterwards
    {
        class Boom final : public ByteSink {
        public:
            void Write(const void*, size_t) override { throw Error(E_FAIL, std::string("boom")); }
        } boom;
        bool threw = false;
        try {
            src->ReadEntries({index["f0.txt"], index["f1.txt"]}, [&](uint32_t) { return &boom; }, [](uint32_t, bool) {});
        } catch (const Error&) {
            threw = true;
        }
        EXPECT(threw);
        StringSink again;
        src->ReadEntry(index["f2.txt"], again);
        EXPECT(again.d == Body(2));
    }
    // an index that does not exist
    {
        bool threw = false;
        try {
            src->ReadEntries({9999}, [](uint32_t) -> ByteSink* { return nullptr; }, [](uint32_t, bool) {});
        } catch (const Error&) {
            threw = true;
        }
        EXPECT(threw);
    }

    // ---- the method of every item (the column and the sort), and what an item says about itself (the Properties window) ----
    {
        src = ArchiveSource::Open(root / "solid.7z");   // (the one above was released)
        EXPECT(src->MethodName(0).empty());
        std::wstring m7z;
        for (const Entry& e : src->Entries()) if (!e.isDir) m7z = src->MethodName(e.methodIdx);
        EXPECT(m7z.find(L"LZMA") != std::wstring::npos);   // 7z: the method of the items is that of the solid block
        const auto t7 = src->EntryTechInfo(index["f2.txt"]);
        EXPECT(t7.method.find(L"LZMA") != std::wstring::npos && t7.hasBlock && t7.block == 0);

        CompressOptions zo;
        zo.type = ArchiveType::Zip;
        CreateArchive(root / "plain.zip", {root / "src"}, zo);
        auto zip = ArchiveSource::Open(root / "plain.zip");
        std::wstring mzip;
        uint32_t anyFile = 0;
        for (const Entry& e : zip->Entries()) if (!e.isDir) { mzip = zip->MethodName(e.methodIdx); anyFile = e.index; }
        EXPECT(mzip == L"Deflate");
        const auto tz = zip->EntryTechInfo(anyFile);
        EXPECT(tz.method == L"Deflate" && tz.hasOffset);
        EXPECT(zip->EntryTechInfo(99999).method.empty());   // an index that does not exist: nothing
        EXPECT(zip->MethodName(60000).empty());
        src.reset();   // the files are deleted below
    }

    src.reset();
    fs::remove_all(root);
    if (failures == 0) std::puts("test_readentries: all passed");
    return failures ? 1 : 0;
}
