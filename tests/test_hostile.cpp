#include "no_dialogs.h"
// Hostile archives: the safety layer of the extraction against what other archivers had to fix (links that point out of
// the destination, names Windows treats specially, the Mark of the Web, parallel extraction into one folder).
// The archives are built by hand (zip / tar writers below), so that they hold exactly what an attacker would write.
// LYCAN_HOSTILE_OUT=<folder>: also writes the archives there (seeds of the fuzz corpus, see scripts/fuzz.ps1).
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/encoding.h"
#include "lycan/error.h"
#include "lycan/exectype.h"
#include "lycan/extract.h"
#include "lycan/security.h"
#include "lycan/update.h"

using namespace lycan;
namespace fs = std::filesystem;

static int failures = 0;
#define EXPECT_MSG(cond, msg)                                                         \
    do {                                                                              \
        if (!(cond)) {                                                                \
            std::fprintf(stderr, "FAIL line %d: %s (%s)\n", __LINE__, #cond, (msg)); \
            ++failures;                                                               \
        }                                                                             \
    } while (0)
#define EXPECT(cond) EXPECT_MSG(cond, "")

static fs::path g_root;

static void WriteBytes(const fs::path& p, const std::string& s) {
    fs::create_directories(ToLongPath(p.parent_path()));
    std::ofstream(ToLongPath(p), std::ios::binary) << s;
}
static std::string ReadAll(const fs::path& p) {
    std::ifstream f(ToLongPath(p), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static fs::path Fresh(const wchar_t* name) {
    fs::path p = g_root / name;
    std::error_code ec;
    fs::remove_all(ToLongPath(p), ec);
    fs::create_directories(ToLongPath(p));
    return p;
}
// Everything below dir (full long paths)
static std::vector<fs::path> Walk(const fs::path& dir) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(ToLongPath(dir), fs::directory_options::none, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        out.push_back(it->path());
        // do not walk into a link (the test would follow it out of the folder)
        if (GetFileAttributesW(it->path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) it.disable_recursion_pending();
    }
    return out;
}
static std::string Names(const fs::path& dir) {
    std::string r;
    for (auto& p : Walk(dir)) r += ToUtf8(fs::relative(p, ToLongPath(dir)).wstring()) + " | ";
    return r;
}
static std::string First(const std::vector<Message>& v) { return v.empty() ? std::string("-") : ToUtf8(v[0]); }

static void SaveSample(const std::string& name, const std::string& bytes) {
    static const char* out = std::getenv("LYCAN_HOSTILE_OUT");
    if (out && *out) WriteBytes(fs::path(WideFromUtf8(out)) / WideFromUtf8(name), bytes);
}

// ---- archive writers ----

static uint32_t Crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void Le16(std::string& s, uint32_t v) { s += static_cast<char>(v & 0xFF); s += static_cast<char>((v >> 8) & 0xFF); }
static void Le32(std::string& s, uint32_t v) { Le16(s, v & 0xFFFF); Le16(s, v >> 16); }

struct ZipItem {
    std::string name;      // UTF-8
    std::string data;
    uint32_t external = 0;  // external attributes
    bool unix = false;      // "made by" Unix: the high 16 bits of `external` are the mode
};
// A zip with stored items, UTF-8 names (flag 11)
static std::string MakeZip(const std::vector<ZipItem>& items) {
    std::string out, cd;
    for (auto& it : items) {
        const uint32_t off = static_cast<uint32_t>(out.size()), crc = Crc32(it.data), n = static_cast<uint32_t>(it.data.size());
        Le32(out, 0x04034b50); Le16(out, 20); Le16(out, 0x0800); Le16(out, 0); Le16(out, 0); Le16(out, 0x5821);
        Le32(out, crc); Le32(out, n); Le32(out, n); Le16(out, static_cast<uint32_t>(it.name.size())); Le16(out, 0);
        out += it.name + it.data;
        Le32(cd, 0x02014b50); Le16(cd, (it.unix ? 3u << 8 : 0u) | 20); Le16(cd, 20); Le16(cd, 0x0800); Le16(cd, 0); Le16(cd, 0); Le16(cd, 0x5821);
        Le32(cd, crc); Le32(cd, n); Le32(cd, n); Le16(cd, static_cast<uint32_t>(it.name.size())); Le16(cd, 0); Le16(cd, 0); Le16(cd, 0); Le16(cd, 0);
        Le32(cd, it.external); Le32(cd, off);
        cd += it.name;
    }
    const uint32_t cdOff = static_cast<uint32_t>(out.size());
    out += cd;
    Le32(out, 0x06054b50); Le16(out, 0); Le16(out, 0); Le16(out, static_cast<uint32_t>(items.size())); Le16(out, static_cast<uint32_t>(items.size()));
    Le32(out, static_cast<uint32_t>(cd.size())); Le32(out, cdOff); Le16(out, 0);
    return out;
}

struct TarItem {
    std::string name;
    char type = '0';  // '0' file, '1' hard link, '2' symbolic link, '5' folder
    std::string link;
    std::string data;
};
static std::string MakeTar(const std::vector<TarItem>& items) {
    std::string out;
    for (auto& it : items) {
        std::string h(512, '\0');
        auto put = [&](size_t off, const std::string& s, size_t max) { std::memcpy(&h[off], s.data(), std::min(s.size(), max)); };
        char buf[16];
        put(0, it.name, 100);
        std::snprintf(buf, sizeof buf, "%07o", 0644); put(100, buf, 8);
        std::snprintf(buf, sizeof buf, "%07o", 0); put(108, buf, 8); put(116, buf, 8);
        std::snprintf(buf, sizeof buf, "%011o", static_cast<unsigned>(it.data.size())); put(124, buf, 12);
        std::snprintf(buf, sizeof buf, "%011o", 0); put(136, buf, 12);
        h[156] = it.type;
        put(157, it.link, 100);
        put(257, "ustar", 6); h[263] = '0'; h[264] = '0';
        std::memset(&h[148], ' ', 8);
        unsigned sum = 0;
        for (unsigned char c : h) sum += c;
        std::snprintf(buf, sizeof buf, "%06o", sum);
        std::memcpy(&h[148], buf, 6); h[154] = '\0'; h[155] = ' ';
        out += h + it.data + std::string((512 - it.data.size() % 512) % 512, '\0');
    }
    return out + std::string(1024, '\0');
}

static ExtractReport ExtractFile(const fs::path& arc, const fs::path& dest, ExtractOptions opt = {}) {
    auto s = ArchiveSource::Open(arc);
    return ExtractTo(*s, dest, opt);
}

// ---- checks shared by the cases ----

// Nothing in dest is a link, a junction or a mount point
static void ExpectNoReparse(const fs::path& dest, const std::string& label) {
    for (auto& p : Walk(dest))
        EXPECT_MSG(!(GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT), (label + ": a link was written: " + ToUtf8(p.wstring())).c_str());
}
// No file in dest has a second hard link
static void ExpectSingleLinks(const fs::path& dest, const std::string& label) {
    for (auto& p : Walk(dest)) {
        HANDLE h = CreateFileW(p.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        BY_HANDLE_FILE_INFORMATION fi{};
        GetFileInformationByHandle(h, &fi);
        CloseHandle(h);
        if (!(fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            EXPECT_MSG(fi.nNumberOfLinks == 1, (label + ": a file has a hard link to somewhere else: " + ToUtf8(p.wstring())).c_str());
    }
}
// The streams of a file other than the content (":Zone.Identifier", ...)
static std::vector<std::wstring> NamedStreams(const fs::path& p) {
    std::vector<std::wstring> out;
    WIN32_FIND_STREAM_DATA d;
    HANDLE h = FindFirstStreamW(p.c_str(), FindStreamInfoStandard, &d, 0);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring n = d.cStreamName;
        if (n != L"::$DATA") out.push_back(n);
    } while (FindNextStreamW(h, &d));
    FindClose(h);
    return out;
}
static std::string ReadZone(const fs::path& p) { return ReadAll(fs::path(ToLongPath(p).wstring() + L":Zone.Identifier")); }
static void WriteZone(const fs::path& p, const std::string& s) {
    std::ofstream(fs::path(ToLongPath(p).wstring() + L":Zone.Identifier"), std::ios::binary) << s;
}

// What the system itself would take for a device (CON, COM1 ...) when a program opens the name without the \\?\ prefix
static bool OsSeesDevice(const fs::path& p) {
    using Fn = ULONG(NTAPI*)(PCWSTR);
    static Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlIsDosDeviceName_U"));
    std::wstring s = p.wstring();
    if (s.rfind(LR"(\\?\)", 0) == 0) s = s.substr(4);
    return fn && fn(s.c_str()) != 0;
}
// The names Microsoft documents as reserved, whatever this Windows version does with them
static bool DocumentedReserved(std::wstring c) {
    const auto dot = c.find(L'.');
    std::wstring stem = dot == std::wstring::npos ? c : c.substr(0, dot);
    while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.pop_back();
    for (auto& ch : stem) ch = static_cast<wchar_t>(towupper(ch));
    for (const wchar_t* r : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (stem == r) return true;
    if (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0)) {
        const wchar_t d = stem[3];
        if ((d >= L'1' && d <= L'9') || d == 0xB9 || d == 0xB2 || d == 0xB3) return true;
    }
    return false;
}

// ---- cases ----

// Links: skipped in every arrangement, nothing is written outside the destination
static void TestLinks() {
    std::puts("-- links");
    fs::path base = Fresh(L"links");
    fs::path outside = base / L"outside";
    WriteBytes(outside / L"secret.txt", "secret");
    const std::string outAbs = ToUtf8(outside.wstring());
    struct Case { const char* label; std::string bytes; const wchar_t* ext; };
    std::vector<Case> cases = {
        {"tar hard link to an absolute path", MakeTar({{"h.txt", '1', outAbs + "\\secret.txt", ""}, {"after.txt", '0', "", "ok"}}), L".tar"},
        {"tar hard link up the tree", MakeTar({{"h.txt", '1', "../outside/secret.txt", ""}, {"h.txt", '0', "", "overwritten?"}}), L".tar"},
        {"tar symlink to a symlink to outside", MakeTar({{"l1", '2', "../outside", ""}, {"l2", '2', "l1", ""}, {"l2/pwn.txt", '0', "", "pwned"}}), L".tar"},
        {"tar symlink, then a file of the same name", MakeTar({{"same", '2', outAbs, ""}, {"same", '0', "", "plain file"}, {"same2", '0', "", "x"}}), L".tar"},
        {"tar file, then a symlink of the same name", MakeTar({{"same", '0', "", "plain file"}, {"same", '2', outAbs, ""}}), L".tar"},
        {"tar symlink folder, then a file into it", MakeTar({{"d", '2', outAbs, ""}, {"d/pwn.txt", '0', "", "pwned"}}), L".tar"},
        {"zip Unix symlink, then a file through it", MakeZip({{"zl", "../outside", 0120777u << 16, true}, {"zl/pwn.txt", "pwned", 0100644u << 16, true}}), L".zip"},
        {"zip Unix symlink to an absolute path", MakeZip({{"za.txt", outAbs + "\\secret.txt", 0120777u << 16, true}}), L".zip"},
        {"zip with the Windows reparse attribute", MakeZip({{"rp", outAbs, FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY}, {"rp/pwn.txt", "pwned", 0}}), L".zip"},
    };
    int n = 0;
    for (auto& c : cases) {
        ++n;
        fs::path arc = base / (L"l" + std::to_wstring(n) + c.ext);
        WriteBytes(arc, c.bytes);
        SaveSample("link" + std::to_string(n) + ToUtf8(c.ext), c.bytes);
        fs::path dest = Fresh((L"links_d" + std::to_wstring(n)).c_str());
        ExtractReport rep;
        try {
            rep = ExtractFile(arc, dest);
        } catch (const Error& e) {
            EXPECT_MSG(false, (std::string(c.label) + ": threw " + std::string(e.what())).c_str());
            continue;
        }
        ExpectNoReparse(dest, c.label);
        ExpectSingleLinks(dest, c.label);
        std::error_code ec;
        size_t outsideCount = 0;
        for (auto it = fs::recursive_directory_iterator(outside, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) ++outsideCount;
        EXPECT_MSG(outsideCount == 1 && ReadAll(outside / L"secret.txt") == "secret", (std::string(c.label) + ": the folder outside changed").c_str());
        EXPECT_MSG(!rep.skipped.empty() || !rep.rejected.empty(), (std::string(c.label) + ": the link was not reported").c_str());
        std::printf("   %-45s files %llu, skipped %zu, rejected %zu, errors %zu | %s\n", c.label, static_cast<unsigned long long>(rep.files), rep.skipped.size(), rep.rejected.size(), rep.errors.size(), Names(dest).c_str());
    }
    // the plain file that follows a link of the same name is still extracted as a file
    EXPECT_MSG(ReadAll(g_root / L"links_d4" / L"same") == "plain file", "the file after the link of the same name");
    EXPECT_MSG(ReadAll(g_root / L"links_d5" / L"same") == "plain file", "the file before the link of the same name");
}

// Names that Windows treats specially: everything stays inside, nothing becomes a device or a stream
static void TestNames() {
    std::puts("-- names");
    fs::path base = Fresh(L"names");
    const std::vector<std::string> names = {
        "CON", "con.txt", "CON .txt", "Con..txt", "nul.tar.gz", "COM1.log", "LPT9", "COM\xC2\xB9.txt", "LPT\xC2\xB3", "CONIN$", "conout$.txt",
        "AUX/inside.txt", "dir/PRN", "COM0.txt", "COM10.txt",
        "a.txt:evil", "b.txt::$DATA", "c.txt:Zone.Identifier", "dots...", "space ", "dir. /x.txt", " lead.txt",
        "..\xE2\x88\x95up.txt", "\xEF\xBC\x8E\xEF\xBC\x8E/fullwidth.txt", "x\xE2\x80\xAEtxt.bin", "e\xCC\x81.txt",   // (a right-to-left mark before an .exe is the classic spoof: antivirus products flag such a file the moment it is written, so the extraction test uses .bin; IsExecutableName is checked with the .exe name, without a file) "\xC3\xA9.txt",
        "Readme.txt", "README.TXT", "PROGRA~1/x.txt", "\\\\?\\C:\\pwn.txt", "\\??\\C:\\pwn.txt", "C:pwn.txt", "/abs.txt", "a/../../up.txt",
        std::string(300, 'L') + ".txt", "ok.txt",
    };
    std::vector<ZipItem> items;
    for (auto& n : names) items.push_back({n, "x:" + n, 0});
    const std::string zip = MakeZip(items);
    fs::path arc = base / L"names.zip";
    WriteBytes(arc, zip);
    SaveSample("names.zip", zip);
    fs::path dest = base / L"dest";
    fs::create_directories(dest);
    WriteBytes(base / L"sentinel.txt", "s");
    ExtractReport rep;
    try {
        rep = ExtractFile(arc, dest);
    } catch (const Error& e) {
        EXPECT_MSG(false, ("names: threw " + std::string(e.what())).c_str());
        return;
    }
    // nothing outside dest: only dest, the archive and the sentinel are in base
    std::set<std::wstring> top;
    for (auto& e : fs::directory_iterator(base)) top.insert(e.path().filename().wstring());
    EXPECT_MSG(top == std::set<std::wstring>({L"dest", L"names.zip", L"sentinel.txt"}), "names: something was written next to the destination");
    EXPECT_MSG(!fs::exists(L"C:\\pwn.txt"), "names: a file reached C:\\");
    for (auto& p : Walk(dest)) {
        const std::string u = ToUtf8(p.wstring());
        EXPECT_MSG(!OsSeesDevice(p), ("names: Windows takes this for a device: " + u).c_str());
        EXPECT_MSG(!DocumentedReserved(p.filename().wstring()), ("names: a documented reserved name was written: " + u).c_str());
        for (auto& s : NamedStreams(p)) EXPECT_MSG(false, ("names: a stream was written: " + u + ToUtf8(s)).c_str());
    }
    // every item is accounted for: written, renamed, refused or an error
    const uint64_t seen = rep.files + rep.rejected.size() + rep.errors.size() + rep.skipped.size();
    EXPECT_MSG(seen == names.size(), ("names: " + std::to_string(seen) + " of " + std::to_string(names.size()) + " accounted for").c_str());
    EXPECT_MSG(ReadAll(dest / L"ok.txt") == "x:ok.txt", "names: the ordinary file");
    std::printf("   files %llu, renamed %zu, rejected %zu, errors %zu (first error: %s)\n", static_cast<unsigned long long>(rep.files), rep.renamed.size(), rep.rejected.size(), rep.errors.size(), First(rep.errors).c_str());
    for (auto& r : rep.renamed) std::printf("     renamed: %s\n", ToUtf8(r).c_str());

    // an 8.3 short name of a file that is already in the destination is not a way to replace it
    fs::path d2 = Fresh(L"names_short");
    WriteBytes(d2 / L"LongFileNameHere.txt", "original");
    wchar_t shortName[MAX_PATH] = {};
    GetShortPathNameW((d2 / L"LongFileNameHere.txt").c_str(), shortName, MAX_PATH);
    const std::wstring sn = fs::path(shortName).filename().wstring();
    if (sn.find(L'~') == std::wstring::npos) {
        std::puts("   (no 8.3 names on this volume) short-name case skipped");
    } else {
        const std::string z2 = MakeZip({{ToUtf8(sn), "replaced", 0}});
        WriteBytes(base / L"short.zip", z2);
        auto r2 = ExtractFile(base / L"short.zip", d2);  // the default: an existing file is an error, not replaced
        EXPECT_MSG(ReadAll(d2 / L"LongFileNameHere.txt") == "original", "names: the short name replaced the long file");
        std::printf("   short name %s: files %llu, errors %zu\n", ToUtf8(sn).c_str(), static_cast<unsigned long long>(r2.files), r2.errors.size());
    }
}

// The Mark of the Web of the archive goes to every file that comes out, in every way a file comes out
static void TestMotw() {
    std::puts("-- Mark of the Web");
    const std::string zone = "[ZoneTransfer]\r\nZoneId=3\r\nReferrerUrl=https://example.invalid/\r\n";
    fs::path base = Fresh(L"motw");
    fs::path src = base / L"src";
    WriteBytes(src / L"a.txt", "a");
    WriteBytes(src / L"dir" / L"b.exe", "MZ");
    const struct { const wchar_t* ext; ArchiveType type; } kinds[] = {
        {L".zip", ArchiveType::Zip}, {L".7z", ArchiveType::SevenZip}, {L".tar", ArchiveType::Tar}, {L".tar.gz", ArchiveType::TarGz}, {L".tar.xz", ArchiveType::TarXz}};
    for (auto& k : kinds) {
        const std::string label = ToUtf8(k.ext);
        fs::path arc = base / (std::wstring(L"m") + k.ext);
        CompressOptions co;
        co.type = k.type;
        co.overwrite = true;
        CreateArchive(arc, {src / L"a.txt", src / L"dir"}, co);
        WriteZone(arc, zone);
        // all items
        fs::path d = Fresh((L"motw_all" + std::wstring(k.ext)).c_str());
        auto rep = ExtractFile(arc, d);
        int n = 0;
        for (auto& p : Walk(d)) {
            if (fs::is_directory(p)) continue;
            ++n;
            EXPECT_MSG(ReadZone(p) == zone, (label + ": no Zone.Identifier on " + ToUtf8(p.wstring())).c_str());
        }
        EXPECT_MSG(n == 2 && rep.ok(), (label + ": extracted " + std::to_string(n)).c_str());
        // one item (what "open" and the drag to a folder do), over an existing file (the replace path)
        {
            auto s = ArchiveSource::Open(arc);
            uint32_t idx = 0;
            for (uint32_t i = 0; i < s->Entries().size(); ++i)
                if (s->Entries()[i].path.find(L"b.exe") != std::wstring::npos) idx = i;
            fs::path d1 = Fresh((L"motw_one" + std::wstring(k.ext)).c_str());
            ExtractOptions o;
            o.stripPrefix = L"dir/";
            ExtractTo(*s, d1, o, {idx});
            EXPECT_MSG(ReadZone(d1 / L"b.exe") == zone, (label + ": one item: no Zone.Identifier").c_str());
            WriteBytes(d1 / L"b.exe", "old");
            o.overwrite = OverwriteMode::Overwrite;
            ExtractTo(*s, d1, o, {idx});
            EXPECT_MSG(ReadAll(d1 / L"b.exe") == "MZ" && ReadZone(d1 / L"b.exe") == zone, (label + ": replaced file: no Zone.Identifier").c_str());
            o.overwrite = OverwriteMode::Rename;
            ExtractTo(*s, d1, o, {idx});
            EXPECT_MSG(ReadZone(d1 / L"b (2).exe") == zone, (label + ": renamed file: no Zone.Identifier").c_str());
            // turned off: no mark
            fs::path d0 = Fresh((L"motw_off" + std::wstring(k.ext)).c_str());
            o.overwrite = OverwriteMode::Fail;
            o.propagateMotw = false;
            ExtractTo(*s, d0, o, {idx});
            EXPECT_MSG(NamedStreams(d0 / L"b.exe").empty(), (label + ": propagateMotw=false still marked the file").c_str());
        }
        // an archive that is changed in place keeps its own mark (or what comes out of it later would lose it)
        if (k.type == ArchiveType::Zip || k.type == ArchiveType::SevenZip || k.type == ArchiveType::Tar) {
            WriteBytes(base / L"added.txt", "new");
            try {
                UpdateArchive(arc, {UpdateOp::Add(L"", {base / L"added.txt"})});
                EXPECT_MSG(ReadZone(arc) == zone, (label + ": the archive lost its Zone.Identifier when it was changed").c_str());
                UpdateArchive(arc, {UpdateOp::Delete(L"a.txt")});
                EXPECT_MSG(ReadZone(arc) == zone, (label + ": the archive lost its Zone.Identifier after a delete").c_str());
            } catch (const Error& e) {
                EXPECT_MSG(false, (label + ": update threw " + std::string(e.what())).c_str());
            }
        }
        std::printf("   %-8s ok\n", label.c_str());
    }
    // volumes: the mark of the first volume
    {
        fs::path big = base / L"vsrc" / L"big.bin";
        std::string data(300000, '\0');
        for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>((i * 2654435761u) >> 13);
        WriteBytes(big, data);
        CompressOptions co;
        co.type = ArchiveType::SevenZip;
        co.level = 0;
        co.volumeSize = 100000;
        co.overwrite = true;
        CreateArchive(base / L"v.7z", {big}, co);
        WriteZone(base / L"v.7z.001", zone);
        fs::path d = Fresh(L"motw_vol");
        ExtractFile(base / L"v.7z.001", d);
        EXPECT_MSG(ReadZone(d / L"big.bin") == zone, "volumes: no Zone.Identifier from the first volume");
        std::puts("   volumes  ok");
    }
}

// The list of the types that run code when opened (asked before opening, the files around it come out too)
static void TestExecutableTypes() {
    std::puts("-- executable types");
    for (const wchar_t* n : {L"a.exe", L"A.EXE", L"setup.msi", L"x.scr", L"x.lnk", L"x.cpl", L"x.msc", L"x.jar", L"x.hta", L"x.appref-ms",
                             L"x.settingcontent-ms", L"x.library-ms", L"x.searchconnector-ms", L"x.application", L"x.scf", L"x.chm", L"x.vb",
                             L"x.ws", L"x.wsc", L"x.sct", L"x.xll", L"x.xbap", L"x.msu", L"x.diagcab", L"x.ps1xml", L"x.psc1", L"x.jnlp",
                             L"x.url", L"x.reg", L"x.inf", L"photo.jpg.exe", L"x\x202Egpj.exe"})
        EXPECT_MSG(IsExecutableName(n), ToUtf8(n).c_str());
    for (const wchar_t* n : {L"a.txt", L"a.exe.jpg", L"exe", L"a.", L"a.zip", L"a.pdf", L"a.docx", L"a.png"})
        EXPECT_MSG(!IsExecutableName(n), ("not executable: " + ToUtf8(n)).c_str());
}

// Several extractions into one folder at the same time, the same names in each: every file is whole and from one of them
static void TestParallelSameDestination() {
    std::puts("-- parallel extraction into one folder");
    fs::path base = Fresh(L"par");
    std::vector<ZipItem> a, b;
    for (int i = 0; i < 20; ++i) {
        a.push_back({"shared/f" + std::to_string(i) + ".bin", std::string(150000 + i, 'a'), 0});
        b.push_back({"shared/f" + std::to_string(i) + ".bin", std::string(150000 + i, 'b'), 0});
    }
    WriteBytes(base / L"a.zip", MakeZip(a));
    WriteBytes(base / L"b.zip", MakeZip(b));
    fs::path dest = base / L"dest";
    std::atomic<int> errors{0}, threw{0};
    std::vector<std::thread> th;
    for (int t = 0; t < 8; ++t)
        th.emplace_back([&, t] {
            for (int round = 0; round < 3; ++round) {
                try {
                    ExtractOptions o;
                    o.overwrite = OverwriteMode::Overwrite;
                    auto rep = ExtractFile(base / ((t + round) % 2 ? L"a.zip" : L"b.zip"), dest, o);
                    errors += static_cast<int>(rep.errors.size());
                    if (!rep.errors.empty()) std::printf("   thread %d: %s\n", t, First(rep.errors).c_str());
                } catch (const Error& e) {
                    ++threw;
                    std::printf("   thread %d threw %s\n", t, std::string(e.what()).c_str());
                }
            }
        });
    for (auto& x : th) x.join();
    EXPECT_MSG(threw == 0, "parallel: an extraction threw");
    int leftovers = 0, torn = 0, files = 0;
    for (auto& p : Walk(dest)) {
        if (fs::is_directory(p)) continue;
        const std::wstring fn = p.filename().wstring();
        if (fn.rfind(L".lycanzip-", 0) == 0) { ++leftovers; continue; }
        ++files;
        const std::string s = ReadAll(p);
        const int i = std::stoi(fn.substr(1));
        if (s.size() != static_cast<size_t>(150000 + i) || (s != std::string(s.size(), 'a') && s != std::string(s.size(), 'b'))) ++torn;
    }
    EXPECT_MSG(files == 20 && torn == 0 && leftovers == 0, ("parallel: files " + std::to_string(files) + ", torn " + std::to_string(torn) + ", temporary leftovers " + std::to_string(leftovers)).c_str());
    std::printf("   24 extractions: errors %d, files %d, torn %d, leftovers %d\n", errors.load(), files, torn, leftovers);
    EXPECT_MSG(errors == 0, "parallel: errors while replacing (see above)");
}

// An archive whose headers say far less than what comes out: the question about a suspiciously big extraction is asked
// while it is written (once), with what came out so far; "no" stops it and leaves nothing half-written
static void TestSizesLie() {
    std::puts("-- headers that lie about the sizes");
    // (a zip is no way to lie: 7-Zip stops at the size of the header.) A .gz keeps the size in a 32-bit field at the end
    // that nothing checks before the end: 8 MB of zeros, said to be 1000 bytes. The deflate stream is written by hand:
    // one block of fixed codes, a literal 0, then matches of 258 bytes at distance 1.
    const uint32_t kZeros = 8u << 20;
    std::string deflate;
    uint32_t acc = 0;
    int nbits = 0;
    auto bits = [&](uint32_t v, int n) {  // LSB first
        acc |= v << nbits;
        nbits += n;
        while (nbits >= 8) { deflate += static_cast<char>(acc & 0xFF); acc >>= 8; nbits -= 8; }
    };
    auto code = [&](uint32_t c, int n) {  // Huffman codes go most significant bit first
        for (int i = n - 1; i >= 0; --i) bits((c >> i) & 1u, 1);
    };
    bits(1, 1);          // the last block
    bits(1, 2);          // fixed codes
    code(0x30, 8);       // literal 0
    for (uint32_t done = 1; done + 258 <= kZeros;) { code(0xC5, 8); code(0, 5); done += 258; }  // length 258 (code 285), distance 1
    const uint32_t tail = (kZeros - 1) % 258;
    for (uint32_t i = 0; i < tail; ++i) code(0x30, 8);
    code(0, 7);          // end of block
    if (nbits) bits(0, 8 - nbits);
    std::string gz = std::string("\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff", 10) + deflate;
    Le32(gz, Crc32(std::string(kZeros, '\0')));
    std::string honest = gz;
    Le32(honest, kZeros);
    Le32(gz, 1000);
    fs::path base = Fresh(L"lie");
    WriteBytes(base / L"lie.gz", gz);
    WriteBytes(base / L"honest.gz", honest);
    SaveSample("sizes-lie.gz", gz);
    auto run = [&](const wchar_t* arc, bool answer, int& asked, uint64_t& askedAt, bool& cancelled) {
        fs::path dest = Fresh(L"lie_dest");
        ExtractOptions o;
        o.largeBytes = 2 << 20;
        o.largeRatio = 50;
        o.onLarge = [&](const LargeExtractInfo& i) { ++asked; askedAt = i.declaredBytes; return answer; };
        cancelled = false;
        try {
            ExtractFile(base / arc, dest, o);
        } catch (const Error& e) {
            cancelled = e.cancelled();
        }
        uint64_t n = 0;
        for (auto& p : Walk(dest)) { std::error_code ec; if (fs::is_regular_file(p, ec)) n += fs::file_size(p, ec); }
        return n;
    };
    int asked = 0;
    uint64_t at = 0;
    bool cancelled = false;
    uint64_t size = run(L"lie.gz", false, asked, at, cancelled);
    EXPECT_MSG(asked == 1 && cancelled && size == 0, ("lie, answer no: asked " + std::to_string(asked) + ", cancelled " + std::to_string(cancelled) + ", left " + std::to_string(size)).c_str());
    std::printf("   answer no: asked %d time(s) at %llu bytes, cancelled %d, left %llu bytes\n", asked, static_cast<unsigned long long>(at), cancelled, static_cast<unsigned long long>(size));
    asked = 0;
    size = run(L"lie.gz", true, asked, at, cancelled);
    EXPECT_MSG(asked == 1 && !cancelled, ("lie, answer yes: asked " + std::to_string(asked) + ", cancelled " + std::to_string(cancelled)).c_str());
    std::printf("   answer yes: asked %d time(s), %llu bytes came out\n", asked, static_cast<unsigned long long>(size));
    // an honest archive is asked about before anything is written, and only then
    asked = 0;
    size = run(L"honest.gz", true, asked, at, cancelled);
    EXPECT_MSG(asked == 1 && size == kZeros, ("honest archive: asked " + std::to_string(asked) + ", size " + std::to_string(size)).c_str());
}

int main(int argc, char** argv) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_root = fs::path(tmp) / (L"lycan_hostile_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(g_root, ec);
    auto want = [&](const char* name) {
        if (argc < 2) return true;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], name) == 0) return true;
        return false;
    };
    try {
        if (want("links")) TestLinks();
        if (want("names")) TestNames();
        if (want("motw")) TestMotw();
        if (want("exec")) TestExecutableTypes();
        if (want("parallel")) TestParallelSameDestination();
        if (want("lie")) TestSizesLie();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: uncaught %s\n", e.what());
        ++failures;
    }
    fs::remove_all(g_root, ec);
    if (failures == 0) std::puts("test_hostile: all passed");
    else std::fprintf(stderr, "test_hostile: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
