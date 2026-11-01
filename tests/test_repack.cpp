// Repacking (repack.h): convert to another format with or without a password, remove the password in place (replaced only
// after the new archive passes the test), damaged sources; "upgrade" an old zip whose names have no UTF-8 mark (update.h
// rewriteNames, verify); new zip archives mark non-ASCII names as UTF-8.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/repack.h"
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

static void WriteFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}
static std::string ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// path -> content of every file of an archive (read with `password`)
static std::map<std::wstring, std::string> Contents(const fs::path& archive, const std::wstring& password = L"", unsigned cp = 0) {
    OpenOptions oo;
    oo.nameCodepage = cp;
    if (!password.empty()) oo.password = [password](std::wstring& out) { out = password; return true; };
    auto src = ArchiveSource::Open(archive, oo);
    std::map<std::wstring, std::string> out;
    struct Sink : ByteSink {
        std::string s;
        void Write(const void* d, size_t n) override { s.append(static_cast<const char*>(d), n); }
    };
    for (auto const& e : src->Entries()) {
        if (e.isDir) continue;
        Sink sink;
        src->ReadEntry(e.index, sink);
        out[e.path] = sink.s;
    }
    return out;
}

static bool Encrypted(const fs::path& archive, const std::wstring& password) {
    OpenOptions oo;
    oo.password = [password](std::wstring& out) { out = password; return true; };
    return ArchiveSource::Open(archive, oo)->Caps().encrypted;
}

static uint32_t Crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFF;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}
static void Put16(std::string& o, uint32_t v) { o += char(v & 0xFF); o += char((v >> 8) & 0xFF); }
static void Put32(std::string& o, uint32_t v) { Put16(o, v & 0xFFFF); Put16(o, v >> 16); }

// An old zip: stored files whose names are in a legacy code page, without the UTF-8 mark (what old Japanese tools made)
static void WriteOldZip(const fs::path& file, const std::vector<std::pair<std::wstring, std::string>>& items, UINT cp) {
    std::string out, central;
    for (auto const& [name, data] : items) {
        char buf[512];
        const int n = WideCharToMultiByte(cp, 0, name.c_str(), -1, buf, sizeof buf, nullptr, nullptr) - 1;
        const std::string raw(buf, n);
        const uint32_t offset = static_cast<uint32_t>(out.size()), crc = Crc32(data), size = static_cast<uint32_t>(data.size());
        Put32(out, 0x04034b50); Put16(out, 10); Put16(out, 0); Put16(out, 0); Put16(out, 0); Put16(out, 0x21);
        Put32(out, crc); Put32(out, size); Put32(out, size); Put16(out, static_cast<uint32_t>(raw.size())); Put16(out, 0);
        out += raw + data;
        Put32(central, 0x02014b50); Put16(central, 20); Put16(central, 10); Put16(central, 0); Put16(central, 0); Put16(central, 0);
        Put16(central, 0x21); Put32(central, crc); Put32(central, size); Put32(central, size);
        Put16(central, static_cast<uint32_t>(raw.size())); Put16(central, 0); Put16(central, 0); Put16(central, 0); Put16(central, 0);
        Put32(central, 0x20); Put32(central, offset);
        central += raw;
    }
    const uint32_t cdOffset = static_cast<uint32_t>(out.size());
    out += central;
    Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0); Put16(out, static_cast<uint32_t>(items.size())); Put16(out, static_cast<uint32_t>(items.size()));
    Put32(out, static_cast<uint32_t>(central.size())); Put32(out, cdOffset); Put16(out, 0);
    WriteFile(file, out);
}

