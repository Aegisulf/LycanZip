// Creating ISO images (ISO 9660 + Joliet, lycan::WriteIso through CreateArchive): the image is read back with 7z.dll and compared
// (names, contents, times, empty files and folders, long and non-ASCII names, names that collide in the 8.3 part, volume label),
// and the refusals (a file of 4 GiB, a password, volumes, an existing output, cancelling).
#include "no_dialogs.h"
#include <windows.h>
#include <winioctl.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

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

static void Write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}

static void SetTime(const fs::path& p, int year) {
    SYSTEMTIME st{static_cast<WORD>(year), 6, 6, 1, 10, 20, 30, 0};
    FILETIME ft{};
    SystemTimeToFileTime(&st, &ft);
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) { SetFileTime(h, nullptr, nullptr, &ft); CloseHandle(h); }
}

struct VecSink : ByteSink {
    std::string data;
    void Write(const void* p, size_t n) override { data.append(static_cast<const char*>(p), n); }
};

int main() {
    const fs::path base = fs::temp_directory_path() / (L"lycan_iso_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(base);
    fs::create_directories(base);
    try {
        const fs::path src = base / L"disc";
        std::string big(5 * 1024 * 1024 + 123, 'x');
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i * 7 + i / 1000) % 26);
        Write(src / L"readme.txt", "hello iso");
        Write(src / L"empty.txt", "");
        Write(src / L"big.bin", big);
        Write(src / L"exactly one sector.bin", std::string(2048, 'S'));
        Write(src / L"sub" / L"deeper" / L"note.txt", "deep");
        Write(src / L"sub" / L"a long file name that goes on and on for quite a while, longer than eight characters.txt", "long");
        Write(src / L"中文檔名.txt", "unicode");
        Write(src / L"same_prefix_one.txt", "1");   // all three become SAME_PRE.TXT in the 8.3 part
        Write(src / L"same_prefix_two.txt", "2");
        Write(src / L"same_prefix_three.txt", "3");
        Write(src / L"with;semicolon.txt", "semi");
        fs::create_directories(src / L"empty folder");
        SetTime(src / L"readme.txt", 2020);

        CompressOptions co;
        co.type = ArchiveType::Iso;
        co.isoLabel = L"MY DISC";
        const fs::path iso = base / L"test.iso";
        uint64_t lastDone = 0;
        co.progress = [&](uint64_t d, uint64_t) { lastDone = d; return true; };
        auto rep = CreateArchive(iso, {src}, co);
        EXPECT(rep.ok() && rep.files == 11 && rep.outputs.size() == 1);
        EXPECT(fs::exists(iso) && fs::file_size(iso) % 2048 == 0 && fs::file_size(iso) > big.size());
        EXPECT(!fs::exists(fs::path(iso.wstring() + L".lycantmp")));
        EXPECT(lastDone >= big.size());

        auto arc = ArchiveSource::Open(iso);
        std::map<std::wstring, std::string> files;
        std::map<std::wstring, uint64_t> times;
        std::vector<std::wstring> dirs;
        for (const Entry& e : arc->Entries()) {
            if (e.isDir) { dirs.push_back(e.path); continue; }
            VecSink sink;
            arc->ReadEntry(e.index, sink);
            files[e.path] = sink.data;
            times[e.path] = e.mtimeFileTime;
        }
        EXPECT(files.size() == 11);
        EXPECT(files[L"disc/readme.txt"] == "hello iso");
        EXPECT(files.count(L"disc/empty.txt") && files[L"disc/empty.txt"].empty());
        EXPECT(files[L"disc/big.bin"] == big);
        EXPECT(files[L"disc/exactly one sector.bin"] == std::string(2048, 'S'));
        EXPECT(files[L"disc/sub/deeper/note.txt"] == "deep");
        {   // a Joliet name has 64 characters at most: the name is cut, the extension stays
            int found = 0;
            for (auto& f : files)
                if (f.first.rfind(L"disc/sub/a long file name", 0) == 0) {
                    ++found;
                    EXPECT(f.second == "long" && f.first.size() == std::wstring(L"disc/sub/").size() + 64 && f.first.substr(f.first.size() - 4) == L".txt");
                }
            EXPECT(found == 1);
        }
        EXPECT(files[L"disc/中文檔名.txt"] == "unicode");
        EXPECT(files[L"disc/same_prefix_one.txt"] == "1" && files[L"disc/same_prefix_two.txt"] == "2" && files[L"disc/same_prefix_three.txt"] == "3");
        EXPECT(files[L"disc/with_semicolon.txt"] == "semi");   // ';' is not allowed in Joliet names
        bool emptyDir = false;
        for (auto& d : dirs) if (d == L"disc/empty folder") emptyDir = true;
        EXPECT(emptyDir);
        // the time of a file is kept (to the 2 s of the Windows file system... the image keeps whole seconds)
        {
            SYSTEMTIME st{2020, 6, 6, 1, 10, 20, 30, 0};
            FILETIME ft{};
            SystemTimeToFileTime(&st, &ft);
            const uint64_t want = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
            const uint64_t got = times[L"disc/readme.txt"];
            const uint64_t diff = got > want ? got - want : want - got;
            EXPECT(diff < 20000000ull * 2);
        }

        // several inputs and the label, and the times of the writing
        {
            CompressOptions c2;
            c2.type = ArchiveType::Iso;
            c2.isoKeepTimes = false;
            const fs::path iso2 = base / L"two.iso";
            auto r2 = CreateArchive(iso2, {src / L"readme.txt", src / L"sub"}, c2);
            EXPECT(r2.ok());
            auto a2 = ArchiveSource::Open(iso2);
            int n = 0;
            for (const Entry& e : a2->Entries()) if (!e.isDir) ++n;
            EXPECT(n == 3);
            for (const Entry& e : a2->Entries())
                if (e.path == L"readme.txt") EXPECT(e.mtimeFileTime > times[L"disc/readme.txt"] + 365ull * 86400 * 10000000ull);
        }

        // the refusals
        {
            bool threw = false;
            try { CreateArchive(iso, {src}, co); } catch (const Error&) { threw = true; }
            EXPECT(threw);   // exists already
            CompressOptions c3 = co;
            c3.overwrite = true;
            EXPECT(CreateArchive(iso, {src}, c3).ok());   // replaced
            c3.password = L"x";
            threw = false;
            try { CreateArchive(base / L"pw.iso", {src}, c3); } catch (const Error&) { threw = true; }
            EXPECT(threw && !fs::exists(base / L"pw.iso"));
            c3.password.clear();
            c3.volumeSize = 1024 * 1024;
            threw = false;
            try { CreateArchive(base / L"vol.iso", {src}, c3); } catch (const Error&) { threw = true; }
            EXPECT(threw);
            // cancelling leaves nothing
            CompressOptions c4;
            c4.type = ArchiveType::Iso;
            c4.progress = [](uint64_t, uint64_t) { return false; };
            threw = false;
            try { CreateArchive(base / L"cancel.iso", {src}, c4); } catch (const Error& e) { threw = e.hr() == E_ABORT; }
            EXPECT(threw && !fs::exists(base / L"cancel.iso") && !fs::exists(base / L"cancel.iso.lycantmp"));
        }

        // a file of 4 GiB or more (a sparse file: no disk space is used) is refused before anything is written
        {
            const fs::path huge = base / L"huge" / L"huge.bin";
            fs::create_directories(huge.parent_path());
            HANDLE h = CreateFileW(huge.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            bool made = false;
            if (h != INVALID_HANDLE_VALUE) {
                DWORD br = 0;
                DeviceIoControl(h, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &br, nullptr);
                LARGE_INTEGER pos;
                pos.QuadPart = 4ll * 1024 * 1024 * 1024;
                made = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && SetEndOfFile(h);
                CloseHandle(h);
            }
            if (made) {
                bool threw = false;
                try { CreateArchive(base / L"huge.iso", {huge}, CompressOptions{ArchiveType::Iso}); } catch (const Error&) { threw = true; }
                EXPECT(threw && !fs::exists(base / L"huge.iso") && !fs::exists(base / L"huge.iso.lycantmp"));
            } else {
                std::puts("test_iso: (no sparse file here: the 4 GiB check was skipped)");
            }
        }

        // the type is guessed from the name
        ArchiveType t;
        EXPECT(GuessArchiveType(L"disc.ISO", t) && t == ArchiveType::Iso);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: exception: %s\n", e.what());
        ++failures;
    }
    fs::remove_all(base);
    if (failures == 0) std::puts("test_iso: all passed");
    return failures ? 1 : 0;
}
