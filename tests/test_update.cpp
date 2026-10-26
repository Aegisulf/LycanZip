// Changing an existing archive in place (update.h): add / delete / rename / make folder / replace, for zip, 7z (solid and
// not) and tar; items that are not touched are copied through; a failed or cancelled change leaves the archive as it was.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <set>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/extract.h"
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

static void WriteFile(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary).write(s.data(), static_cast<std::streamsize>(s.size()));
}
static std::string ReadFile(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::string Text(size_t bytes, unsigned seed) {
    static const char* words[] = {"alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet"};
    std::mt19937 rng(seed);
    std::string s;
    while (s.size() < bytes) {
        s += words[rng() % 10];
        s += (rng() % 9 == 0) ? '\n' : ' ';
        if (rng() % 7 == 0) s += std::to_string(rng() % 1000);
    }
    s.resize(bytes);
    return s;
}
static std::string Random(size_t bytes, unsigned seed) {
    std::mt19937_64 rng(seed);
    std::string s(bytes, 0);
    for (size_t i = 0; i + 8 <= bytes; i += 8) { uint64_t v = rng(); memcpy(&s[i], &v, 8); }
    return s;
}

struct Tree {
    std::map<std::string, std::string> files;  // path ('/') -> content
    std::set<std::string> dirs;
    bool operator==(const Tree& o) const { return files == o.files && dirs == o.dirs; }
};

static std::string Norm(std::wstring p) {
    for (auto& c : p) if (c == L'\\') c = L'/';
    while (!p.empty() && p.back() == L'/') p.pop_back();
    return ToUtf8(p);
}

class StringSink final : public ByteSink {
public:
    std::string data;
    void Write(const void* d, size_t n) override { data.append(static_cast<const char*>(d), n); }
};

static PasswordFn PwFn(const std::wstring& pw) {
    if (pw.empty()) return {};
    return [pw](std::wstring& out) { out = pw; return true; };
}

// Everything the archive holds, read through ArchiveSource (also runs the integrity test)
static Tree Dump(const fs::path& arc, const std::wstring& pw = L"", unsigned cp = 0, bool* integrityOk = nullptr) {
    Tree t;
    OpenOptions oo;
    oo.password = PwFn(pw);
    oo.nameCodepage = cp;
    auto src = ArchiveSource::Open(arc, oo);
    for (const Entry& e : src->Entries()) {
        const std::string name = Norm(e.path);
        if (e.isDir) { t.dirs.insert(name); continue; }
        StringSink sink;
        src->ReadEntry(e.index, sink);
        t.files[name] = sink.data;
    }
    if (integrityOk) {
        ExtractOptions eo;
        eo.password = PwFn(pw);
        *integrityOk = TestArchive(*src, eo).ok();
    }
    return t;
}

static std::map<std::string, Entry> Entries(const fs::path& arc, const std::wstring& pw = L"") {
    OpenOptions oo;
    oo.password = PwFn(pw);
    auto src = ArchiveSource::Open(arc, oo);
    std::map<std::string, Entry> m;
    for (const Entry& e : src->Entries()) m[Norm(e.path)] = e;
    return m;
}

// Does the call throw lycan::Error, and does the archive stay byte for byte as it was (and no leftover next to it)?
template <class F>
static bool FailsAndKeeps(const fs::path& arc, F&& f) {
    const std::string before = ReadFile(arc);
    bool threw = false;
    try { f(); } catch (const Error&) { threw = true; }
    return threw && ReadFile(arc) == before && !fs::exists(arc.wstring() + L".lycantmp") && !fs::exists(arc.wstring() + L".bak");
}

struct Fmt {
    const char* label;
    ArchiveType type;
    int64_t solid;  // CompressOptions::solidSize
    const wchar_t* ext;
};

static void BuildSource(const fs::path& src, Tree& model) {
    model = Tree{};
    auto put = [&](const char* rel, std::string data) {
        WriteFile(src / rel, data);
        model.files[rel] = std::move(data);
    };
    put("docs/a.txt", Text(200 * 1024, 1));
    put("docs/b.txt", Text(150 * 1024, 2));
    put("img/p1.bin", Random(100 * 1024, 3));
    put("readme.txt", Text(3000, 4));
    fs::create_directories(src / "emptydir");
    for (const char* d : {"docs", "img", "emptydir"}) model.dirs.insert(d);
}

static void RunFormat(const Fmt& f, const fs::path& root) {
    std::printf("[%s]\n", f.label);
    const fs::path work = root / f.label;
    const fs::path src = work / "src";
    Tree model;
    BuildSource(src, model);
    const bool tar = f.type == ArchiveType::Tar;
    const bool packedKept = f.type == ArchiveType::Zip || (f.type == ArchiveType::SevenZip && f.solid == kSolidOff);

    const fs::path arc = work / (std::wstring(L"t") + f.ext);
    CompressOptions co;
    co.type = f.type;
    co.solidSize = f.solid;
    CreateArchive(arc, {src / "docs", src / "img", src / "readme.txt", src / "emptydir"}, co);
    EXPECT(Dump(arc) == model);
    {
        auto s = ArchiveSource::Open(arc);
        EXPECT(s->Caps().canModify);
    }

    const auto before = Entries(arc);

    // ---- add: into the top level and into a folder (one new file, one folder with a nested file) -------------------
    WriteFile(work / "in" / "new.txt", Text(5000, 10));
    WriteFile(work / "in" / "pack" / "deep" / "x.txt", Text(3000, 11));
    {
        auto rep = UpdateArchive(arc, {UpdateOp::Add(L"docs", {work / "in" / "new.txt"}), UpdateOp::Add(L"", {work / "in" / "pack"})});
        EXPECT(rep.ok() && rep.changed);
        EXPECT(rep.added == 2);       // new.txt, x.txt
        EXPECT(rep.dirsCreated == 2); // pack, pack\deep
        model.files["docs/new.txt"] = ReadFile(work / "in" / "new.txt");
        model.files["pack/deep/x.txt"] = ReadFile(work / "in" / "pack" / "deep" / "x.txt");
        model.dirs.insert("pack");
        model.dirs.insert("pack/deep");
        bool ok = false;
        EXPECT(Dump(arc, L"", 0, &ok) == model);
        EXPECT(ok);
        const auto after = Entries(arc);
        for (const char* k : {"docs/a.txt", "img/p1.bin", "readme.txt"}) {
            EXPECT(after.count(k) && before.count(k));
            if (!after.count(k) || !before.count(k)) continue;
            EXPECT(after.at(k).mtimeFileTime == before.at(k).mtimeFileTime);
            if (packedKept) EXPECT(after.at(k).packedSize == before.at(k).packedSize);  // copied, not compressed again
        }
    }

    // ---- delete: a file, and a folder with what is below it ------------------------------------------------------------
    {
        auto rep = UpdateArchive(arc, {UpdateOp::Delete(L"docs/b.txt"), UpdateOp::Delete(L"img")});
        EXPECT(rep.ok() && rep.deleted == 3);  // b.txt, img, img/p1.bin
        model.files.erase("docs/b.txt");
        model.files.erase("img/p1.bin");
        model.dirs.erase("img");
        EXPECT(Dump(arc) == model);
    }

    // ---- rename: a file, a folder with its children, a move to another folder -------------------------------------------
    {
        auto rep = UpdateArchive(arc, {UpdateOp::Rename(L"readme.txt", L"README-1.txt"), UpdateOp::Rename(L"docs", L"documents"),
                                       UpdateOp::Rename(L"pack/deep/x.txt", L"documents/x.txt")});
        EXPECT(rep.ok() && rep.renamed == 5);  // readme.txt, docs + 2 files, x.txt
        std::map<std::string, std::string> nf;
        for (auto& [k, v] : model.files) {
            std::string n = k;
            if (n == "readme.txt") n = "README-1.txt";
            else if (n.rfind("docs/", 0) == 0) n = "documents/" + n.substr(5);
            else if (n == "pack/deep/x.txt") n = "documents/x.txt";
            nf[n] = v;
        }
        model.files = nf;
        model.dirs.erase("docs");
        model.dirs.insert("documents");
        EXPECT(Dump(arc) == model);
        // the data of renamed items is not touched
        if (packedKept) {
            const auto after = Entries(arc);
            EXPECT(after.at("documents/a.txt").packedSize == before.at("docs/a.txt").packedSize);
        }
    }

    // ---- make a folder, replace the content of a file -------------------------------------------------------------------
    {
        WriteFile(work / "in" / "edited.txt", "edited content\n");
        auto rep = UpdateArchive(arc, {UpdateOp::MakeDir(L"fresh/inner"), UpdateOp::Replace(L"documents/a.txt", work / "in" / "edited.txt")});
        EXPECT(rep.ok() && rep.dirsCreated == 1 && rep.replaced == 1);
        model.dirs.insert("fresh/inner");
        model.files["documents/a.txt"] = "edited content\n";
        EXPECT(Dump(arc) == model);
    }

    // ---- adding a file that exists replaces it; the option says no ------------------------------------------------------
    {
        WriteFile(work / "in2" / "a.txt", "second version\n");
        UpdateOptions uo;
        uo.replaceSameName = false;
        EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Add(L"documents", {work / "in2" / "a.txt"})}, uo); }));
        auto rep = UpdateArchive(arc, {UpdateOp::Add(L"documents", {work / "in2" / "a.txt"})});
        EXPECT(rep.ok() && rep.replaced == 1 && rep.added == 0);
        model.files["documents/a.txt"] = "second version\n";
        EXPECT(Dump(arc) == model);
    }

    // ---- bad operations throw before anything is written ---------------------------------------------------------------
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Delete(L"nothing/here.txt")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Rename(L"nothing", L"x")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Rename(L"README-1.txt", L"documents")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Rename(L"documents", L"documents/inner")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Rename(L"README-1.txt", L"../escape.txt")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::MakeDir(L"a:b")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::MakeDir(L"documents")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Replace(L"documents", work / "in" / "new.txt")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Replace(L"README-1.txt", work / "in" / "missing.txt")}); }));
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Add(L"", {work / "in" / "missing.txt"})}); }));
    // a file where a folder is (and the other way round)
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Add(L"", {work / "in2" / "a.txt"}), UpdateOp::Rename(L"a.txt", L"documents")}); }));
    // operations that change nothing do not rewrite the archive
    {
        const std::string b = ReadFile(arc);
        auto rep = UpdateArchive(arc, {UpdateOp::Rename(L"README-1.txt", L"README-1.txt")});
        EXPECT(!rep.changed && ReadFile(arc) == b);
    }

    // ---- cancel, a full disk, too little room: the archive stays as it was ---------------------------------------------
    {
        WriteFile(work / "big" / "big.bin", Random(6u << 20, 20));
        UpdateOptions uo;
        uo.progress = [](uint64_t, uint64_t) { return false; };
        const std::string b = ReadFile(arc);
        bool cancelled = false;
        try { UpdateArchive(arc, {UpdateOp::Add(L"", {work / "big" / "big.bin"})}, uo); } catch (const Error& e) { cancelled = e.cancelled(); }
        EXPECT(cancelled && ReadFile(arc) == b && !fs::exists(arc.wstring() + L".lycantmp"));

        SetTestWriteFault(100);
        bool full = false;
        try { UpdateArchive(arc, {UpdateOp::Delete(L"README-1.txt")}); } catch (const Error& e) { full = e.diskFull(); }
        SetTestWriteFault(0);
        EXPECT(full && ReadFile(arc) == b && !fs::exists(arc.wstring() + L".lycantmp"));

        SetTestFreeSpace(1000);
        bool space = false;
        try { UpdateArchive(arc, {UpdateOp::Delete(L"README-1.txt")}); } catch (const Error& e) { space = e.diskFull(); }
        SetTestFreeSpace(0);
        EXPECT(space && ReadFile(arc) == b);
        EXPECT(Dump(arc) == model);
    }

    // ---- keepBackup leaves the old archive next to the new one ----------------------------------------------------------
    {
        const std::string b = ReadFile(arc);
        UpdateOptions uo;
        uo.keepBackup = true;
        UpdateArchive(arc, {UpdateOp::Delete(L"README-1.txt")}, uo);
        model.files.erase("README-1.txt");
        EXPECT(fs::exists(arc.wstring() + L".bak") && ReadFile(arc.wstring() + L".bak") == b);
        EXPECT(Dump(arc) == model);
        EXPECT(Dump(arc.wstring() + L".bak").files.count("README-1.txt") == 1);
        fs::remove(arc.wstring() + L".bak");
        UpdateArchive(arc, {UpdateOp::Delete(L"fresh")});  // without the option no backup appears
        model.dirs.erase("fresh/inner");
        EXPECT(!fs::exists(arc.wstring() + L".bak"));
        EXPECT(Dump(arc) == model);
    }
    (void)tar;
}

