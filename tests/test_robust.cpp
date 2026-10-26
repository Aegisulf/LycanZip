// Robustness tests at the Core API level: cancellation cleanup, disk-full injection, decompression-bomb limits,
// long paths, locked files, symbolic links, concurrency, many-volume archives.
// Everything runs on small or sparse data so the whole file takes well under a minute. Heavy cases (> 4 GB)
// live in scripts/robust-heavy.ps1.
#include "no_dialogs.h"
#include <windows.h>
#include <psapi.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <thread>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/encoding.h"
#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/security.h"
#include "lycan/tree.h"
#include "lycan/testhooks.h"
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
#define EXPECT_MSG(cond, msg)                                                       \
    do {                                                                            \
        if (!(cond)) {                                                              \
            std::fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #cond, (msg)); \
            ++failures;                                                             \
        }                                                                           \
    } while (0)

static fs::path g_root;

static void WriteRandom(const fs::path& p, size_t bytes, unsigned seed) {
    fs::create_directories(ToLongPath(p.parent_path()));
    std::mt19937 rng(seed);
    std::ofstream f(ToLongPath(p), std::ios::binary);
    std::vector<uint32_t> buf(64 * 1024 / 4);
    for (size_t left = bytes; left;) {
        for (auto& v : buf) v = rng();
        const size_t n = std::min(left, buf.size() * 4);
        f.write(reinterpret_cast<const char*>(buf.data()), static_cast<std::streamsize>(n));
        left -= n;
    }
}
static void WriteText(const fs::path& p, const std::string& s) {
    fs::create_directories(ToLongPath(p.parent_path()));
    std::ofstream(ToLongPath(p), std::ios::binary) << s;
}
static std::string ReadAll(const fs::path& p) {
    std::ifstream f(ToLongPath(p), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
// All files and folders below dir (relative paths); empty if dir does not exist.
static std::set<std::wstring> ListAll(const fs::path& dir) {
    std::set<std::wstring> out;
    std::error_code ec;
    if (!fs::exists(ToLongPath(dir), ec)) return out;
    for (auto it = fs::recursive_directory_iterator(ToLongPath(dir), ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        out.insert(fs::relative(it->path(), ToLongPath(dir)).wstring());
    return out;
}
static std::string Join(const std::set<std::wstring>& s) {
    std::string r;
    for (auto& x : s) r += ToUtf8(x) + " ";
    return r;
}
static fs::path Fresh(const wchar_t* name) {
    fs::path p = g_root / name;
    std::error_code ec;
    fs::remove_all(ToLongPath(p), ec);
    fs::create_directories(ToLongPath(p));
    return p;
}

static const struct { const wchar_t* label; ArchiveType type; const wchar_t* ext; } kTypes[] = {
    {L"zip", ArchiveType::Zip, L".zip"},       {L"7z", ArchiveType::SevenZip, L".7z"}, {L"tar", ArchiveType::Tar, L".tar"},
    {L"tar.gz", ArchiveType::TarGz, L".tar.gz"}, {L"tar.xz", ArchiveType::TarXz, L".tar.xz"}, {L"tar.bz2", ArchiveType::TarBz2, L".tar.bz2"},
};

// A private TEMP so that leftovers of the tar.* staging file are visible.
static fs::path g_tmp;
static std::set<std::wstring> TempLeftovers() { return ListAll(g_tmp); }

// ---------------------------------------------------------------------------------------------------------
static void TestCompressCancel() {
    std::puts("-- cancelling compression leaves nothing behind");
    fs::path src = Fresh(L"cc_src");
    WriteRandom(src / "a.bin", 6 << 20, 1);
    WriteRandom(src / "b.bin", 6 << 20, 2);
    for (auto& t : kTypes) {
        for (int vol = 0; vol < 2; ++vol) {
            if (vol && t.type != ArchiveType::Zip && t.type != ArchiveType::SevenZip) continue;
            fs::path out = Fresh(L"cc_out");
            CompressOptions co;
            co.type = t.type;
            co.volumeSize = vol ? 256 * 1024 : 0;
            int calls = 0;
            co.progress = [&](uint64_t d, uint64_t tot) { ++calls; return !(calls > 2 && d * 4 > tot); };
            bool cancelled = false;
            try { CreateArchive(out / (std::wstring(L"x") + t.ext), {src}, co); }
            catch (const Error& e) { cancelled = e.cancelled(); }
            std::string label = ToUtf8(t.label) + (vol ? " (volumes)" : "");
            EXPECT_MSG(cancelled, label.c_str());
            auto left = ListAll(out);
            EXPECT_MSG(left.empty(), (label + " left: " + Join(left)).c_str());
            auto tl = TempLeftovers();
            EXPECT_MSG(tl.empty(), (label + " temp left: " + Join(tl)).c_str());
        }
    }
}

static void TestExtractCancel() {
    std::puts("-- cancelling extraction leaves no partial file");
    fs::path src = Fresh(L"ec_src");
    WriteRandom(src / "one.bin", 24 << 20, 3);
    WriteRandom(src / "two.bin", 4 << 20, 4);
    for (auto& t : kTypes) {
        fs::path arc = g_root / (std::wstring(L"ec_") + t.ext);
        fs::remove(arc);
        CompressOptions co;
        co.type = t.type;
        co.level = 0;  // store: progress is fine-grained
        co.overwrite = true;
        CreateArchive(arc, {src}, co);
        fs::path dest = Fresh(L"ec_dest");
        auto s = ArchiveSource::Open(arc);
        ExtractOptions eo;
        int calls = 0;
        eo.progress = [&](uint64_t d, uint64_t tot) { ++calls; return !(calls > 2 && d * 3 > tot); };
        bool cancelled = false;
        try { ExtractTo(*s, dest, eo); }
        catch (const Error& e) { cancelled = e.cancelled(); }
        s.reset();
        std::string label = ToUtf8(t.label);
        EXPECT_MSG(cancelled, label.c_str());
        // Whole files that finished before the cancel may stay; a half-written file must not.
        for (auto& rel : ListAll(dest)) {
            fs::path f = dest / rel;
            std::error_code ec;
            if (!fs::is_regular_file(ToLongPath(f), ec)) continue;
            const auto size = fs::file_size(ToLongPath(f), ec);
            const std::wstring name = f.filename().wstring();
            const uint64_t want = name == L"one.bin" ? (24u << 20) : name == L"two.bin" ? (4u << 20) : 0;
            EXPECT_MSG(size == want, (label + " partial file left: " + ToUtf8(rel) + " size " + std::to_string(size)).c_str());
        }
        EXPECT_MSG(TempLeftovers().empty(), (label + " temp left: " + Join(TempLeftovers())).c_str());
    }
}

static void TestDiskFull() {
    std::puts("-- disk full (injected) while compressing and extracting");
    fs::path src = Fresh(L"df_src");
    WriteRandom(src / "a.bin", 4 << 20, 5);
    WriteText(src / "b.txt", std::string(100000, 'b'));
    for (auto& t : kTypes) {
        for (int vol = 0; vol < 2; ++vol) {
            if (vol && t.type != ArchiveType::Zip && t.type != ArchiveType::SevenZip) continue;
            fs::path out = Fresh(L"df_out");
            CompressOptions co;
            co.type = t.type;
            co.volumeSize = vol ? 256 * 1024 : 0;
            SetTestWriteFault(1 << 20);
            std::string what;
            bool threw = false, diskFull = false;
            try { CreateArchive(out / (std::wstring(L"x") + t.ext), {src}, co); }
            catch (const Error& e) { threw = true; what = e.what(); diskFull = e.diskFull(); }
            SetTestWriteFault(0);
            std::string label = ToUtf8(t.label) + (vol ? " (volumes)" : "");
            EXPECT_MSG(threw, (label + ": compression must fail when the disk is full").c_str());
            EXPECT_MSG(diskFull, (label + ": the failure is recognised as disk-full: " + what).c_str());
            auto left = ListAll(out);
            EXPECT_MSG(left.empty(), (label + " left: " + Join(left)).c_str());
            EXPECT_MSG(TempLeftovers().empty(), (label + " temp left: " + Join(TempLeftovers())).c_str());
            std::printf("   compress %-22s -> \"%s\"\n", label.c_str(), what.c_str());
        }
        // extraction
        fs::path arc = g_root / (std::wstring(L"df_") + t.ext);
        fs::remove(arc);
        CompressOptions co;
        co.type = t.type;
        co.overwrite = true;
        CreateArchive(arc, {src}, co);
        fs::path dest = Fresh(L"df_dest");
        auto s = ArchiveSource::Open(arc);
        SetTestWriteFault(1 << 20);
        bool threw = false, reportedErr = false, diskFull = false;
        std::string what;
        try {
            auto rep = ExtractTo(*s, dest, {});
            reportedErr = !rep.ok();
            if (!rep.errors.empty()) what = ToUtf8(rep.errors[0]);
        } catch (const Error& e) { threw = true; what = e.what(); diskFull = e.diskFull(); }
        SetTestWriteFault(0);
        s.reset();
        std::string label = ToUtf8(t.label);
        EXPECT_MSG(threw || reportedErr, (label + ": extraction must report the failure").c_str());
        EXPECT_MSG(diskFull, (label + ": the failure is recognised as disk-full: " + what).c_str());
        for (auto& rel : ListAll(dest)) {
            fs::path f = dest / rel;
            std::error_code ec;
            if (!fs::is_regular_file(ToLongPath(f), ec)) continue;
            const auto size = fs::file_size(ToLongPath(f), ec);
            const std::wstring name = f.filename().wstring();
            const uint64_t want = name == L"a.bin" ? (4u << 20) : name == L"b.txt" ? 100000u : 0;
            EXPECT_MSG(size == want, (label + " partial file left: " + ToUtf8(rel)).c_str());
        }
        std::printf("   extract  %-22s -> \"%s\"\n", label.c_str(), what.c_str());
    }
}

// The multi-threaded BZip2 encoder of 7z.dll never returned when its output failed (found with a real full disk): the output of
// every BZip2 user is guarded now. Many blocks are needed, so the data is big enough for several threads.
static void TestDiskFullBzip2() {
    std::puts("-- disk full (injected) with the BZip2 encoder: must return, not hang");
    fs::path src = Fresh(L"dfb_src");
    WriteRandom(src / "r.bin", 24 << 20, 21);
    struct Case { const wchar_t* label; ArchiveType type; const wchar_t* method; const wchar_t* ext; uint64_t fault; } cases[] = {
        {L"tar.bz2", ArchiveType::TarBz2, L"", L".tar.bz2", (24u << 20) + (3u << 20)},  // the staged tar is written first
        {L"zip BZip2", ArchiveType::Zip, L"BZip2", L".zip", 3u << 20},
        {L"7z BZip2", ArchiveType::SevenZip, L"BZip2", L".7z", 3u << 20},
    };
    for (auto& c : cases) {
        fs::path out = Fresh(L"dfb_out");
        auto done = std::make_shared<std::promise<std::string>>();
        auto fut = done->get_future();
        std::thread([=] {
            CompressOptions co;
            co.type = c.type;
            co.method = c.method;
            SetTestWriteFault(c.fault);
            std::string what = "no error";
            try { CreateArchive(out / (std::wstring(L"x") + c.ext), {src}, co); }
            catch (const Error& e) { what = e.diskFull() ? "disk full" : std::string("other: ") + e.what(); }
            SetTestWriteFault(0);
            done->set_value(what);
        }).detach();
        if (fut.wait_for(std::chrono::seconds(180)) != std::future_status::ready) {
            std::printf("FAIL: %s hangs when the output fails\n", ToUtf8(c.label).c_str());
            std::fflush(stdout);
            std::_Exit(1);
        }
        const std::string what = fut.get();
        EXPECT_MSG(what == "disk full", (ToUtf8(c.label) + ": " + what).c_str());
        auto left = ListAll(out);
        EXPECT_MSG(left.empty(), (ToUtf8(c.label) + " left: " + Join(left)).c_str());
        std::printf("   %-10s -> %s\n", ToUtf8(c.label).c_str(), what.c_str());
    }
}

static void TestFreeSpacePrecheck() {
    std::puts("-- free-space precheck before extracting");
    fs::path src = Fresh(L"fs_src");
    WriteRandom(src / "a.bin", 3 << 20, 11);
    fs::path arc = g_root / L"fs.zip";
    fs::remove(arc);
    CompressOptions co;
    co.overwrite = true;
    CreateArchive(arc, {src}, co);
    auto s = ArchiveSource::Open(arc);
    fs::path dest = Fresh(L"fs_dest");
    SetTestFreeSpace(1 << 20);  // 1 MB "free", 3 MB needed
    bool diskFull = false;
    std::string what;
    try { ExtractTo(*s, dest, {}); } catch (const Error& e) { diskFull = e.diskFull(); what = e.what(); }
    EXPECT_MSG(diskFull, ("not enough room is reported up front: " + what).c_str());
    EXPECT_MSG(ListAll(dest).empty(), "nothing is written when the precheck fails");
    SetTestFreeSpace(64 << 20);
    auto rep = ExtractTo(*s, dest, {});
    EXPECT_MSG(rep.ok() && rep.files == 1, "enough room: extraction proceeds");
    SetTestFreeSpace(0);
    std::printf("   message: %s\n", what.c_str());
}

static void TestBombLimits() {
    std::puts("-- decompression-bomb limits");
    fs::path src = Fresh(L"bomb_src");
    {  // 64 MB of zeros: tiny archive, huge ratio
        fs::create_directories(src);
        std::ofstream f(src / "zeros.bin", std::ios::binary);
        std::string z(1 << 20, '\0');
        for (int i = 0; i < 64; ++i) f.write(z.data(), static_cast<std::streamsize>(z.size()));
    }
    fs::path arc = g_root / L"bomb.zip";
    fs::remove(arc);
    CompressOptions co;
    co.overwrite = true;
    CreateArchive(arc, {src}, co);
    auto s = ArchiveSource::Open(arc);
    {
        ExtractOptions eo;
        eo.maxTotalBytes = 10 << 20;
        fs::path dest = Fresh(L"bomb_dest");
        bool threw = false;
        try { ExtractTo(*s, dest, eo); } catch (const Error&) { threw = true; }
        EXPECT_MSG(threw, "maxTotalBytes rejects an archive whose declared size is over the limit");
        EXPECT_MSG(ListAll(dest).empty(), ("nothing written: " + Join(ListAll(dest))).c_str());
    }
    {
        ExtractOptions eo;
        eo.maxRatio = 100;
        fs::path dest = Fresh(L"bomb_dest");
        bool threw = false;
        try { ExtractTo(*s, dest, eo); } catch (const Error&) { threw = true; }
        EXPECT_MSG(threw, "maxRatio rejects a 64 MB -> ~70 KB archive");
        EXPECT_MSG(ListAll(dest).empty(), ("nothing written: " + Join(ListAll(dest))).c_str());
    }
    {
        ExtractOptions eo;
        eo.maxTotalBytes = 1ull << 30;
        eo.maxRatio = 100000;
        fs::path dest = Fresh(L"bomb_dest");
        auto rep = ExtractTo(*s, dest, eo);
        EXPECT_MSG(rep.ok() && rep.bytes == (64u << 20), "limits that are not exceeded do not interfere");
    }
    s.reset();
}

static void TestDeclaredSizeLie() {
    // The central directory says 1 KB but the data inflates to far more: the write budget must still stop it.
    std::puts("-- actual output is capped even if the declared size lies");
    // Build a zip by hand: one deflated entry (a stream of zeros) with a falsified uncompressed size.
    fs::path src = Fresh(L"lie_src");
    {
        std::ofstream f(src / "z.bin", std::ios::binary);
        std::string z(1 << 20, '\0');
        for (int i = 0; i < 32; ++i) f.write(z.data(), static_cast<std::streamsize>(z.size()));
    }
    fs::path arc = g_root / L"lie.zip";
    fs::remove(arc);
    CompressOptions co;
    co.overwrite = true;
    CreateArchive(arc, {src}, co);
    std::string data = ReadAll(arc);
    // Patch the uncompressed size (central directory entry offset 24, local header offset 22) to 1000.
    auto patch = [&](const char* sig, size_t off) {
        size_t p = data.find(std::string(sig, 4));
        if (p != std::string::npos) { uint32_t v = 1000; std::memcpy(&data[p + off], &v, 4); }
    };
    patch("PK\x03\x04", 22);
    patch("PK\x01\x02", 24);
    WriteText(arc, data);
    try {
        auto s = ArchiveSource::Open(arc);
        ExtractOptions eo;
        eo.maxTotalBytes = 8 << 20;
        fs::path dest = Fresh(L"lie_dest");
        bool threw = false;
        ExtractReport rep;
        try { rep = ExtractTo(*s, dest, eo); } catch (const Error&) { threw = true; }
        uint64_t written = 0;
        for (auto& rel : ListAll(dest)) { std::error_code ec; auto sz = fs::file_size(ToLongPath(dest / rel), ec); if (!ec) written += sz; }
        EXPECT_MSG(written <= (8u << 20), ("bytes written despite the lie: " + std::to_string(written)).c_str());
        std::printf("   threw=%d written=%llu errors=%zu\n", threw, static_cast<unsigned long long>(written), rep.errors.size());
    } catch (const Error& e) {
        std::printf("   archive with falsified size rejected at open: %s\n", e.what());
    }
}

static std::wstring LongDir(const fs::path& base, int segments) {
    std::wstring p = base.wstring();
    for (int i = 0; i < segments; ++i) p += L"\\segment_with_a_fairly_long_name_" + std::to_wstring(i);
    return p;
}

static void TestLongPaths() {
    std::puts("-- paths longer than 260 characters");
    fs::path base = Fresh(L"lp");
    fs::path deep = LongDir(base / L"src", 9);  // ~ 300 characters
    EXPECT(deep.wstring().size() > 280);
    WriteText(deep / L"file.txt", "long path content");
    WriteText(base / L"src" / L"short.txt", "short");
    for (auto& t : kTypes) {
        fs::path arc = base / (std::wstring(L"x") + t.ext);
        CompressOptions co;
        co.type = t.type;
        co.overwrite = true;
        auto rep = CreateArchive(arc, {base / L"src"}, co);
        std::string label = ToUtf8(t.label);
        EXPECT_MSG(rep.ok() && rep.files == 2, (label + " compress long path").c_str());
        // Extract into a destination that is itself deep.
        fs::path dest = LongDir(base / (std::wstring(L"dest_") + t.label), 4);
        auto s = ArchiveSource::Open(arc);
        auto er = ExtractTo(*s, dest, {});
        EXPECT_MSG(er.ok() && er.files == 2, (label + " extract long path").c_str());
        fs::path got = dest / L"src";
        got = LongDir(got, 9) + L"\\file.txt";
        EXPECT_MSG(ReadAll(got) == "long path content", (label + " long path content").c_str());
    }
}

static void TestLockedFile() {
    std::puts("-- a source file locked by another program");
    fs::path src = Fresh(L"lock_src");
    WriteText(src / "ok1.txt", "one");
    WriteText(src / "locked.txt", "two");
    WriteText(src / "ok2.txt", "three");
    HANDLE h = CreateFileW((src / "locked.txt").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT(h != INVALID_HANDLE_VALUE);
    for (auto& t : kTypes) {
        fs::path out = Fresh(L"lock_out");
        CompressOptions co;
        co.type = t.type;
        std::string label = ToUtf8(t.label);
        try {
            auto rep = CreateArchive(out / (std::wstring(L"x") + t.ext), {src}, co);
            EXPECT_MSG(!rep.ok(), (label + ": locked file must be reported").c_str());
            auto s = ArchiveSource::Open(rep.outputs.at(0));
            int files = 0;
            for (auto& e : s->Entries()) if (!e.isDir) ++files;
            EXPECT_MSG(files == 2, (label + ": the other files are still archived (got " + std::to_string(files) + ")").c_str());
            std::printf("   %-8s errors=%zu: %s\n", label.c_str(), rep.errors.size(), rep.errors.empty() ? "" : ToUtf8(rep.errors[0]).c_str());
        } catch (const Error& e) {
            // Failing the whole job is acceptable only if nothing is left behind.
            std::printf("   %-8s whole job failed: %s\n", label.c_str(), e.what());
            EXPECT_MSG(ListAll(out).empty(), (label + " left: " + Join(ListAll(out))).c_str());
        }
        EXPECT_MSG(TempLeftovers().empty(), (label + " temp left: " + Join(TempLeftovers())).c_str());
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

static bool MakeSymlink(const fs::path& link, const fs::path& target, bool dir) {
    return CreateSymbolicLinkW(link.c_str(), target.c_str(), (dir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0) | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
}

static void TestSymlinks() {
    std::puts("-- symbolic links");
    fs::path base = Fresh(L"sl");
    fs::path secret = base / L"outside" / L"secret.txt";
    WriteText(secret, "do not archive me");
    fs::path src = base / L"src";
    WriteText(src / L"real.txt", "real");
    bool fileLink = MakeSymlink(src / L"filelink.txt", secret, false);
    bool dirLink = MakeSymlink(src / L"dirlink", base / L"outside", true);
    if (!fileLink || !dirLink) {
        std::puts("   (cannot create symlinks here: developer mode off?) skipped");
        return;
    }
    for (auto& t : kTypes) {
        fs::path arc = base / (std::wstring(L"x") + t.ext);
        CompressOptions co;
        co.type = t.type;
        co.overwrite = true;
        auto rep = CreateArchive(arc, {src}, co);
        std::string label = ToUtf8(t.label);
        auto s = ArchiveSource::Open(arc);
        bool leaked = false;
        for (auto& e : s->Entries()) if (e.path.find(L"secret") != std::wstring::npos || e.path.find(L"dirlink/") != std::wstring::npos) leaked = true;
        EXPECT_MSG(!leaked, (label + ": a symlink target was followed into the archive").c_str());
        EXPECT_MSG(rep.skipped.size() == 2, (label + ": both links reported as skipped (" + std::to_string(rep.skipped.size()) + ")").c_str());
    }
    // Extraction: a tar with a symlink entry followed by a file "through" it (the classic symlink-escape).
    // Built by hand: two tar members.
    auto tarHeader = [](const std::string& name, char type, const std::string& link, size_t size) {
        std::string h(512, '\0');
        auto put = [&](size_t off, const std::string& s) { std::memcpy(&h[off], s.data(), std::min<size_t>(s.size(), 100)); };
        put(0, name);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%07o", 0644); put(100, buf);
        std::snprintf(buf, sizeof buf, "%07o", 0); put(108, buf); put(116, buf);
        std::snprintf(buf, sizeof buf, "%011o", static_cast<unsigned>(size)); put(124, buf);
        std::snprintf(buf, sizeof buf, "%011o", 0); put(136, buf);
        h[156] = type;
        put(157, link);
        put(257, "ustar"); h[263] = '0'; h[264] = '0';
        std::memset(&h[148], ' ', 8);
        unsigned sum = 0;
        for (unsigned char c : h) sum += c;
        std::snprintf(buf, sizeof buf, "%06o", sum);
        std::memcpy(&h[148], buf, 6); h[154] = '\0'; h[155] = ' ';
        return h;
    };
    std::string content = "pwned";
    std::string tar = tarHeader("evil", '2', (base / L"escape_target").string(), 0) + tarHeader("evil/pwn.txt", '0', "", content.size()) + content + std::string(512 - content.size(), '\0') + std::string(1024, '\0');
    fs::create_directories(base / L"escape_target");
    WriteText(base / L"evil.tar", tar);
    auto s = ArchiveSource::Open(base / L"evil.tar");
    fs::path dest = Fresh(L"sl_dest");
    auto rep = ExtractTo(*s, dest, {});
    s.reset();
    EXPECT_MSG(!rep.skipped.empty() && !fs::is_regular_file(dest / L"evil"), ("a tar symlink entry is skipped, not written as a file: " + Join(ListAll(dest))).c_str());
    EXPECT_MSG(ListAll(base / L"escape_target").empty(), ("a file was written through an archived symlink: " + Join(ListAll(base / L"escape_target"))).c_str());
    std::printf("   tar symlink-escape: extracted %llu files, skipped %zu, rejected %zu, errors %zu\n", static_cast<unsigned long long>(rep.files), rep.skipped.size(), rep.rejected.size(), rep.errors.size());
    std::printf("   dest holds: %s | first error: %s\n", Join(ListAll(dest)).c_str(), rep.errors.empty() ? "-" : ToUtf8(rep.errors[0]).c_str());
}

static void TestConcurrency() {
    std::puts("-- concurrent jobs");
    fs::path src = Fresh(L"cn_src");
    WriteRandom(src / "a.bin", 3 << 20, 7);
    WriteText(src / "b.txt", std::string(50000, 'q'));
    // 1) several tar.* compressions at once (they share the temp folder for the intermediate tar)
    {
        fs::path out = Fresh(L"cn_out");
        std::atomic<int> failed{0};
        std::vector<std::thread> th;
        std::vector<std::string> msgs(8);
        for (int i = 0; i < 8; ++i)
            th.emplace_back([&, i] {
                try {
                    CompressOptions co;
                    co.type = i % 2 ? ArchiveType::TarGz : ArchiveType::TarXz;
                    auto rep = CreateArchive(out / (L"t" + std::to_wstring(i) + (i % 2 ? L".tar.gz" : L".tar.xz")), {src}, co);
                    if (!rep.ok()) { ++failed; msgs[i] = "report not ok"; }
                } catch (const Error& e) { ++failed; msgs[i] = e.what(); }
            });
        for (auto& t : th) t.join();
        std::string all;
        for (auto& m : msgs) if (!m.empty()) all += m + "; ";
        EXPECT_MSG(failed == 0, ("parallel tar.* compression: " + std::to_string(failed.load()) + " of 8 failed: " + all).c_str());
        EXPECT_MSG(TempLeftovers().empty(), ("temp left: " + Join(TempLeftovers())).c_str());
    }
    // 2) several extractions of the same archive into the same folder with overwrite, and into different folders
    {
        fs::path arc = g_root / L"cn.7z";
        fs::remove(arc);
        CompressOptions co;
        co.type = ArchiveType::SevenZip;
        co.overwrite = true;
        CreateArchive(arc, {src}, co);
        fs::path same = Fresh(L"cn_same");
        std::atomic<int> failed{0};
        std::vector<std::thread> th;
        for (int i = 0; i < 6; ++i)
            th.emplace_back([&, i] {
                try {
                    auto s = ArchiveSource::Open(arc);
                    ExtractOptions eo;
                    eo.overwrite = OverwriteMode::Overwrite;
                    auto rep = ExtractTo(*s, i < 3 ? same : g_root / (L"cn_d" + std::to_wstring(i)), eo);
                    if (!rep.ok() && i >= 3) ++failed;  // distinct folders must never fail; the shared one may report sharing errors
                } catch (const Error&) { if (i >= 3) ++failed; }
            });
        for (auto& t : th) t.join();
        EXPECT_MSG(failed == 0, "extractions into different folders succeed concurrently");
        // the shared folder must end with intact files (never a mix)
        std::error_code ec;
        const auto size = fs::file_size(same / L"cn_src" / L"a.bin", ec);
        EXPECT_MSG(!ec && size == (3u << 20), "the shared destination holds complete files");
    }
    // 3) read entries while another thread extracts from a second handle of the same archive
    {
        fs::path arc = g_root / L"cn.zip";
        fs::remove(arc);
        CompressOptions co;
        co.overwrite = true;
        CreateArchive(arc, {src}, co);
        std::atomic<bool> stop{false};
        std::atomic<int> bad{0};
        std::thread reader([&] {
            try {
                while (!stop) {
                    auto s = ArchiveSource::Open(arc);
                    struct Sink : ByteSink { size_t n = 0; void Write(const void*, size_t k) override { n += k; } } sink;
                    for (auto& e : s->Entries()) if (!e.isDir) s->ReadEntry(e.index, sink);
                }
            } catch (...) { ++bad; }
        });
        for (int i = 0; i < 5; ++i) {
            auto s = ArchiveSource::Open(arc);
            auto rep = ExtractTo(*s, Fresh(L"cn_x"), {});
            if (!rep.ok()) ++bad;
        }
        stop = true;
        reader.join();
        EXPECT_MSG(bad == 0, "reading and extracting the same archive concurrently");
    }
}

static void TestManyVolumes() {
    std::puts("-- more than 999 volumes");
    fs::path src = Fresh(L"mv_src");
    WriteRandom(src / "big.bin", 70 << 20, 9);  // 70 MB / 64 KB = 1120 volumes
    for (auto t : {ArchiveType::SevenZip, ArchiveType::Zip}) {
        fs::path out = Fresh(L"mv_out");
        CompressOptions co;
        co.type = t;
        co.level = 0;
        co.volumeSize = 64 * 1024;
        auto rep = CreateArchive(out / (t == ArchiveType::Zip ? L"v.zip" : L"v.7z"), {src}, co);
        std::string label = t == ArchiveType::Zip ? "zip" : "7z";
        EXPECT_MSG(rep.outputs.size() > 1000, (label + " volume count " + std::to_string(rep.outputs.size())).c_str());
        if (rep.outputs.size() > 1000) std::printf("   %s: first=%s 1000th=%s last=%s\n", label.c_str(), ToUtf8(rep.outputs.front().filename().wstring()).c_str(),
                                                    ToUtf8(rep.outputs[999].filename().wstring()).c_str(), ToUtf8(rep.outputs.back().filename().wstring()).c_str());
        try {
            auto s = ArchiveSource::Open(rep.outputs.front());
            fs::path dest = Fresh(L"mv_dest");
            auto er = ExtractTo(*s, dest, {});
            std::error_code ec;
            const auto size = fs::file_size(dest / L"mv_src" / L"big.bin", ec);
            EXPECT_MSG(er.ok() && !ec && size == (70u << 20), (label + ": extract across > 999 volumes").c_str());
        } catch (const Error& e) {
            EXPECT_MSG(false, (label + ": open/extract across > 999 volumes: " + e.what()).c_str());
        }
        // cancel deletes every volume
        fs::path out2 = Fresh(L"mv_out2");
        co.progress = [&](uint64_t d, uint64_t tot) { return !(d * 2 > tot); };
        try { CreateArchive(out2 / (t == ArchiveType::Zip ? L"v.zip" : L"v.7z"), {src}, co); } catch (const Error&) {}
        EXPECT_MSG(ListAll(out2).empty(), (label + " volumes left after cancel: " + std::to_string(ListAll(out2).size())).c_str());
    }
}


// ---------------------------------------------------------------------------------------------------------
// Name-encoding detection on synthetic but realistic names: random names built from common characters of each
// language, encoded with the legacy code page, as an old zip would store them.
static std::string EncodeCp(const std::wstring& w, unsigned cp) {
    int n = WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

static void TestEncodingCorpus() {
    std::puts("-- name-encoding detection on synthetic corpora");
    struct Lang { const char* label; unsigned cp; std::wstring pool; };
    const Lang langs[] = {
        {"Big5 (Traditional Chinese)", kCpBig5, L"的一是不了人我在有他這為之大來以個中上們到說國和地也子時道出而要於就下得可你年生自會那後能對著事其裡所去行過家十用發天如然作方成者多日都三小軍二無同麼經法當起與好看學進種將還分此心前面又定見只主沒公從資料夾測試報告文件會議紀錄使用手冊圖片音樂影片備份專案設計說明書客戶訂單發票照片相簿論文簡報表格程式碼"},
        {"GBK (Simplified Chinese)", kCpGbk, L"的一是不了人我在有他这为之大来以个中上们到说国和地也子时道出而要于就下得可你年生自会那后能对着事其里所去行过家十用发天如然作方成者多日都三小军二无同么经法当起与好看学进种将还分此心前面又定见只主没公从资料夹测试报告文件会议记录使用手册图片音乐视频备份项目设计说明书客户订单发票照片相册论文简报表格程序代码"},
        {"Shift-JIS (Japanese)", kCpShiftJis, L"日本語設計書資料説明会議事録写真音楽動画文書仕様顧客注文請求ひらがなカタカナのをにはがでとしてこれそれあるいうえおかきくけこさすせそたちつてなぬねまみむめもやゆよらりるれろわんテストファイルフォルダバックアッププロジェクト"},
        {"EUC-KR (Korean)", kCpEucKr, L"한국어파일프로젝트설계문서안녕하세요사진음악동영상백업자료회의록보고서고객주문청구서테스트폴더문서작성발표자료"},
    };
    const unsigned acps[] = {950, 936, 932, 949, 1252};
    const wchar_t* exts[] = {L".txt", L".docx", L".jpg", L".pdf", L".mp3"};
    std::mt19937 rng(2026);
    auto name = [&](const Lang& l, int minLen, int maxLen) {
        std::wstring n;
        int len = minLen + static_cast<int>(rng() % static_cast<unsigned>(maxLen - minLen + 1));
        for (int i = 0; i < len; ++i) n += l.pool[rng() % l.pool.size()];
        if (rng() % 3 == 0) n += L"_" + std::to_wstring(rng() % 100);
        n += exts[rng() % 5];
        if (rng() % 3 == 0) {
            std::wstring dir;
            for (int i = 0; i < 2 + static_cast<int>(rng() % 3); ++i) dir += l.pool[rng() % l.pool.size()];
            n = dir + L"/" + n;
        }
        return n;
    };
    for (auto& l : langs) {
        for (unsigned acp : acps) {
            int okMany = 0, okOne = 0, okTwo = 0, ambiguousMany = 0, silentWrong = 0, silentWrongOne = 0;
            const int trials = 300;
            for (int t = 0; t < trials; ++t) {
                std::vector<std::string> many, two, one;
                for (int i = 0; i < 8; ++i) many.push_back(EncodeCp(name(l, 2, 8), l.cp));
                two = {EncodeCp(name(l, 2, 6), l.cp), EncodeCp(name(l, 2, 6), l.cp)};
                one = {EncodeCp(name(l, 2, 6), l.cp)};
                auto g1 = DetectNameEncoding(many, acp);
                if (g1.codepage == l.cp) ++okMany;
                if (g1.ambiguous) ++ambiguousMany;
                auto g2 = DetectNameEncoding(two, acp);
                auto g3 = DetectNameEncoding(one, acp);
                if (g2.codepage == l.cp) ++okTwo;
                else if (!g2.ambiguous) ++silentWrong;  // wrong and the UI would not ask the user to confirm
                if (g3.codepage == l.cp) ++okOne;
                else if (!g3.ambiguous) ++silentWrongOne;
            }
            std::printf("   %-27s acp=%-4u 8 names: %5.1f%% (ambiguous %2d%%)  2 names: %5.1f%% (silently wrong %4.1f%%)  1 name: %5.1f%% (silently wrong %4.1f%%)\n",
                        l.label, acp, 100.0 * okMany / trials, 100 * ambiguousMany / trials, 100.0 * okTwo / trials, 100.0 * silentWrong / trials,
                        100.0 * okOne / trials, 100.0 * silentWrongOne / trials);
            // Simplified Chinese is the hard case (GB2312 bytes also look like plausible Big5); known and flagged as ambiguous.
            const int need = l.cp == kCpGbk ? 90 : 98;
            EXPECT_MSG(okMany >= trials * need / 100, (std::string(l.label) + ": detection with 8 names too low").c_str());
            EXPECT_MSG(silentWrong * 100 <= trials * 5 && silentWrongOne * 100 <= trials * 8,
                       (std::string(l.label) + ": too many wrong guesses that are not flagged as ambiguous").c_str());
        }
    }
}

// ---------------------------------------------------------------------------------------------------------
// Password compatibility with 7-Zip (the reference implementation): special characters, non-ASCII, long.
static std::wstring QuoteArg(const std::wstring& a) {
    // 7-Zip parses its own command line and treats backslashes literally: plain quoting when no quote is inside
    if (a.find(L'"') == std::wstring::npos) return L"\"" + a + L"\"";
    std::wstring q = L"\"";
    for (size_t i = 0; i < a.size(); ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') { ++bs; ++i; }
        if (i == a.size()) { q.append(bs * 2, L'\\'); break; }
        if (a[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; }
        else { q.append(bs, L'\\'); q += a[i]; }
    }
    return q + L"\"";
}

static int RunExe(const std::wstring& exe, const std::vector<std::wstring>& args, const fs::path& cwd) {
    std::wstring cmd = QuoteArg(exe);
    for (auto& a : args) cmd += L" " + QuoteArg(a);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD rc = 99;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(rc);
}

static void TestPasswordCompat() {
    std::puts("-- passwords: compatibility with 7-Zip in both directions");
    const std::wstring sevenZip = L"C:\\Program Files\\7-Zip\\7z.exe";
    if (!fs::exists(sevenZip)) { std::puts("   (7z.exe not installed) skipped"); return; }
    struct Pw { const wchar_t* label; std::wstring pw; bool cmdlineSafe; };
    const Pw pws[] = {
        {L"plain", L"secret123", true},
        {L"space inside", L"two words here", true},
        {L"single space", L" ", true},
        {L"punctuation", L"^%&!$#@()[]{}<>|;,.=+-_~`", true},
        {L"double quote", L"say \"hi\" now", false},
        {L"single quote", L"it's", true},
        {L"backslashes", L"C:\\path\\to\\", true},
        {L"Latin accents", L"\u00fcn\u00efc\u00f6d\u00e9 \u00e5\u00df", true},
        {L"Chinese", L"\u4e2d\u6587\u5bc6\u78bc\u6e2c\u8a66", true},
        {L"Japanese", L"\u65e5\u672c\u8a9e\u30d1\u30b9\u30ef\u30fc\u30c9", true},
        {L"emoji (non-BMP)", L"pw\U0001F600\U0001F512end", true},
        {L"200 characters", std::wstring(200, L'k'), true},
    };
    struct Fmt { const char* label; ArchiveType type; Profile profile; bool encNames; const wchar_t* ext; std::vector<std::wstring> sevenArgs; };
    const Fmt fmts[] = {
        {"zip AES-256", ArchiveType::Zip, Profile::Balanced, false, L".zip", {L"-tzip", L"-mem=AES256"}},
        {"zip ZipCrypto", ArchiveType::Zip, Profile::Compatible, false, L".zip", {L"-tzip", L"-mem=ZipCrypto"}},
        {"7z", ArchiveType::SevenZip, Profile::Balanced, false, L".7z", {L"-t7z"}},
        {"7z + encrypted names", ArchiveType::SevenZip, Profile::Balanced, true, L".7z", {L"-t7z", L"-mhe=on"}},
    };
    fs::path src = Fresh(L"pw_src");
    WriteText(src / "data.txt", "the quick brown fox");
    fs::path work = Fresh(L"pw_work");
    int bad = 0;
    std::printf("   %-22s", "");
    for (auto& f : fmts) std::printf(" | %-27s", f.label);
    std::puts("");
    for (auto& p : pws) {
        std::printf("   %-22s", ToUtf8(p.label).c_str());
        for (auto& f : fmts) {
            // A: we create, 7-Zip extracts
            std::string a = "-", bres = "-";
            {
                fs::path arc = work / (std::wstring(L"a") + f.ext);
                fs::remove(arc);
                CompressOptions co;
                co.type = f.type;
                co.profile = f.profile;
                co.password = p.pw;
                co.encryptFileNames = f.encNames;
                co.overwrite = true;
                bool created = true;
                std::string refusal;
                try { CreateArchive(arc, {src}, co); } catch (const Error& e) { created = false; refusal = e.what(); }
                if (created && p.cmdlineSafe) {
                    fs::path out = Fresh(L"pw_out");
                    int rc = RunExe(sevenZip, {L"x", L"-y", L"-p" + p.pw, L"-o" + out.wstring(), arc.wstring()}, work);
                    const bool okc = rc == 0 && ReadAll(out / L"pw_src" / L"data.txt") == "the quick brown fox";
                    a = okc ? "ok" : "FAIL";
                    if (!okc) ++bad;
                } else if (!created) {
                    // 7-Zip's zip encryption is ASCII-only (and AES takes at most 99 characters): refusing is right,
                    // but the message must say why instead of a generic failure.
                    const bool zip = f.type == ArchiveType::Zip;
                    const bool clear = zip && refusal.find("ZIP") != std::string::npos;
                    a = clear ? "refused (clear msg)" : "NOCREATE";
                    if (!clear) { ++bad; std::fprintf(stderr, "      [%s %s] unclear refusal: %s\n", ToUtf8(p.label).c_str(), f.label, refusal.c_str()); }
                }
                // our own round trip
                if (created) {
                    try {
                        OpenOptions oo;
                        oo.password = [&](std::wstring& o) { o = p.pw; return true; };
                        auto s = ArchiveSource::Open(arc, oo);
                        ExtractOptions eo;
                        eo.password = oo.password;
                        fs::path out = Fresh(L"pw_out2");
                        auto rep = ExtractTo(*s, out, eo);
                        if (!(rep.ok() && ReadAll(out / L"pw_src" / L"data.txt") == "the quick brown fox")) { a += "/selfFAIL"; ++bad; }
                        // the wrong password must be refused
                        bool refused = false;
                        try {
                            OpenOptions wo;
                            wo.password = [&](std::wstring& o) { o = p.pw + L"x"; return true; };
                            auto s2 = ArchiveSource::Open(arc, wo);
                            ExtractOptions e2;
                            e2.password = wo.password;
                            auto r2 = ExtractTo(*s2, Fresh(L"pw_out3"), e2);
                            refused = !r2.ok() || r2.wrongPassword;
                        } catch (const Error&) { refused = true; }
                        if (!refused) { a += "/wrongpwOK!"; ++bad; }
                    } catch (const Error& e) { a += "/selfERR"; ++bad; std::fprintf(stderr, "      [%s %s] %s\n", ToUtf8(p.label).c_str(), f.label, e.what()); }
                }
            }
            // B: 7-Zip creates, we extract
            if (p.cmdlineSafe) {
                fs::path arc = work / (std::wstring(L"b") + f.ext);
                fs::remove(arc);
                std::vector<std::wstring> args = {L"a", L"-y", L"-p" + p.pw};
                for (auto& x : f.sevenArgs) args.push_back(x);
                args.push_back(arc.wstring());
                args.push_back((src / L"data.txt").wstring());
                int rc = RunExe(sevenZip, args, work);
                if (rc == 0) {
                    try {
                        OpenOptions oo;
                        oo.password = [&](std::wstring& o) { o = p.pw; return true; };
                        auto s = ArchiveSource::Open(arc, oo);
                        ExtractOptions eo;
                        eo.password = oo.password;
                        fs::path out = Fresh(L"pw_out4");
                        auto rep = ExtractTo(*s, out, eo);
                        const bool okc = rep.ok() && ReadAll(out / L"data.txt") == "the quick brown fox";
                        bres = okc ? "ok" : "FAIL";
                        if (!okc) ++bad;
                    } catch (const Error&) { bres = "ERR"; ++bad; }
                } else { bres = "no7z"; }
            }
            std::printf(" | we->7z %-10s 7z->we %-4s", a.c_str(), bres.c_str());
        }
        std::puts("");
    }
    EXPECT_MSG(bad == 0, (std::to_string(bad) + " password combinations are not interoperable").c_str());
}

// Opening a double-layer archive (tar.gz) expands it to a scratch file first: cancel and disk-full during that step.
static void TestOpenScratch() {
    std::puts("-- opening tar.gz: scratch file is cleaned up on cancel and on disk-full");
    fs::path src = Fresh(L"os_src");
    WriteRandom(src / "a.bin", 12 << 20, 21);
    fs::path arc = g_root / L"os.tar.gz";
    fs::remove(arc);
    CompressOptions co;
    co.type = ArchiveType::TarGz;
    co.overwrite = true;
    CreateArchive(arc, {src}, co);
    {
        OpenOptions oo;
        int calls = 0;
        oo.progress = [&](uint64_t d, uint64_t t) { ++calls; return !(calls > 1 && d * 4 > t); };
        bool cancelled = false;
        try { auto s = ArchiveSource::Open(arc, oo); } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT_MSG(cancelled, "cancelling while a tar.gz is being opened throws a cancel error");
        EXPECT_MSG(TempLeftovers().empty(), ("scratch left after cancel: " + Join(TempLeftovers())).c_str());
    }
    {
        SetTestWriteFault(1 << 20);
        bool threw = false, diskFull = false;
        std::string what;
        try { auto s = ArchiveSource::Open(arc); } catch (const Error& e) { threw = true; diskFull = e.diskFull(); what = e.what(); }
        SetTestWriteFault(0);
        EXPECT_MSG(threw, "opening a tar.gz fails (not silently shows a broken view) when the scratch disk is full");
        EXPECT_MSG(diskFull, ("recognised as disk-full: " + what).c_str());
        EXPECT_MSG(TempLeftovers().empty(), ("scratch left after disk-full: " + Join(TempLeftovers())).c_str());
        std::printf("   message: %s\n", what.c_str());
    }
    {
        // several opens at once (multi-window batch): each must get its own scratch file
        std::atomic<int> bad{0};
        std::vector<std::thread> th;
        for (int i = 0; i < 6; ++i)
            th.emplace_back([&] {
                try {
                    auto s = ArchiveSource::Open(arc);
                    bool hasFile = false;
                    for (auto& e : s->Entries()) if (e.path.find(L"a.bin") != std::wstring::npos) hasFile = true;
                    bool tarView = s->FormatName().find(L"tar") != std::wstring::npos && hasFile;
                    if (!tarView) ++bad;
                } catch (...) { ++bad; }
            });
        for (auto& t : th) t.join();
        EXPECT_MSG(bad == 0, (std::to_string(bad.load()) + " of 6 simultaneous opens of a tar.gz did not show the inner tar").c_str());
    }
}

// ---------------------------------------------------------------------------------------------------------
// Heavy: entries larger than 4 GB (32-bit overflow in sizes and progress) and tar members over 8 GB. Needs ~25 GB of
// free disk space and several minutes; only runs with ROBUST_HEAVY=1 (see scripts\robust-heavy.ps1).
static void MakeZeroFile(const fs::path& p, uint64_t bytes) {
    fs::create_directories(p.parent_path());
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER sz;
    sz.QuadPart = static_cast<LONGLONG>(bytes);
    SetFilePointerEx(h, sz, nullptr, FILE_BEGIN);
    SetEndOfFile(h);
    CloseHandle(h);
}

static void TestHugeFiles() {
    std::puts("-- entries larger than 4 GB (and a tar member over 8 GB)");
    fs::path src = Fresh(L"huge_src");
    struct Case { const wchar_t* label; ArchiveType type; uint64_t bytes; int level; };
    const Case cases[] = {
        {L"zip 5 GB", ArchiveType::Zip, 5ull << 30, 1},
        {L"7z 5 GB", ArchiveType::SevenZip, 5ull << 30, 1},
        {L"tar.gz 5 GB", ArchiveType::TarGz, 5ull << 30, 1},
        {L"tar 9 GB (member > 8 GB)", ArchiveType::Tar, 9ull << 30, 0},
    };
    for (auto& c : cases) {
        const std::string label = ToUtf8(c.label);
        fs::remove_all(ToLongPath(src));
        MakeZeroFile(src / L"big.bin", c.bytes);
        WriteText(src / L"small.txt", "tail file after the big one");
        fs::path arc = g_root / (std::wstring(L"huge") + kTypes[0].ext);
        for (auto& t : kTypes) if (t.type == c.type) arc = g_root / (std::wstring(L"huge") + t.ext);
        fs::remove(arc);

        // compress: progress must stay within [0, total], never go backwards, and end at the total
        uint64_t lastDone = 0, lastTotal = 0;
        bool backwards = false, over = false;
        CompressOptions co;
        co.type = c.type;
        co.level = c.level;
        co.overwrite = true;
        co.progress = [&](uint64_t d, uint64_t t) {
            if (d < lastDone) backwards = true;
            if (d > t) over = true;
            lastDone = d;
            lastTotal = t;
            return true;
        };
        const auto t0 = GetTickCount64();
        auto rep = CreateArchive(arc, {src}, co);
        const auto t1 = GetTickCount64();
        EXPECT_MSG(rep.ok() && rep.bytesIn >= c.bytes, (label + ": compress report").c_str());
        EXPECT_MSG(!backwards, (label + ": compress progress went backwards").c_str());
        EXPECT_MSG(!over, (label + ": compress progress exceeded the total").c_str());
        EXPECT_MSG(lastTotal >= c.bytes, (label + ": progress total smaller than the data (32-bit overflow?)").c_str());
        std::printf("   %-26s compressed in %llu s -> %llu bytes (progress total %llu)\n", label.c_str(),
                    static_cast<unsigned long long>((t1 - t0) / 1000), static_cast<unsigned long long>(rep.bytesOut), static_cast<unsigned long long>(lastTotal));

        // list + stream (cat) the big entry, then extract and compare size
        auto s = ArchiveSource::Open(arc);
        uint32_t bigIndex = UINT32_MAX;
        for (auto& e : s->Entries()) if (e.path.find(L"big.bin") != std::wstring::npos) { bigIndex = e.index; EXPECT_MSG(e.size == c.bytes, (label + ": listed size").c_str()); }
        EXPECT_MSG(bigIndex != UINT32_MAX, (label + ": big entry listed").c_str());
        if (bigIndex != UINT32_MAX) {
            struct Count : ByteSink { uint64_t n = 0; uint64_t nonzero = 0; void Write(const void* d, size_t k) override { n += k; if (k && static_cast<const char*>(d)[0] != 0) ++nonzero; } } sink;
            uint64_t pl = 0;
            bool pback = false;
            s->ReadEntry(bigIndex, sink, [&](uint64_t d, uint64_t) { if (d < pl) pback = true; pl = d; return true; });
            EXPECT_MSG(sink.n == c.bytes, (label + ": streamed byte count " + std::to_string(sink.n)).c_str());
            EXPECT_MSG(!pback, (label + ": read progress went backwards").c_str());
        }
        fs::path dest = Fresh(L"huge_dest");
        uint64_t xl = 0;
        bool xback = false;
        ExtractOptions eo;
        eo.progress = [&](uint64_t d, uint64_t) { if (d < xl) xback = true; xl = d; return true; };
        auto er = ExtractTo(*s, dest, eo);
        s.reset();
        std::error_code ec;
        uint64_t got = 0;
        for (auto& rel : ListAll(dest)) if (fs::path(rel).filename() == L"big.bin") got = fs::file_size(ToLongPath(dest / rel), ec);
        EXPECT_MSG(er.ok() && got == c.bytes, (label + ": extracted size " + std::to_string(got)).c_str());
        EXPECT_MSG(!xback, (label + ": extract progress went backwards").c_str());
        fs::remove_all(ToLongPath(dest), ec);
        fs::remove(arc, ec);
    }
    std::error_code ec;
    fs::remove_all(ToLongPath(src), ec);
}

// tar.* is staged as a plain tar first (headers and padding make it larger than the input): progress must still
// stay within [0, total] and never go backwards. Many tiny files make the overhead large.
static void TestTarProgress() {
    std::puts("-- tar.* progress stays within the total");
    fs::path src = Fresh(L"tp_src");
    for (int i = 0; i < 1500; ++i) WriteText(src / (L"f" + std::to_wstring(i) + L".txt"), std::string(100, 'a' + i % 20));
    for (auto& t : kTypes) {
        if (t.type == ArchiveType::Zip || t.type == ArchiveType::SevenZip) continue;
        uint64_t lastDone = 0, lastTotal = 0;
        bool backwards = false, over = false;
        CompressOptions co;
        co.type = t.type;
        co.overwrite = true;
        co.progress = [&](uint64_t d, uint64_t tot) {
            if (d < lastDone) backwards = true;
            if (d > tot) over = true;
            lastDone = d;
            lastTotal = tot;
            return true;
        };
        CreateArchive(g_root / (std::wstring(L"tp") + t.ext), {src}, co);
        std::string label = ToUtf8(t.label);
        EXPECT_MSG(!over, (label + ": progress exceeded the total").c_str());
        EXPECT_MSG(!backwards, (label + ": progress went backwards").c_str());
        EXPECT_MSG(lastDone * 100 >= lastTotal * 95, (label + ": progress did not reach the end").c_str());
    }
}

// Relative input paths ("." / ".." / sub\..) must resolve to the real folder name instead of ending up in the
// archive as entries starting with a dot component.
static void TestRelativeInputs() {
    std::puts("-- relative input paths ('.', '..')");
    fs::path base = Fresh(L"ri");
    WriteText(base / L"proj" / L"a.txt", "A");
    WriteText(base / L"proj" / L"sub" / L"b.txt", "B");
    wchar_t old[MAX_PATH];
    GetCurrentDirectoryW(MAX_PATH, old);
    SetCurrentDirectoryW((base / L"proj").c_str());
    struct Case { const wchar_t* label; const wchar_t* input; const wchar_t* expectTop; };
    const Case cases[] = {{L".", L".", L"proj"}, {L"..", L"..", L"ri"}, {L".\\", L".\\", L"proj"}, {L"sub\\..", L"sub\\..", L"proj"}};
    for (auto& c : cases) {
        for (auto t : {ArchiveType::Zip, ArchiveType::SevenZip}) {
            fs::path out = base / (std::wstring(L"out_") + (t == ArchiveType::Zip ? L"z" : L"s") + std::to_wstring(&c - cases) + (t == ArchiveType::Zip ? L".zip" : L".7z"));
            CompressOptions co;
            co.type = t;
            co.overwrite = true;
            std::string label = std::string("input '") + ToUtf8(c.label) + "' " + (t == ArchiveType::Zip ? "zip" : "7z");
            try {
                CreateArchive(out, {fs::path(c.input)}, co);
                auto s = ArchiveSource::Open(out);
                bool bad = false, top = false;
                for (auto& e : s->Entries()) {
                    if (e.path.rfind(L"./", 0) == 0 || e.path.rfind(L"../", 0) == 0 || e.path == L"." || e.path == L"..") bad = true;
                    if (e.path.rfind(std::wstring(c.expectTop) + L"/", 0) == 0 || e.path == c.expectTop) top = true;
                }
                EXPECT_MSG(!bad && top, (label + ": entry names must start with the folder name '" + ToUtf8(c.expectTop) + "'").c_str());
                // and the archive must extract completely
                auto er = ExtractTo(*s, Fresh(L"ri_dest"), {});
                EXPECT_MSG(er.ok() && er.rejected.empty() && er.files >= 2, (label + ": extraction has rejected entries").c_str());
            } catch (const Error& e) {
                EXPECT_MSG(false, (label + ": " + e.what()).c_str());
            }
        }
    }
    SetCurrentDirectoryW(old);
}

// Pathological names in the folder tree (built for every opened archive): one very deep path, a huge flat folder,
// many hostile names. Runs on a thread with the default 1 MB stack (like the UI thread) to catch deep recursion, and
// watches the memory it needs.
static size_t PeakWorkingSet() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    return pmc.PeakWorkingSetSize;
}

static void TestPathologicalTree() {
    std::puts("-- folder tree with pathological names");
    auto runOnSmallStack = [](auto fn) {
        struct Ctx { decltype(fn)* f; };
        Ctx c{&fn};
        HANDLE h = CreateThread(nullptr, 1 << 20, [](LPVOID p) -> DWORD { (*static_cast<Ctx*>(p)->f)(); return 0; }, &c, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        WaitForSingleObject(h, 120000);
        CloseHandle(h);
    };
    // 1. one entry with 30000 path components (a name of ~60 KB fits in a zip)
    {
        std::wstring deep;
        for (int i = 0; i < 30000; ++i) deep += L"a/";
        deep += L"file.txt";
        std::vector<Entry> entries(1);
        entries[0].path = deep;
        entries[0].size = 1;
        const size_t before = PeakWorkingSet();
        const auto t0 = GetTickCount64();
        bool ok = true;
        runOnSmallStack([&] {
            ArchiveTree tree(entries);
            auto d = tree.Descendants(tree.Root());
            (void)tree.Search(tree.Root(), L"file");
            ok = !d.empty();
        });
        const size_t grew = (PeakWorkingSet() - before) >> 20;
        std::printf("   30000-component path: %llu ms, +%zu MB\n", static_cast<unsigned long long>(GetTickCount64() - t0), grew);
        EXPECT_MSG(ok, "deep path: tree built and traversed");
        EXPECT_MSG(grew < 200, ("deep path: the tree needs too much memory: +" + std::to_string(grew) + " MB").c_str());
    }
    // 2. 300000 siblings in one folder + 100000 nested folders
    {
        std::vector<Entry> entries;
        for (uint32_t i = 0; i < 300000; ++i) { Entry e; e.index = i; e.path = L"flat/file" + std::to_wstring(i) + L".txt"; e.size = 1; entries.push_back(e); }
        for (uint32_t i = 0; i < 100000; ++i) { Entry e; e.index = 300000 + i; e.path = L"d" + std::to_wstring(i % 1000) + L"/s" + std::to_wstring(i / 1000) + L"/f.txt"; e.size = 1; entries.push_back(e); }
        const auto t0 = GetTickCount64();
        runOnSmallStack([&] {
            ArchiveTree tree(entries);
            auto flat = tree.Find(L"flat");
            (void)tree.Children(flat, SortKey::Name, true);
            (void)tree.Children(flat, SortKey::Size, false);
            (void)tree.Descendants(tree.Root(), SortKey::Path, true, false);
            (void)tree.Search(tree.Root(), L"file12");
        });
        const auto ms = GetTickCount64() - t0;
        std::printf("   400000 entries: %llu ms\n", static_cast<unsigned long long>(ms));
        EXPECT_MSG(ms < 60000, "400000 entries: tree operations finish in reasonable time");
    }
}

// API misuse and hostile callbacks: bad indices, odd inputs, callbacks that throw. None may crash or hang.
static void TestApiMisuse() {
    std::puts("-- bad indices, odd inputs, throwing callbacks");
    fs::path src = Fresh(L"mu_src");
    WriteText(src / "a.txt", "alpha");
    WriteText(src / "b.txt", "bravo");
    fs::path arc = g_root / L"mu.zip";
    fs::remove(arc);
    CompressOptions co;
    co.overwrite = true;
    CreateArchive(arc, {src}, co);
    struct Count : ByteSink { size_t n = 0; void Write(const void*, size_t k) override { n += k; } };

    // 1. indices out of range
    {
        auto s = ArchiveSource::Open(arc);
        bool threw = false;
        Count c;
        try { s->ReadEntry(99999, c); } catch (const Error&) { threw = true; }
        EXPECT_MSG(threw, "ReadEntry with an index out of range reports an error");
        try { s->ReadEntry(UINT32_MAX, c); } catch (const Error&) {}
        auto rep = ExtractTo(*s, Fresh(L"mu_dest"), {}, {99999, UINT32_MAX});
        EXPECT_MSG(rep.files == 0, "ExtractTo with only invalid indices extracts nothing");
        auto rep2 = ExtractTo(*s, Fresh(L"mu_dest"), {}, {1, 1, 1});
        EXPECT_MSG(rep2.ok(), "duplicate indices are harmless");
    }
    // 2. things that are not archives
    {
        const fs::path dir = Fresh(L"mu_odd");
        WriteText(dir / "empty.zip", "");
        WriteText(dir / "text.7z", "this is not a 7z");
        fs::create_directories(dir / "folder.zip");
        std::vector<fs::path> odd = {dir / "empty.zip", dir / "text.7z", dir / "folder.zip", dir / "missing.zip", fs::path(L"")};
        // plain text must not open as a "Base64 archive" (signature-less formats are tried by extension only) ...
        WriteText(dir / "notes.txt", "just some plain words and numbers 12345");
        odd.push_back(dir / "notes.txt");
        // ... while a real .b64 file still opens
        WriteText(dir / "data.b64", "SGVsbG8gV29ybGQ=");
        try { auto s = ArchiveSource::Open(dir / "data.b64"); EXPECT_MSG(s->Entries().size() == 1, "a .b64 file opens as a one-entry archive"); }
        catch (const Error& e) { EXPECT_MSG(false, (std::string("a .b64 file should still open: ") + e.what()).c_str()); }
        for (auto& p : odd) {
            bool threw = false;
            try { ArchiveSource::Open(p); } catch (const Error&) { threw = true; } catch (const std::exception& e) { EXPECT_MSG(false, (std::string("non-lycan exception from Open: ") + e.what() + " for " + ToUtf8(p.wstring())).c_str()); } catch (...) { EXPECT_MSG(false, ("non-lycan exception from Open for " + ToUtf8(p.wstring())).c_str()); }
            EXPECT_MSG(threw, ("Open reports an error for " + ToUtf8(p.wstring())).c_str());
        }
    }
    // 3. callbacks that throw
    {
        auto s = ArchiveSource::Open(arc);
        ExtractOptions eo;
        eo.progress = [](uint64_t, uint64_t) -> bool { throw std::runtime_error("progress callback failed"); };
        bool threw = false;
        try { ExtractTo(*s, Fresh(L"mu_dest"), eo); } catch (const std::exception&) { threw = true; }
        EXPECT_MSG(threw, "an exception from the progress callback propagates as an exception");
    }
    {
        // password callback that throws, on an encrypted archive
        fs::path enc = g_root / L"mu_enc.7z";
        fs::remove(enc);
        CompressOptions ec;
        ec.type = ArchiveType::SevenZip;
        ec.password = L"pw";
        ec.overwrite = true;
        CreateArchive(enc, {src}, ec);
        OpenOptions oo;
        oo.password = [](std::wstring&) -> bool { throw std::runtime_error("password callback failed"); };
        bool threw = false;
        try {
            auto s = ArchiveSource::Open(enc, oo);
            ExtractOptions eo;
            eo.password = oo.password;
            ExtractTo(*s, Fresh(L"mu_dest"), eo);
        } catch (const std::exception&) { threw = true; }
        EXPECT_MSG(threw, "an exception from the password callback propagates");
        // password callback that always declines
        OpenOptions no;
        no.password = [](std::wstring&) { return false; };
        bool refused = false;
        try { auto s = ArchiveSource::Open(enc, no); ExtractOptions eo; eo.password = no.password; auto r = ExtractTo(*s, Fresh(L"mu_dest"), eo); refused = !r.ok(); }
        catch (const Error&) { refused = true; }
        EXPECT_MSG(refused, "declining the password fails the job instead of extracting garbage");
    }
    {
        // conflict callback that throws
        fs::path dest = Fresh(L"mu_dest");
        WriteText(dest / "mu_src" / "a.txt", "old");
        auto s = ArchiveSource::Open(arc);
        ExtractOptions eo;
        eo.overwrite = OverwriteMode::Ask;
        eo.onConflict = [](const ConflictInfo&) -> ConflictAnswer { throw std::runtime_error("conflict callback failed"); };
        bool threw = false;
        try { ExtractTo(*s, dest, eo); } catch (const std::exception&) { threw = true; }
        EXPECT_MSG(threw, "an exception from the conflict callback propagates");
        EXPECT_MSG(ReadAll(dest / "mu_src" / "a.txt") == "old", "the existing file is untouched when the conflict callback fails");
    }
    {
        // a sink that throws while reading an entry
        auto s = ArchiveSource::Open(arc);
        struct Bad : ByteSink { void Write(const void*, size_t) override { throw std::runtime_error("sink failed"); } } bad;
        bool threw = false;
        for (auto& e : s->Entries()) if (!e.isDir) { try { s->ReadEntry(e.index, bad); } catch (const std::exception&) { threw = true; } }
        EXPECT_MSG(threw, "an exception from a ByteSink propagates");
    }
    {
        // compress with a progress callback that throws: nothing left behind
        fs::path out = Fresh(L"mu_out");
        CompressOptions c2;
        c2.progress = [](uint64_t, uint64_t) -> bool { throw std::runtime_error("progress failed"); };
        bool threw = false;
        try { CreateArchive(out / L"x.zip", {src}, c2); } catch (const std::exception&) { threw = true; }
        EXPECT_MSG(threw, "an exception from the compress progress callback propagates");
        EXPECT_MSG(ListAll(out).empty(), ("nothing left after a throwing progress callback: " + Join(ListAll(out))).c_str());
        EXPECT_MSG(TempLeftovers().empty(), "no scratch left");
    }
    // 4. compress odd inputs
    {
        fs::path out = Fresh(L"mu_out");
        bool a = false, b = false, c = false;
        try { CreateArchive(out / L"x.zip", {fs::path(L"Z:\\definitely\\missing")}, {}); } catch (const Error&) { a = true; }
        try { CreateArchive(out / L"y.zip", {}, {}); } catch (const Error&) { b = true; }
        try { CreateArchive(fs::path(L"Z:\\no\\such\\drive\\x.zip"), {src}, {}); } catch (const Error&) { c = true; }
        EXPECT_MSG(a && b && c, "missing input / no input / unwritable output all report errors");
        EXPECT_MSG(ListAll(out).empty(), "nothing left after the failed attempts");
    }
}

// Handles and memory must not grow when the same operations are repeated (a long session opens thousands of archives).
static void TestResourceLeaks() {
    std::puts("-- repeated operations do not leak handles or memory");
    fs::path src = Fresh(L"lk_src");
    WriteText(src / "a.txt", std::string(20000, 'a'));
    WriteRandom(src / "b.bin", 300 * 1024, 31);
    fs::path zip = g_root / L"lk.zip", tgz = g_root / L"lk.tar.gz", sevenz = g_root / L"lk.7z";
    for (auto [p, t] : {std::pair{zip, ArchiveType::Zip}, std::pair{tgz, ArchiveType::TarGz}, std::pair{sevenz, ArchiveType::SevenZip}}) {
        fs::remove(p);
        CompressOptions co;
        co.type = t;
        co.overwrite = true;
        CreateArchive(p, {src}, co);
    }
    struct Count : ByteSink { size_t n = 0; void Write(const void*, size_t k) override { n += k; } };
    auto round = [&](int i) {
        for (auto& p : {zip, tgz, sevenz}) {
            auto s = ArchiveSource::Open(p);
            ArchiveTree tree(s->Entries());
            Count c;
            for (auto& e : s->Entries()) if (!e.isDir) s->ReadEntry(e.index, c);
            ExtractTo(*s, g_root / L"lk_dest", ExtractOptions{.overwrite = OverwriteMode::Overwrite});
        }
        // a cancelled extraction and a cancelled compression every time
        {
            auto s = ArchiveSource::Open(zip);
            ExtractOptions eo;
            eo.overwrite = OverwriteMode::Overwrite;
            eo.progress = [](uint64_t, uint64_t) { return false; };
            try { ExtractTo(*s, g_root / L"lk_dest", eo); } catch (const Error&) {}
            CompressOptions co;
            co.overwrite = true;
            co.progress = [](uint64_t, uint64_t) { return false; };
            try { CreateArchive(g_root / L"lk_out.zip", {src}, co); } catch (const Error&) {}
        }
        (void)i;
    };
    for (int i = 0; i < 20; ++i) round(i);  // warm-up: caches, lazily loaded DLL parts
    DWORD h0 = 0, h1 = 0;
    GetProcessHandleCount(GetCurrentProcess(), &h0);
    PROCESS_MEMORY_COUNTERS_EX m0{}, m1{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m0), sizeof m0);
    const int rounds = 150;
    for (int i = 0; i < rounds; ++i) round(i);
    GetProcessHandleCount(GetCurrentProcess(), &h1);
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&m1), sizeof m1);
    const long dHandles = static_cast<long>(h1) - static_cast<long>(h0);
    const long dMB = static_cast<long>((static_cast<long long>(m1.PrivateUsage) - static_cast<long long>(m0.PrivateUsage)) >> 20);
    std::printf("   after %d rounds (%d opens + extractions): handles %+ld, private memory %+ld MB\n", rounds, rounds * 4, dHandles, dMB);
    EXPECT_MSG(dHandles <= 10, ("handle leak: " + std::to_string(dHandles)).c_str());
    EXPECT_MSG(dMB <= 40, ("memory growth: " + std::to_string(dMB) + " MB").c_str());
    EXPECT_MSG(TempLeftovers().empty(), ("scratch left: " + Join(TempLeftovers())).c_str());
}

// Integrity test: one pass in 7-Zip's native test mode. The old per-entry ReadEntry loop was O(n^2) on solid archives
// (every entry decompressed the block from its start): 600 files of 100 KB took 16 s instead of 0.2 s.
static void TestIntegrityPass() {
    std::puts("-- integrity test (TestArchive) on solid archives");
    fs::path src = Fresh(L"it_src");
    for (int i = 0; i < 600; ++i) WriteRandom(src / (L"f" + std::to_wstring(i) + L".bin"), 100 * 1024, 100 + i);
    fs::path arc = g_root / L"it.7z";
    fs::remove(arc);
    CompressOptions co;
    co.type = ArchiveType::SevenZip;
    co.level = 1;
    co.solidSize = kSolidAll;
    co.overwrite = true;
    CreateArchive(arc, {src}, co);
    {
        auto s = ArchiveSource::Open(arc);
        uint64_t last = 0;
        bool backwards = false;
        ExtractOptions to;
        to.progress = [&](uint64_t d, uint64_t) { if (d < last) backwards = true; last = d; return true; };
        const auto t0 = GetTickCount64();
        auto rep = TestArchive(*s, to);
        const auto ms = GetTickCount64() - t0;
        std::printf("   600 files / 60 MB solid: %llu ms\n", static_cast<unsigned long long>(ms));
        EXPECT_MSG(rep.ok() && rep.files == 600, "a sound archive tests clean");
        EXPECT_MSG(ms < 3000, "the test of a solid archive is one pass, not one pass per file");
        EXPECT_MSG(!backwards, "test progress never goes backwards");
    }
    {
        // one flipped byte in the middle: reported, and the file is named
        std::string data = ReadAll(arc);
        data[data.size() / 2] ^= 0x5A;
        fs::path bad = g_root / L"it_bad.7z";
        WriteText(bad, data);
        try {
            auto s = ArchiveSource::Open(bad);
            auto rep = TestArchive(*s);
            EXPECT_MSG(!rep.ok() && !rep.errors.empty(), "a damaged archive is reported");
            if (!rep.errors.empty()) std::printf("   damaged archive: %zu problem(s), first: %s\n", rep.errors.size(), ToUtf8(rep.errors[0]).c_str());
        } catch (const Error& e) {
            std::printf("   damaged archive rejected at open: %s\n", e.what());
        }
    }
    {
        // cancel
        auto s = ArchiveSource::Open(arc);
        ExtractOptions to;
        int calls = 0;
        to.progress = [&](uint64_t, uint64_t) { return ++calls < 3; };
        bool cancelled = false;
        try { TestArchive(*s, to); } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT_MSG(cancelled, "cancelling a test throws a cancel error");
    }
    {
        // wrong password on an encrypted archive
        fs::path enc = g_root / L"it_enc.zip";
        fs::remove(enc);
        CompressOptions ec;
        ec.password = L"right";
        ec.overwrite = true;
        CreateArchive(enc, {src}, ec);
        auto s = ArchiveSource::Open(enc, OpenOptions{.password = [](std::wstring& o) { o = L"right"; return true; }});
        ExtractOptions good, bad;
        good.password = [](std::wstring& o) { o = L"right"; return true; };
        bad.password = [](std::wstring& o) { o = L"wrong"; return true; };
        auto r1 = TestArchive(*s, good);
        auto r2 = TestArchive(*s, bad);
        EXPECT_MSG(r1.ok(), "the right password tests clean");
        EXPECT_MSG(!r2.ok() && r2.wrongPassword, "the wrong password is reported as such");
    }
}

// Smart extraction scans every entry to decide the output folder. With a flat archive of many files that scan was
// O(n^2) (a linear search of the top-level names per entry): 100,000 files took 7 minutes instead of under a minute.
// A hand-built zip of 50,000 empty entries; the destination is on a drive that does not exist, so the call fails
// right after the scan and the time measured is the scan (plus listing), not disk writes.
static void WriteFlatZip(const fs::path& path, int count) {
    std::string zip, central;
    auto le16 = [](std::string& s, uint16_t v) { s.append(reinterpret_cast<const char*>(&v), 2); };
    auto le32 = [](std::string& s, uint32_t v) { s.append(reinterpret_cast<const char*>(&v), 4); };
    for (int i = 0; i < count; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "file%06d.txt", i);
        const std::string n = name;
        const uint32_t offset = static_cast<uint32_t>(zip.size());
        le32(zip, 0x04034b50); le16(zip, 20); le16(zip, 0); le16(zip, 0); le16(zip, 0); le16(zip, 0x21);
        le32(zip, 0); le32(zip, 0); le32(zip, 0); le16(zip, static_cast<uint16_t>(n.size())); le16(zip, 0);
        zip += n;
        le32(central, 0x02014b50); le16(central, 20); le16(central, 20); le16(central, 0); le16(central, 0); le16(central, 0); le16(central, 0x21);
        le32(central, 0); le32(central, 0); le32(central, 0); le16(central, static_cast<uint16_t>(n.size())); le16(central, 0); le16(central, 0);
        le16(central, 0); le16(central, 0); le32(central, 0); le32(central, offset);
        central += n;
    }
    const uint32_t cdOffset = static_cast<uint32_t>(zip.size());
    zip += central;
    le32(zip, 0x06054b50); le16(zip, 0); le16(zip, 0); le16(zip, static_cast<uint16_t>(count)); le16(zip, static_cast<uint16_t>(count));
    le32(zip, static_cast<uint32_t>(central.size())); le32(zip, cdOffset); le16(zip, 0);
    WriteText(path, zip);
}

static void TestSmartScanFlat() {
    std::puts("-- smart extraction of a flat archive with many files");
    fs::path arc = g_root / L"flat50k.zip";
    WriteFlatZip(arc, 50000);
    auto s = ArchiveSource::Open(arc);
    EXPECT_MSG(s->Entries().size() == 50000, "the hand-built zip lists 50000 entries");
    ExtractOptions eo;
    eo.smart = true;
    const auto t0 = GetTickCount64();
    bool failedAsExpected = false;
    try { ExtractTo(*s, fs::path(L"Z:\\lycan_no_such_drive\\out"), eo); } catch (const Error&) { failedAsExpected = true; }
    const auto ms = GetTickCount64() - t0;
    std::printf("   scan of 50000 entries: %llu ms\n", static_cast<unsigned long long>(ms));
    EXPECT_MSG(failedAsExpected, "extraction into a missing drive fails");
    EXPECT_MSG(ms < 5000, "the smart-extraction scan is linear, not quadratic, in the number of entries");
}

// Extracting right into the root of a drive (an archive stored at D:\ is extracted "here" = D:\). The root already
// exists; std::filesystem::create_directories reports an error for it, which made the whole extraction fail with
// "cannot create the destination folder". The archive holds one top-level folder so that only a folder is created
// in the root (ordinary users may create folders there, not files).
static void TestDriveRootDestination() {
    std::puts("-- extracting into a drive root");
    const fs::path rootDir = g_root.root_path();  // e.g. C:\ (the drive of the scratch folder)
    const std::wstring top = L"lycan_roottest_" + std::to_wstring(GetCurrentProcessId());
    fs::path src = Fresh(L"dr_src");
    WriteText(src / top / "f.txt", "in the root");
    fs::path arc = g_root / L"dr.zip";
    fs::remove(arc);
    CompressOptions co;
    co.overwrite = true;
    CreateArchive(arc, {src / top}, co);
    auto s = ArchiveSource::Open(arc);
    bool threw = false;
    std::string what;
    ExtractReport rep;
    try { rep = ExtractTo(*s, rootDir, {}); } catch (const Error& e) { threw = true; what = e.what(); }
    std::error_code ec;
    const bool there = fs::exists(rootDir / top / L"f.txt", ec);
    if (!threw && !rep.ok() && !there) {
        std::printf("   (cannot create folders in %s here: permissions) skipped\n", ToUtf8(rootDir.wstring()).c_str());
    } else {
        EXPECT_MSG(!threw, ("extracting into a drive root must not fail: " + what).c_str());
        EXPECT_MSG(there && ReadAll(rootDir / top / L"f.txt") == "in the root", "the file arrived in the drive root");
    }
    fs::remove_all(rootDir / top, ec);
}

// ---------------------------------------------------------------------------------------------------------
// ISO images and the update modes (2026-10-14)

static uint64_t ContentHash(const fs::path& p) {
    std::ifstream f(ToLongPath(p), std::ios::binary);
    const std::string s((std::istreambuf_iterator<char>(f)), {});
    return std::hash<std::string>{}(s) ^ (static_cast<uint64_t>(s.size()) << 1);
}

struct StringSink : ByteSink {
    std::string data;
    void Write(const void* p, size_t n) override { data.append(static_cast<const char*>(p), n); }
};

// path -> size of the files of an archive (folders left out)
static std::map<std::wstring, uint64_t> FileListing(const fs::path& archive, const OpenOptions& oo = {}) {
    std::map<std::wstring, uint64_t> m;
    auto s = ArchiveSource::Open(archive, oo);
    for (const Entry& e : s->Entries()) if (!e.isDir) m[e.path] = e.size;
    return m;
}

static std::string ReadFromArchive(const fs::path& archive, const std::wstring& path, const OpenOptions& oo = {}) {
    auto s = ArchiveSource::Open(archive, oo);
    for (const Entry& e : s->Entries())
        if (e.path == path) {
            StringSink sink;
            s->ReadEntry(e.index, sink);
            return sink.data;
        }
    return "<missing>";
}

static void SetFileTimeDays(const fs::path& p, int days) {
    HANDLE h = CreateFileW(ToLongPath(p).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    u.QuadPart += static_cast<unsigned long long>(days) * 86400ull * 10000000ull;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

static void TestIsoStress() {
    std::puts("-- ISO images: many files, deep folders, names that collide, cancel, disk full");
    fs::path src = Fresh(L"iso_src");
    const int kDirs = 400, kPerDir = 50;
    for (int d = 0; d < kDirs; ++d)
        for (int f = 0; f < kPerDir; ++f)
            WriteText(src / ("dir" + std::to_string(d)) / ("file" + std::to_string(f) + ".txt"), std::to_string(d) + " " + std::to_string(f));
    fs::path deep = src / "deep";
    for (int i = 0; i < 120; ++i) deep /= "level" + std::to_string(i);
    WriteText(deep / "bottom.txt", "bottom");
    for (int i = 0; i < 300; ++i) {   // all of them are SAME_PRE.TXT in the 8.3 part
        char name[64];
        std::snprintf(name, sizeof name, "same_prefix_%03d.txt", i);
        WriteText(src / "same" / name, "same " + std::to_string(i));
    }
    const std::string longPrefix(70, 'x');   // the same first 64 characters in the Joliet part
    for (int i = 0; i < 80; ++i) WriteText(src / "long" / (longPrefix + std::to_string(1000 + i) + ".txt"), "long " + std::to_string(i));
    for (int i = 0; i < 20; ++i) WriteText(src / L"中文" / (L"檔案" + std::to_wstring(i) + L".txt"), "uni " + std::to_string(i));

    fs::path iso = g_root / L"stress.iso";
    fs::remove(iso);
    CompressOptions co;
    co.type = ArchiveType::Iso;
    co.isoLabel = L"STRESS";
    const auto t0 = GetTickCount64();
    auto rep = CreateArchive(iso, {src}, co);
    const uint64_t expectFiles = static_cast<uint64_t>(kDirs) * kPerDir + 1 + 300 + 80 + 20;
    EXPECT_MSG(rep.ok() && rep.files == expectFiles, "ISO: the report counts every file");
    // the structure: the volume size of the descriptor is the size of the file
    {
        std::ifstream f(iso, std::ios::binary);
        std::string pvd(2048, '\0');
        f.seekg(16 * 2048);
        f.read(pvd.data(), 2048);
        uint32_t blocks = 0;
        std::memcpy(&blocks, pvd.data() + 80, 4);
        EXPECT_MSG(pvd.compare(1, 5, "CD001") == 0 && static_cast<uint64_t>(blocks) * 2048 == fs::file_size(iso), "ISO: the volume descriptor matches the file");
    }
    auto listing = FileListing(iso);
    EXPECT_MSG(listing.size() == expectFiles, ("ISO: read back " + std::to_string(listing.size()) + " files, expected " + std::to_string(expectFiles)).c_str());
    int wrong = 0, missing = 0;
    {
        auto s = ArchiveSource::Open(iso);
        std::map<std::wstring, uint32_t> index;
        for (const Entry& e : s->Entries()) if (!e.isDir) index[e.path] = e.index;
        std::set<std::string> longSeen;
        for (const auto& [path, idx] : index) {
            StringSink sink;
            s->ReadEntry(idx, sink);
            if (path.rfind(L"stress/long/", 0) == 0 || path.rfind(L"src/long/", 0) == 0 || path.find(L"/long/") != std::wstring::npos) { longSeen.insert(sink.data); continue; }
        }
        for (int d = 0; d < kDirs; d += 37)
            for (int f = 0; f < kPerDir; f += 7) {
                const std::wstring p = L"iso_src/dir" + std::to_wstring(d) + L"/file" + std::to_wstring(f) + L".txt";
                auto it = index.find(p);
                if (it == index.end()) { ++missing; continue; }
                StringSink sink;
                s->ReadEntry(it->second, sink);
                if (sink.data != std::to_string(d) + " " + std::to_string(f)) ++wrong;
            }
        EXPECT_MSG(missing == 0 && wrong == 0, ("ISO: sampled files missing " + std::to_string(missing) + ", wrong " + std::to_string(wrong)).c_str());
        EXPECT_MSG(longSeen.size() == 80, ("ISO: the 80 names that collide when cut to 64 characters are all there (" + std::to_string(longSeen.size()) + ")").c_str());
        bool bottom = false;
        for (const auto& [path, idx] : index) if (path.size() > 600 && path.find(L"bottom.txt") != std::wstring::npos) bottom = true;
        EXPECT_MSG(bottom, "ISO: the file at the bottom of 120 folders is there");
    }
    std::printf("   %llu files, %llu bytes, %llu s\n", static_cast<unsigned long long>(expectFiles), static_cast<unsigned long long>(fs::file_size(iso)),
                static_cast<unsigned long long>((GetTickCount64() - t0) / 1000));
    fs::remove(iso);
    EXPECT_MSG(TempLeftovers().empty(), ("ISO: temp left: " + Join(TempLeftovers())).c_str());

    // cancel and a full disk leave nothing
    fs::path src2 = Fresh(L"iso_src2");
    WriteRandom(src2 / "a.bin", 6 << 20, 11);
    WriteText(src2 / "b.txt", std::string(100000, 'b'));
    {
        fs::path out = Fresh(L"iso_out");
        CompressOptions c2;
        c2.type = ArchiveType::Iso;
        int calls = 0;
        c2.progress = [&](uint64_t d, uint64_t tot) { ++calls; return !(calls > 1 && d * 3 > tot); };
        bool cancelled = false;
        try { CreateArchive(out / L"x.iso", {src2}, c2); }
        catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT_MSG(cancelled, "ISO: cancelling is reported as a cancel");
        EXPECT_MSG(ListAll(out).empty(), ("ISO: cancel left: " + Join(ListAll(out))).c_str());
    }
    {
        fs::path out = Fresh(L"iso_out");
        CompressOptions c2;
        c2.type = ArchiveType::Iso;
        SetTestWriteFault(2 << 20);
        bool threw = false, diskFull = false;
        try { CreateArchive(out / L"x.iso", {src2}, c2); }
        catch (const Error& e) { threw = true; diskFull = e.diskFull(); }
        SetTestWriteFault(0);
        EXPECT_MSG(threw && diskFull, "ISO: a full disk is reported as a full disk");
        EXPECT_MSG(ListAll(out).empty(), ("ISO: full disk left: " + Join(ListAll(out))).c_str());
        EXPECT_MSG(TempLeftovers().empty(), ("ISO: full disk temp left: " + Join(TempLeftovers())).c_str());
    }
    // a file that is locked for reading (another program writes it): the others are in the image, the file is reported, the layout holds
    {
        fs::path src3 = Fresh(L"iso_src3");
        WriteText(src3 / "ok.txt", "fine");
        WriteText(src3 / "locked.txt", "locked data");
        HANDLE h = CreateFileW(ToLongPath(src3 / "locked.txt").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        fs::path out = g_root / L"locked.iso";
        fs::remove(out);
        CompressOptions c3;
        c3.type = ArchiveType::Iso;
        bool threw = false;
        CompressReport r3;
        try { r3 = CreateArchive(out, {src3}, c3); } catch (const Error&) { threw = true; }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        EXPECT_MSG(threw || !r3.errors.empty(), "ISO: a locked file is reported (error or failure), not silently left out");
        if (!threw) EXPECT_MSG(FileListing(out).count(L"iso_src3/ok.txt") == 1, "ISO: the other file is in the image");
        fs::remove(out);
    }
    std::error_code ec;
    fs::remove_all(ToLongPath(src), ec);
}

// An image bigger than 2 GiB (the sizes and the sector numbers must not wrap): 2.5 GiB of zeros and a small file after it
static void TestIsoHuge() {
    std::puts("-- ISO image of 2.5 GB");
    fs::path src = Fresh(L"isohuge_src");
    const uint64_t big = (5ull << 30) / 2;
    MakeZeroFile(src / L"big.bin", big);
    WriteText(src / L"tail.txt", "the file after the big one");
    fs::path iso = g_root / L"huge.iso";
    fs::remove(iso);
    CompressOptions co;
    co.type = ArchiveType::Iso;
    uint64_t last = 0, total = 0;
    bool backwards = false;
    co.progress = [&](uint64_t d, uint64_t t) { if (d < last) backwards = true; last = d; total = t; return true; };
    const auto t0 = GetTickCount64();
    auto rep = CreateArchive(iso, {src}, co);
    EXPECT_MSG(rep.ok() && fs::file_size(iso) > big && fs::file_size(iso) % 2048 == 0, "ISO 2.5 GB: made, and a whole number of sectors");
    EXPECT_MSG(!backwards && total >= big, "ISO 2.5 GB: the progress is not 32 bit");
    auto listing = FileListing(iso);
    uint64_t bigSize = 0;
    for (auto& [p, sz] : listing) if (p.find(L"big.bin") != std::wstring::npos) bigSize = sz;
    EXPECT_MSG(bigSize == big, ("ISO 2.5 GB: the size of the big file is listed (" + std::to_string(bigSize) + ")").c_str());
    EXPECT_MSG(ReadFromArchive(iso, L"isohuge_src/tail.txt") == "the file after the big one", "ISO 2.5 GB: the file after the big one is read back");
    std::printf("   2.5 GB image in %llu s\n", static_cast<unsigned long long>((GetTickCount64() - t0) / 1000));
    std::error_code ec;
    fs::remove(iso, ec);
    fs::remove_all(ToLongPath(src), ec);
}

// The update modes on a lot of entries: the counts of a preview and of the real run agree, the archive is not touched by a preview or a cancelled run,
// the result is the folder
static void UpdateScenario(const wchar_t* label, ArchiveType type, const wchar_t* ext, int count, const std::wstring& password, bool solid) {
    const std::string name = ToUtf8(label);
    std::printf("   %s: %d files\n", name.c_str(), count);
    fs::path work = Fresh(L"um_work");
    fs::path data = work / "data";
    const int dirs = (std::max)(1, count / 50);
    auto fileOf = [&](int i) { return data / ("d" + std::to_string(i % dirs)) / ("f" + std::to_string(i) + ".txt"); };
    for (int i = 0; i < count; ++i) WriteText(fileOf(i), "v1-" + std::to_string(i));
    fs::path arc = work / (std::wstring(L"t") + ext);
    CompressOptions co;
    co.type = type;
    co.password = password;
    if (type == ArchiveType::SevenZip) co.solidSize = solid ? kSolidAll : kSolidOff;
    CreateArchive(arc, {data}, co);
    OpenOptions oo;
    if (!password.empty()) oo.password = [password](std::wstring& o) { o = password; return true; };
    EXPECT_MSG(FileListing(arc, oo).size() == static_cast<size_t>(count), (name + ": the archive has every file").c_str());

    // the folder changes: a third is newer (and other content), a third is gone, 300 are new
    int changed = 0, removed = 0;
    for (int i = 0; i < count; ++i) {
        if (i % 3 == 0) { WriteText(fileOf(i), "v2-" + std::to_string(i)); SetFileTimeDays(fileOf(i), 1); ++changed; }
        else if (i % 3 == 1) { fs::remove(ToLongPath(fileOf(i))); ++removed; }
    }
    const int added = 300;
    for (int i = 0; i < added; ++i) WriteText(data / "new" / ("n" + std::to_string(i) + ".txt"), "new-" + std::to_string(i));
    const int unchanged = count - changed - removed;

    UpdateOptions uo;
    uo.compress.type = type;
    uo.compress.password = password;
    uo.compress.storeCompressed = false;
    uo.mode = UpdateMode::Synchronize;
    if (type == ArchiveType::SevenZip) uo.compress.solidSize = solid ? kSolidAll : kSolidOff;
    const std::vector<UpdateOp> ops = {UpdateOp::Add(L"", {data})};

    // a preview does not touch the archive and says what will happen
    const uint64_t before = ContentHash(arc);
    auto pv = PreviewUpdate(arc, ops, uo);
    EXPECT_MSG(ContentHash(arc) == before, (name + ": a preview leaves the archive as it was").c_str());
    EXPECT_MSG(pv.deleted == static_cast<uint32_t>(removed) && pv.replaced == static_cast<uint32_t>(changed) && pv.added == static_cast<uint32_t>(added) && pv.unchanged == static_cast<uint32_t>(unchanged),
               (name + ": preview counts deleted " + std::to_string(pv.deleted) + "/" + std::to_string(removed) + " replaced " + std::to_string(pv.replaced) + "/" + std::to_string(changed) +
                " added " + std::to_string(pv.added) + "/" + std::to_string(added) + " unchanged " + std::to_string(pv.unchanged) + "/" + std::to_string(unchanged)).c_str());

    // a cancelled run leaves the archive byte for byte and nothing next to it
    {
        const auto beforeFiles = ListAll(work);
        UpdateOptions cu = uo;
        int calls = 0;
        cu.progress = [&](uint64_t, uint64_t) { return ++calls < 3; };
        bool cancelled = false;
        try { UpdateArchive(arc, ops, cu); } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT_MSG(cancelled, (name + ": cancelling the update is a cancel").c_str());
        EXPECT_MSG(ContentHash(arc) == before, (name + ": a cancelled update leaves the archive as it was").c_str());
        EXPECT_MSG(ListAll(work) == beforeFiles, (name + ": a cancelled update leaves nothing next to the archive").c_str());
    }

    // the real run: the counts of the preview, and the archive is the folder
    auto rep = UpdateArchive(arc, ops, uo);
    EXPECT_MSG(rep.ok() && rep.changed && rep.deleted == pv.deleted && rep.replaced == pv.replaced && rep.added == pv.added, (name + ": the run does what the preview said").c_str());
    auto after = FileListing(arc, oo);
    EXPECT_MSG(after.size() == static_cast<size_t>(count - removed + added), (name + ": the archive has " + std::to_string(after.size()) + " files, expected " + std::to_string(count - removed + added)).c_str());
    int wrongNames = 0;
    for (const auto& [path, sz] : after) {
        (void)sz;
        const bool isNew = path.find(L"/new/") != std::wstring::npos;
        if (!isNew) {
            const size_t f = path.find(L"/f");
            if (f == std::wstring::npos) { ++wrongNames; continue; }
            const int i = std::stoi(path.substr(f + 2));
            if (i % 3 == 1) ++wrongNames;   // a deleted one is still there
        }
    }
    EXPECT_MSG(wrongNames == 0, (name + ": files that are gone from the folder are gone from the archive").c_str());
    // contents: a replaced file has the new content, an untouched one the old
    EXPECT_MSG(ReadFromArchive(arc, L"data/d0/f0.txt", oo) == "v2-0", (name + ": a replaced file has its new content").c_str());
    EXPECT_MSG(ReadFromArchive(arc, L"data/d" + std::to_wstring(2 % dirs) + L"/f2.txt", oo) == "v1-2", (name + ": an untouched file keeps its content").c_str());
    // and again: nothing to do, the archive is not rewritten
    const uint64_t done = ContentHash(arc);
    UpdateOptions again = uo;
    again.mode = UpdateMode::UpdateAdd;
    auto rep2 = UpdateArchive(arc, ops, again);
    EXPECT_MSG(rep2.ok() && !rep2.changed && ContentHash(arc) == done, (name + ": the same folder again changes nothing").c_str());
    EXPECT_MSG(ListAll(work).size() > 0, "work folder is there");
    std::error_code ec;
    fs::remove_all(ToLongPath(work), ec);
}

static void TestUpdateModesStress() {
    std::puts("-- update modes on many entries (zip, solid 7z, encrypted zip)");
    const bool heavy = std::getenv("ROBUST_HEAVY") != nullptr;
    UpdateScenario(L"zip", ArchiveType::Zip, L".zip", heavy ? 30000 : 3000, L"", false);
    UpdateScenario(L"7z solid", ArchiveType::SevenZip, L".7z", heavy ? 8000 : 1500, L"", true);
    UpdateScenario(L"7z not solid", ArchiveType::SevenZip, L".7z", heavy ? 8000 : 1500, L"", false);
    UpdateScenario(L"zip AES-256", ArchiveType::Zip, L".zip", heavy ? 3000 : 600, L"secret123", false);
    UpdateScenario(L"7z encrypted", ArchiveType::SevenZip, L".7z", heavy ? 3000 : 600, L"secret123", true);
}

static int Run() {
    // ROBUST_ROOT: put the scratch folder elsewhere (e.g. a bigger drive for the heavy tests)
    wchar_t rootEnv[MAX_PATH] = {};
    const bool haveRoot = GetEnvironmentVariableW(L"ROBUST_ROOT", rootEnv, MAX_PATH) > 0;
    g_root = (haveRoot ? fs::path(rootEnv) : fs::temp_directory_path()) / (L"lycan_robust_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(g_root);
    g_tmp = g_root / L"tmp";  // private TEMP: leftovers of staging files show up here
    fs::create_directories(g_tmp);
    SetEnvironmentVariableW(L"TMP", g_tmp.c_str());
    SetEnvironmentVariableW(L"TEMP", g_tmp.c_str());

    const char* only = std::getenv("ROBUST_ONLY");
    auto want = [&](const char* n) { return !only || std::string(only).find(n) != std::string::npos; };
    if (want("cancel")) { TestCompressCancel(); TestExtractCancel(); }
    if (want("diskfull")) { TestDiskFull(); TestDiskFullBzip2(); TestFreeSpacePrecheck(); }
    if (want("bomb")) { TestBombLimits(); TestDeclaredSizeLie(); }
    if (want("longpath")) TestLongPaths();
    if (want("locked")) TestLockedFile();
    if (want("symlink")) TestSymlinks();
    if (want("concurrency")) TestConcurrency();
    if (want("volumes")) TestManyVolumes();
    if (want("integrity")) TestIntegrityPass();
    if (want("smartscan")) TestSmartScanFlat();
    if (want("driveroot")) TestDriveRootDestination();
    if (want("leaks")) TestResourceLeaks();
    if (want("misuse")) TestApiMisuse();
    if (want("tree")) TestPathologicalTree();
    if (want("relative")) TestRelativeInputs();
    if (want("progress")) TestTarProgress();
    if (want("encoding")) TestEncodingCorpus();
    if (want("password")) TestPasswordCompat();
    if (want("openscratch")) TestOpenScratch();
    if (want("iso")) TestIsoStress();
    if (want("updatemode")) TestUpdateModesStress();
    if (std::getenv("ROBUST_HEAVY") && want("huge")) { TestHugeFiles(); TestIsoHuge(); }

    std::error_code ec;
    fs::remove_all(ToLongPath(g_root), ec);
    std::printf(failures ? "FAILURES: %d\n" : "all ok\n", failures);
    return failures ? 1 : 0;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    try { return Run(); }
    catch (const lycan::Error& e) {
        std::fprintf(stderr, "UNCAUGHT lycan::Error: %s (hr=0x%08lX)\n", e.what(), static_cast<unsigned long>(e.hr()));
        return 4;
    }
    catch (const std::exception& e) {
        std::fprintf(stderr, "UNCAUGHT: %s\n", e.what());
        return 5;
    }
}