int main() {
    const fs::path dir = fs::temp_directory_path() / (L"lycan_repack_test_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(dir);
    const fs::path in = dir / L"in";
    WriteFile(in / L"資料夾" / L"a.txt", std::string(5000, 'a'));
    WriteFile(in / L"資料夾" / L"子" / L"b.bin", std::string("\x01\x02\x03", 3));
    WriteFile(in / L"readme.txt", "hello");
    fs::create_directories(in / L"empty");
    const std::map<std::wstring, std::string> expected = {
        {L"資料夾/a.txt", std::string(5000, 'a')}, {L"資料夾/子/b.bin", std::string("\x01\x02\x03", 3)}, {L"readme.txt", "hello"}};

    // a new zip marks non-ASCII names as UTF-8 (cu=on): the names come back the same whatever the system code page
    const fs::path zip = dir / L"src.zip";
    {
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.password = L"pw1";
        CreateArchive(zip, {in / L"資料夾", in / L"readme.txt", in / L"empty"}, co);
        auto src = ArchiveSource::Open(zip, [] { OpenOptions o; o.password = [](std::wstring& s) { s = L"pw1"; return true; }; return o; }());
        EXPECT(!src->NameEncoding().applicable);
        EXPECT(Contents(zip, L"pw1") == expected);
    }

    // convert: encrypted zip -> 7z without a password; the old one stays; the counts; the empty folder comes along
    {
        RepackOptions ro;
        ro.open.password = [](std::wstring& s) { s = L"pw1"; return true; };
        ro.compress.type = ArchiveType::SevenZip;
        uint64_t lastDone = 0, lastTotal = 0;
        ro.progress = [&](uint64_t d, uint64_t t) { lastDone = d; lastTotal = t; return true; };
        const fs::path out = dir / L"out.7z";
        auto rep = RepackArchive(zip, out, ro);
        EXPECT(rep.ok() && rep.tested && rep.files == 3 && rep.output == out && fs::exists(out) && fs::exists(zip));
        EXPECT(rep.archiveBefore == fs::file_size(zip) && rep.archiveAfter == fs::file_size(out));
        EXPECT(lastTotal > 0 && lastDone == lastTotal);
        EXPECT(Contents(out) == expected && !Encrypted(out, L""));
        bool emptyDir = false;
        auto converted = ArchiveSource::Open(out);   // (kept in a variable: the list belongs to it)
        for (auto const& e : converted->Entries()) if (e.isDir && e.path == L"empty") emptyDir = true;
        converted.reset();
        EXPECT(emptyDir);
        // the target exists: refused, nothing changed
        bool threw = false;
        try { RepackArchive(zip, out, ro); } catch (Error const&) { threw = true; }
        EXPECT(threw && Contents(out) == expected);
        // with a new password, and to tar.gz (no password possible there)
        ro.compress.password = L"new";
        const fs::path out2 = dir / L"out2.zip";
        ro.compress.type = ArchiveType::Zip;
        EXPECT(RepackArchive(zip, out2, ro).ok() && Encrypted(out2, L"new") && Contents(out2, L"new") == expected);
        ro.compress.password.clear();
        ro.compress.type = ArchiveType::TarGz;
        EXPECT(RepackArchive(zip, dir / L"out3.tar.gz", ro).ok() && Contents(dir / L"out3.tar.gz") == expected);
    }

    // wrong password: refused, nothing made
    {
        RepackOptions ro;
        ro.open.password = [](std::wstring& s) { s = L"nope"; return true; };
        ro.compress.type = ArchiveType::SevenZip;
        bool wrong = false;
        try { RepackArchive(zip, dir / L"wrong.7z", ro); } catch (Error const& e) { wrong = e.wrongPassword(); }
        EXPECT(wrong && !fs::exists(dir / L"wrong.7z"));
    }

    // cancel: nothing left behind
    {
        RepackOptions ro;
        ro.open.password = [](std::wstring& s) { s = L"pw1"; return true; };
        ro.compress.type = ArchiveType::SevenZip;
        int calls = 0;
        ro.progress = [&](uint64_t, uint64_t) { return ++calls < 3; };
        bool cancelled = false;
        try { RepackArchive(zip, dir / L"cancel.7z", ro); } catch (Error const& e) { cancelled = e.cancelled(); }
        EXPECT(cancelled && !fs::exists(dir / L"cancel.7z"));
    }

    // remove the password in place: same format, not encrypted, same content; the scratch file is gone
    {
        const fs::path z = dir / L"inplace.zip";
        fs::copy_file(zip, z);
        RepackOptions ro;
        ro.open.password = [](std::wstring& s) { s = L"pw1"; return true; };
        ro.compress.type = ArchiveType::Zip;
        ro.inPlace = true;
        auto rep = RepackArchive(z, {}, ro);
        EXPECT(rep.ok() && rep.tested && rep.output == z && !fs::exists(dir / L"inplace.zip.lycantmp"));
        EXPECT(!Encrypted(z, L"") && Contents(z) == expected);
    }

    // a damaged source: refused (nothing made), or the damaged item left out with skipDamaged
    {
        const fs::path plainZip = dir / L"plain.zip";
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.level = 0;
        CreateArchive(plainZip, {in / L"資料夾", in / L"readme.txt"}, co);
        std::string raw = ReadFile(plainZip);
        const size_t at = raw.find(std::string(100, 'a'));
        EXPECT(at != std::string::npos);
        raw[at + 50] = 'X';   // the CRC of a.txt no longer matches
        const fs::path bad = dir / L"bad.zip";
        WriteFile(bad, raw);
        RepackOptions ro;
        ro.compress.type = ArchiveType::SevenZip;
        bool refused = false;
        try { RepackArchive(bad, dir / L"bad.7z", ro); } catch (Error const& e) { refused = !e.cancelled(); }
        EXPECT(refused && !fs::exists(dir / L"bad.7z"));
        ro.skipDamaged = true;
        auto rep = RepackArchive(bad, dir / L"bad.7z", ro);
        EXPECT(rep.ok() && rep.skipped.size() == 1 && rep.files == 2);
        auto c = Contents(dir / L"bad.7z");
        EXPECT(c.size() == 2 && c.count(L"readme.txt") && !c.count(L"資料夾/a.txt"));
        // in place, a damaged source is never replaced
        const fs::path badIn = dir / L"bad_inplace.zip";
        WriteFile(badIn, raw);
        RepackOptions ri;
        ri.compress.type = ArchiveType::Zip;
        ri.inPlace = true;
        bool kept = false;
        try { RepackArchive(badIn, {}, ri); } catch (Error const&) { kept = ReadFile(badIn) == raw; }
        EXPECT(kept && !fs::exists(dir / L"bad_inplace.zip.lycantmp"));
    }

    // upgrade: an old Shift-JIS zip without the UTF-8 mark is written again with UTF-8 names, the data copied, tested first
    {
        const fs::path old = dir / L"old.zip";
        const std::vector<std::pair<std::wstring, std::string>> items = {
            {L"日本語のファイル.txt", "nihongo"}, {L"テスト/説明書.txt", "setsumei"}, {L"アーカイブの中身.txt", "nakami"}};
        WriteOldZip(old, items, 932);
        auto before = ArchiveSource::Open(old);
        EXPECT(before->NameEncoding().applicable);
        const unsigned cp = before->NameEncoding().codepage;
        EXPECT(cp == 932);
        before.reset();
        UpdateOptions uo;
        uo.rewriteNames = true;
        uo.verify = true;
        uo.nameCodepage = cp;
        auto rep = UpdateArchive(old, {}, uo);
        EXPECT(rep.changed && rep.ok() && !fs::exists(dir / L"old.zip.lycantmp"));
        auto after = ArchiveSource::Open(old);
        EXPECT(!after->NameEncoding().applicable);
        after.reset();
        auto c = Contents(old);
        EXPECT(c.size() == 3 && c[L"日本語のファイル.txt"] == "nihongo" && c[L"テスト/説明書.txt"] == "setsumei");
        // nothing to upgrade any more: not written again
        EXPECT(!UpdateArchive(old, {}, uo).changed);
    }

    // changing an old zip (not upgrading it): the items that are not touched keep their Shift-JIS names byte for byte, even when
    // the names were decoded with a wrong code page; the new item is in UTF-8 and marked; the encoding is still detected
    {
        const fs::path old = dir / L"old_modify.zip";
        const std::vector<std::pair<std::wstring, std::string>> items = {
            {L"日本語のファイル.txt", "nihongo"}, {L"テスト/説明書.txt", "setsumei"}, {L"アーカイブの中身.txt", "nakami"}};
        WriteOldZip(old, items, 932);
        auto rawName = [](const std::wstring& w, UINT cp) {
            char buf[512];
            const int n = WideCharToMultiByte(cp, 0, w.c_str(), -1, buf, sizeof buf, nullptr, nullptr) - 1;
            return std::string(buf, n);
        };
        const fs::path add = dir / L"新しいファイル.txt";
        WriteFile(add, "atarashii");
        UpdateOptions uo;
        uo.nameCodepage = 950;   // a wrong encoding chosen in the view (Big5)
        auto rep = UpdateArchive(old, {UpdateOp::Add(L"", {add})}, uo);
        EXPECT(rep.changed && rep.ok() && rep.added == 1);
        const std::string raw = ReadFile(old);
        EXPECT(raw.find(rawName(L"日本語のファイル.txt", 932)) != std::string::npos);    // untouched: still Shift-JIS
        EXPECT(raw.find(rawName(L"テスト/説明書.txt", 932)) != std::string::npos);
        EXPECT(raw.find(rawName(L"新しいファイル.txt", CP_UTF8)) != std::string::npos);  // new: UTF-8
        auto src = ArchiveSource::Open(old);
        EXPECT(src->NameEncoding().applicable && src->NameEncoding().codepage == 932);
        src.reset();
        auto c = Contents(old);
        EXPECT(c.size() == 4 && c[L"日本語のファイル.txt"] == "nihongo" && c[L"新しいファイル.txt"] == "atarashii");
        // renaming one old item writes that one in UTF-8; the others stay as they were
        rep = UpdateArchive(old, {UpdateOp::Rename(L"アーカイブの中身.txt", L"中身.txt")}, UpdateOptions{});
        EXPECT(rep.ok() && rep.renamed == 1);
        const std::string raw2 = ReadFile(old);
        EXPECT(raw2.find(rawName(L"中身.txt", CP_UTF8)) != std::string::npos && raw2.find(rawName(L"日本語のファイル.txt", 932)) != std::string::npos);
        c = Contents(old);
        EXPECT(c.size() == 4 && c[L"中身.txt"] == "nakami" && c[L"テスト/説明書.txt"] == "setsumei");
    }

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures == 0) std::puts("test_repack: all passed");
    return failures ? 1 : 0;
}