// ---- encrypted archives: the password is needed, new items are encrypted too -----------------------------------------
static void RunEncrypted(const fs::path& root, ArchiveType type, bool hideNames, const char* label) {
    std::printf("[%s]\n", label);
    const fs::path work = root / label;
    Tree model;
    BuildSource(work / "src", model);
    const fs::path arc = work / (type == ArchiveType::Zip ? L"e.zip" : L"e.7z");
    CompressOptions co;
    co.type = type;
    co.password = L"Secret1";
    co.encryptFileNames = hideNames;
    CreateArchive(arc, {work / "src" / "docs", work / "src" / "readme.txt"}, co);
    model.files.erase("img/p1.bin");
    model.dirs.erase("img");
    model.dirs.erase("emptydir");
    EXPECT(Dump(arc, L"Secret1") == model);

    WriteFile(work / "in" / "n.txt", Text(4000, 30));
    UpdateOptions uo;
    uo.compress.password = L"Secret1";
    // no password / a wrong one: refused, archive untouched
    EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Delete(L"readme.txt")}); }));
    {
        UpdateOptions bad;
        bad.compress.password = L"Wrong99";
        EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Delete(L"readme.txt")}, bad); }));
    }
    auto rep = UpdateArchive(arc, {UpdateOp::Add(L"", {work / "in" / "n.txt"}), UpdateOp::Rename(L"docs/a.txt", L"docs/a2.txt"),
                                   UpdateOp::Delete(L"readme.txt")}, uo);
    EXPECT(rep.ok() && rep.changed);
    model.files["n.txt"] = ReadFile(work / "in" / "n.txt");
    model.files["docs/a2.txt"] = model.files["docs/a.txt"];
    model.files.erase("docs/a.txt");
    model.files.erase("readme.txt");
    bool ok = false;
    EXPECT(Dump(arc, L"Secret1", 0, &ok) == model);
    EXPECT(ok);
    // every file is still encrypted, the new one included
    bool allEnc = true;
    for (auto& [k, e] : Entries(arc, L"Secret1")) if (!e.isDir && !e.isEncrypted) allEnc = false;
    EXPECT(allEnc);
    if (hideNames) {
        // the names stay hidden: the archive cannot even be listed without the password
        bool threw = false;
        try { ArchiveSource::Open(arc); } catch (const Error&) { threw = true; }
        EXPECT(threw);
    }
}

