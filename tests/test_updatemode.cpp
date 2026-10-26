// The update modes of adding to an existing archive (UpdateMode): replace always / update and add (only newer) / freshen (only what is in the archive) /
// synchronize (the archive becomes the folder). Zip and 7z, the preview, the times (2 s, the hour of a zip's local time), a different size.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/update.h"

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

static void Write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}
static std::string Read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

// 2024-06-01 10:00:00 UTC plus `seconds`
static void SetTime(const fs::path& p, long long seconds) {
    SYSTEMTIME st{2024, 6, 6, 1, 10, 0, 0, 0};
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<unsigned long long>(seconds * 10000000LL);
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) { SetFileTime(h, nullptr, nullptr, &ft); CloseHandle(h); }
}

struct VecSink : ByteSink {
    std::string data;
    void Write(const void* p, size_t n) override { data.append(static_cast<const char*>(p), n); }
};

// path -> content of the files of the archive
static std::map<std::wstring, std::string> Contents(const fs::path& archive) {
    std::map<std::wstring, std::string> m;
    auto src = ArchiveSource::Open(archive);
    for (const Entry& e : src->Entries()) {
        if (e.isDir) continue;
        VecSink sink;
        src->ReadEntry(e.index, sink);
        m[e.path] = sink.data;
    }
    return m;
}

static void Fresh(const fs::path& root) {
    fs::remove_all(root);
    fs::create_directories(root / "src" / "sub");
    Write(root / "src" / "a.txt", "a old");
    Write(root / "src" / "b.txt", "b same");
    Write(root / "src" / "sub" / "c.txt", "c in a folder");
    for (auto f : {"a.txt", "b.txt"}) SetTime(root / "src" / f, 0);
    SetTime(root / "src" / "sub" / "c.txt", 0);
}