// ---- zip names without a charset mark (Big5 here): the whole archive ends up with readable names -------------------------
static uint32_t Crc32(const std::string& s) {
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char b : s) {
        c ^= b;
        for (int k = 0; k < 8; ++k) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return ~c;
}
static void Le(std::string& o, uint32_t v, int n) { for (int i = 0; i < n; ++i) o.push_back(static_cast<char>((v >> (8 * i)) & 0xFF)); }
static std::string LegacyZip(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string out, central;
    for (auto& [name, data] : files) {
        const uint32_t off = static_cast<uint32_t>(out.size()), crc = Crc32(data);
        Le(out, 0x04034b50, 4); Le(out, 20, 2); Le(out, 0, 2); Le(out, 0, 2); Le(out, 0, 2); Le(out, 0x21, 2);
        Le(out, crc, 4); Le(out, static_cast<uint32_t>(data.size()), 4); Le(out, static_cast<uint32_t>(data.size()), 4);
        Le(out, static_cast<uint32_t>(name.size()), 2); Le(out, 0, 2);
        out += name; out += data;
        Le(central, 0x02014b50, 4); Le(central, 20, 2); Le(central, 20, 2); Le(central, 0, 2); Le(central, 0, 2); Le(central, 0, 2);
        Le(central, 0x21, 2); Le(central, crc, 4); Le(central, static_cast<uint32_t>(data.size()), 4);
        Le(central, static_cast<uint32_t>(data.size()), 4); Le(central, static_cast<uint32_t>(name.size()), 2);
        Le(central, 0, 2); Le(central, 0, 2); Le(central, 0, 2); Le(central, 0, 2); Le(central, 0, 4); Le(central, off, 4);
        central += name;
    }
    const uint32_t cdOff = static_cast<uint32_t>(out.size());
    out += central;
    Le(out, 0x06054b50, 4); Le(out, 0, 2); Le(out, 0, 2); Le(out, static_cast<uint32_t>(files.size()), 2);
    Le(out, static_cast<uint32_t>(files.size()), 2); Le(out, static_cast<uint32_t>(central.size()), 4); Le(out, cdOff, 4); Le(out, 0, 2);
    return out;
}

static void RunLegacyNames(const fs::path& root) {
    std::printf("[legacy zip names]\n");
    const fs::path work = root / "legacy";
    const fs::path arc = work / L"b.zip";
    // Big5: 測試.txt, 資料夾\說明.txt
    WriteFile(arc, LegacyZip({{"\xB4\xFA\xB8\xD5.txt", "one"}, {"\xB8\xEA\xAE\xC6\xA7\xA8/\xBB\xA1\xA9\xFA.txt", "two"}}));
    WriteFile(work / "in" / "new.txt", "three");
    UpdateOptions uo;
    uo.nameCodepage = 950;
    auto rep = UpdateArchive(arc, {UpdateOp::Add(L"", {work / "in" / "new.txt"}), UpdateOp::Rename(L"測試.txt", L"測試2.txt")}, uo);
    EXPECT(rep.ok() && rep.changed);
    // no help needed any more: the names are Unicode now
    Tree t = Dump(arc);
    EXPECT(t.files.count(ToUtf8(L"測試2.txt")) && t.files[ToUtf8(L"測試2.txt")] == "one");
    EXPECT(t.files.count(ToUtf8(L"資料夾/說明.txt")) && t.files[ToUtf8(L"資料夾/說明.txt")] == "two");
    EXPECT(t.files.count("new.txt"));
}