static void Run(ArchiveType type, const wchar_t* ext, fs::path base) {
    const bool zip = type == ArchiveType::Zip;
    const fs::path root = base / (zip ? L"zip" : L"7z");
    Fresh(root);
    const fs::path archive = root / (std::wstring(L"t") + ext);
    CompressOptions co;
    co.type = type;
    co.storeCompressed = false;
    CreateArchive(archive, {root / "src"}, co);
    const std::string original = Read(archive);

    // the folder changes: a.txt is newer (a day), b.txt is the same, d.txt is new, sub\c.txt is gone, e.txt is older than the archive's copy
    auto change = [&] {
        Write(root / "src" / "a.txt", "a NEW content");
        SetTime(root / "src" / "a.txt", 86400);
        Write(root / "src" / "d.txt", "d is new");
        SetTime(root / "src" / "d.txt", 100);
        fs::remove(root / "src" / "sub" / "c.txt");
    };
    change();

    UpdateOptions uo;
    uo.compress.type = type;
    uo.compress.storeCompressed = false;
    const std::vector<UpdateOp> add = {UpdateOp::Add(L"", {root / "src"})};
    auto fresh = [&] { fs::copy_file(archive.parent_path() / (std::wstring(L"orig") + ext), archive, fs::copy_options::overwrite_existing); };
    { std::ofstream(archive.parent_path() / (std::wstring(L"orig") + ext), std::ios::binary).write(original.data(), static_cast<std::streamsize>(original.size())); }

    // the preview changes nothing and says what would happen
    {
        uo.mode = UpdateMode::Synchronize;
        auto pv = PreviewUpdate(archive, add, uo);
        EXPECT(pv.added == 1 && pv.replaced == 1 && pv.deleted == 1 && pv.unchanged == 1);   // d, a, sub\c.txt, b
        EXPECT(pv.deletedPaths.size() == 1 && pv.deletedPaths[0].find(L"c.txt") != std::wstring::npos);
        EXPECT(Read(archive) == original);
    }
    // replace (the default): everything that is on the disk goes in again
    {
        fresh();
        uo.mode = UpdateMode::AddReplace;
        auto rep = UpdateArchive(archive, add, uo);
        EXPECT(rep.ok() && rep.added == 1 && rep.replaced == 2 && rep.deleted == 0 && rep.unchanged == 0);   // d new; a and b replaced
        auto c = Contents(archive);
        EXPECT(c[L"src/a.txt"] == "a NEW content" && c.count(L"src/d.txt") && c.count(L"src/sub/c.txt"));
    }
    // update and add: only what is newer replaces, new files come in, nothing goes out
    {
        fresh();
        uo.mode = UpdateMode::UpdateAdd;
        auto rep = UpdateArchive(archive, add, uo);
        EXPECT(rep.ok() && rep.added == 1 && rep.replaced == 1 && rep.unchanged == 1 && rep.deleted == 0);   // d; a; b is not newer
        auto c = Contents(archive);
        EXPECT(c[L"src/a.txt"] == "a NEW content" && c[L"src/b.txt"] == "b same" && c.count(L"src/d.txt") && c.count(L"src/sub/c.txt"));
        // again: nothing is newer any more, the archive is not rewritten
        auto again = UpdateArchive(archive, add, uo);
        EXPECT(again.ok() && !again.changed && again.unchanged == 3 && again.added == 0);   // a, b, d
    }
    // freshen: only what is in the archive; new files are not added
    {
        fresh();
        uo.mode = UpdateMode::Freshen;
        auto rep = UpdateArchive(archive, add, uo);
        EXPECT(rep.ok() && rep.added == 0 && rep.notAdded == 1 && rep.replaced == 1 && rep.unchanged == 1);
        auto c = Contents(archive);
        EXPECT(c[L"src/a.txt"] == "a NEW content" && !c.count(L"src/d.txt") && c.count(L"src/sub/c.txt"));
    }
    // synchronize: the archive becomes the folder
    {
        fresh();
        uo.mode = UpdateMode::Synchronize;
        auto rep = UpdateArchive(archive, add, uo);
        EXPECT(rep.ok() && rep.added == 1 && rep.replaced == 1 && rep.deleted == 1 && rep.unchanged == 1);
        auto c = Contents(archive);
        EXPECT(c.size() == 3 && c[L"src/a.txt"] == "a NEW content" && c[L"src/b.txt"] == "b same" && c.count(L"src/d.txt") && !c.count(L"src/sub/c.txt"));
    }
    // a file on the disk that is older than the archive's copy: update leaves it, replace takes it, synchronize takes it (the time differs)
    {
        fresh();
        SetTime(root / "src" / "b.txt", -86400 * 3);
        Write(root / "src" / "b.txt", "b OLDER");
        SetTime(root / "src" / "b.txt", -86400 * 3);
        uo.mode = UpdateMode::UpdateAdd;
        auto rep = UpdateArchive(archive, add, uo);
        EXPECT(Contents(archive)[L"src/b.txt"] == "b same");
        fresh();
        uo.mode = UpdateMode::Synchronize;
        UpdateArchive(archive, add, uo);
        EXPECT(Contents(archive)[L"src/b.txt"] == "b OLDER");
        (void)rep;
    }
    // the same time but another size: synchronize replaces it, update does not
    {
        fresh();
        Write(root / "src" / "b.txt", "b has another size now");
        SetTime(root / "src" / "b.txt", 0);
        uo.mode = UpdateMode::UpdateAdd;
        UpdateArchive(archive, add, uo);
        EXPECT(Contents(archive)[L"src/b.txt"] == "b same");
        fresh();
        uo.mode = UpdateMode::Synchronize;
        UpdateArchive(archive, add, uo);
        EXPECT(Contents(archive)[L"src/b.txt"] == "b has another size now");
    }
    // the hour of a zip's local time: a zip does not count an hour as newer (the clock was put forward), a 7z does
    {
        fresh();
        Write(root / "src" / "b.txt", "b an hour later");
        SetTime(root / "src" / "b.txt", 3600);
        uo.mode = UpdateMode::UpdateAdd;
        UpdateArchive(archive, add, uo);
        EXPECT(Contents(archive)[L"src/b.txt"] == (zip ? "b same" : "b an hour later"));
    }
    // two seconds are no difference
    {
        fresh();
        Write(root / "src" / "b.txt", "b two seconds later");
        SetTime(root / "src" / "b.txt", 2);
        uo.mode = UpdateMode::UpdateAdd;
        UpdateArchive(archive, add, uo);
        EXPECT(Contents(archive)[L"src/b.txt"] == "b same");
    }
    // synchronize only covers the folders that were added: other things of the archive stay
    {
        fresh();
        UpdateArchive(archive, {UpdateOp::Add(L"", {root / "src" / "d.txt"})}, UpdateOptions{});   // a loose file in the archive
        uo.mode = UpdateMode::Synchronize;
        UpdateArchive(archive, add, uo);
        auto c = Contents(archive);
        EXPECT(c.count(L"d.txt") && !c.count(L"src/sub/c.txt"));   // d.txt (outside src) stays, c.txt (inside) goes
    }
}

int main() {
    fs::path base = fs::temp_directory_path() / (L"lycan_um_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(base);
    fs::create_directories(base);
    try {
        Run(ArchiveType::Zip, L".zip", base);
        Run(ArchiveType::SevenZip, L".7z", base);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
        ++failures;
    }
    fs::remove_all(base);
    if (failures == 0) std::puts("test_updatemode: all passed");
    return failures ? 1 : 0;
}