// ---- what cannot be changed -------------------------------------------------------------------------------------------
static void RunUnsupported(const fs::path& root) {
    std::printf("[unsupported]\n");
    const fs::path work = root / "unsup";
    WriteFile(work / "src" / "a.txt", Text(3000, 40));
    {
        const fs::path arc = work / L"t.tar.gz";
        CompressOptions co;
        co.type = ArchiveType::TarGz;
        CreateArchive(arc, {work / "src" / "a.txt"}, co);
        EXPECT(!ArchiveSource::Open(arc)->Caps().canModify);
        EXPECT(FailsAndKeeps(arc, [&] { UpdateArchive(arc, {UpdateOp::Delete(L"a.txt")}); }));
    }
    {
        const fs::path arc = work / L"v.7z";
        CompressOptions co;
        co.type = ArchiveType::SevenZip;
        co.volumeSize = 64 * 1024;
        WriteFile(work / "src" / "big.bin", Random(300 * 1024, 41));
        CreateArchive(arc, {work / "src" / "big.bin"}, co);
        const fs::path first = work / L"v.7z.001";
        EXPECT(fs::exists(first));
        EXPECT(!ArchiveSource::Open(first)->Caps().canModify);
        EXPECT(FailsAndKeeps(first, [&] { UpdateArchive(first, {UpdateOp::Delete(L"big.bin")}); }));
    }
    EXPECT(FailsAndKeeps(work / L"src" / L"a.txt", [&] { UpdateArchive(work / L"src" / L"a.txt", {UpdateOp::Delete(L"a.txt")}); }));
    // a missing archive
    bool threw = false;
    try { UpdateArchive(work / L"nope.zip", {UpdateOp::Delete(L"a")}); } catch (const Error&) { threw = true; }
    EXPECT(threw);
}

// ---- solid 7z: what would be unpacked and packed again ------------------------------------------------------------------
static void RunSolid(const fs::path& root) {
    std::printf("[solid blocks]\n");
    const fs::path work = root / "solid";
    std::map<std::string, std::string> data;
    for (int i = 1; i <= 4; ++i) {
        const std::string n = "f" + std::to_string(i) + ".txt";
        data[n] = Text(300 * 1024, 50 + i);
        WriteFile(work / "src" / n, data[n]);
    }
    const fs::path arc = work / L"s.7z";
    CompressOptions co;
    co.type = ArchiveType::SevenZip;
    co.solidSize = 700 * 1024;  // a block takes what fits under the limit: [f1 f2] [f3 f4]
    CreateArchive(arc, {work / "src" / "f1.txt", work / "src" / "f2.txt", work / "src" / "f3.txt", work / "src" / "f4.txt"}, co);
    EXPECT(ArchiveSource::Open(arc)->Caps().solid);
    const auto before = Entries(arc);

    WriteFile(work / "in" / "x.txt", "x");
    const uint64_t total = 4 * 300 * 1024;
    // adding, renaming, making a folder: nothing is unpacked
    EXPECT(EstimateUpdate(arc, {UpdateOp::Add(L"", {work / "in" / "x.txt"}), UpdateOp::Rename(L"f1.txt", L"g1.txt"), UpdateOp::MakeDir(L"d")}).recompressBytes == 0);
    // deleting f1 recompresses what stays in its block (f2) and nothing else
    auto c = EstimateUpdate(arc, {UpdateOp::Delete(L"f1.txt")});
    EXPECT(c.recompressBytes > 0 && c.recompressBytes < total);
    EXPECT(c.archiveBytes == fs::file_size(arc));
    // replacing counts the same way, the new data is counted as new
    auto r = EstimateUpdate(arc, {UpdateOp::Replace(L"f2.txt", work / "in" / "x.txt")});
    EXPECT(r.recompressBytes > 0 && r.newBytes == 1);

    UpdateArchive(arc, {UpdateOp::Delete(L"f1.txt")});
    data.erase("f1.txt");
    Tree t = Dump(arc);
    EXPECT(t.files == data);
    // the other block was copied as it is
    const auto after = Entries(arc);
    EXPECT(after.at("f3.txt").packedSize == before.at("f3.txt").packedSize);

    // not solid: nothing to recompress, whatever is deleted
    const fs::path arc2 = work / L"n.7z";
    co.solidSize = kSolidOff;
    CreateArchive(arc2, {work / "src" / "f2.txt", work / "src" / "f3.txt"}, co);
    EXPECT(EstimateUpdate(arc2, {UpdateOp::Delete(L"f2.txt")}).recompressBytes == 0);
}

// An archive that is open elsewhere (the app keeps it open while the user works) can still be replaced: the open one goes on
// showing the old content, a new open sees the new one.
static void RunOpenElsewhere(const fs::path& root) {
    std::printf("[open elsewhere]" "\n");
    const fs::path work = root / "openelse";
    WriteFile(work / "src" / "a.txt", Text(5000, 70));
    for (const wchar_t* ext : {L".zip", L".7z"}) {
        const fs::path arc = work / (std::wstring(L"o") + ext);
        CompressOptions co;
        co.type = std::wstring(ext) == L".zip" ? ArchiveType::Zip : ArchiveType::SevenZip;
        CreateArchive(arc, {work / "src" / "a.txt"}, co);
        auto held = ArchiveSource::Open(arc);
        bool threw = false;
        try { UpdateArchive(arc, {UpdateOp::Rename(L"a.txt", L"b.txt")}); } catch (const Error&) { threw = true; }
        EXPECT(!threw);
        EXPECT(held->Entries().size() == 1 && Norm(held->Entries()[0].path) == "a.txt");
        EXPECT(Dump(arc).files.count("b.txt") == 1);
        held.reset();
        EXPECT(!fs::exists(arc.wstring() + L".lycantmp"));
    }
}

int main() {
    const fs::path root = fs::temp_directory_path() / (L"lycan_test_update_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);

    RunFormat({"zip", ArchiveType::Zip, kSolidAuto, L".zip"}, root);
    RunFormat({"7z solid", ArchiveType::SevenZip, kSolidAuto, L".7z"}, root);
    RunFormat({"7z not solid", ArchiveType::SevenZip, kSolidOff, L".7z"}, root);
    RunFormat({"tar", ArchiveType::Tar, kSolidAuto, L".tar"}, root);
    RunEncrypted(root, ArchiveType::Zip, false, "zip encrypted");
    RunEncrypted(root, ArchiveType::SevenZip, false, "7z encrypted");
    RunEncrypted(root, ArchiveType::SevenZip, true, "7z encrypted names");
    RunLegacyNames(root);
    RunUnsupported(root);
    RunSolid(root);
    RunOpenElsewhere(root);

    fs::remove_all(root, ec);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
