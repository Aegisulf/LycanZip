// Smart store (CompressOptions::storeCompressed): files that are compressed already go in as they are; 7z picks "solid or
// not" by itself. Checked with the packed size of every item (stored = exactly the size) and a full read-back.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <random>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/incompressible.h"
#include "lycan/presets.h"
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
static PasswordFn PwFn(const std::wstring& pw) {
    if (pw.empty()) return {};
    return [pw](std::wstring& out) { out = pw; return true; };
}

struct Info {
    uint64_t size = 0, packed = 0;
    bool dir = false;
};
// every item: its size and packed size; also reads every file back and compares it with the source folder
static std::map<std::string, Info> Check(const fs::path& arc, const fs::path& srcRoot, const std::wstring& pw = L"",
                                         const std::map<std::string, std::string>& extra = {}) {
    std::map<std::string, Info> m;
    OpenOptions oo;
    oo.password = PwFn(pw);
    auto src = ArchiveSource::Open(arc, oo);
    class Sink final : public ByteSink {
    public:
        std::string d;
        void Write(const void* p, size_t n) override { d.append(static_cast<const char*>(p), n); }
    };
    for (const Entry& e : src->Entries()) {
        std::string name = ToUtf8(e.path);
        m[name] = {e.size, e.packedSize, e.isDir};
        if (e.isDir) continue;
        Sink s;
        src->ReadEntry(e.index, s);
        std::string want;
        auto x = extra.find(name);
        if (x != extra.end()) want = x->second;
        else want = ReadFile(srcRoot / fs::path(name));
        if (s.d != want) { std::fprintf(stderr, "content differs: %s\n", name.c_str()); ++failures; }
    }
    ExtractOptions eo;
    eo.password = PwFn(pw);
    EXPECT(TestArchive(*src, eo).ok());
    return m;
}

static bool Stored(const std::map<std::string, Info>& m, const char* name) {
    auto it = m.find(name);
    return it != m.end() && it->second.packed == it->second.size;
}

static void MakeSource(const fs::path& src) {
    WriteFile(src / "doc.txt", Text(300 * 1024, 1));
    WriteFile(src / "more.txt", Text(100 * 1024, 2));
    WriteFile(src / "photo.jpg", Random(300 * 1024, 3));
    WriteFile(src / "Movie.MP4", Random(200 * 1024, 4));
    WriteFile(src / "tiny.png", Random(10 * 1024, 5));      // too small to be worth it
    WriteFile(src / "noext", Text(80 * 1024, 6));           // not a known type, and compressible: packed (probe.h looks at the content)
}

int main() {
    const fs::path root = fs::temp_directory_path() / (L"lycan_test_store_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    const fs::path src = root / "src";
    MakeSource(src);
    auto inputs = [&] {
        std::vector<fs::path> v;
        for (const char* n : {"doc.txt", "more.txt", "photo.jpg", "Movie.MP4", "tiny.png", "noext"}) v.push_back(src / n);
        return v;
    }();

    // the list itself
    EXPECT(IsAlreadyCompressedName(L"a/b/Photo.JPG") && IsAlreadyCompressedName(L"x.docx") && IsAlreadyCompressedName(L"c\\d.7z"));
    EXPECT(!IsAlreadyCompressedName(L"a.txt") && !IsAlreadyCompressedName(L"noext") && !IsAlreadyCompressedName(L"dir.jpg/file"));

    for (ArchiveType type : {ArchiveType::Zip, ArchiveType::SevenZip}) {
        const wchar_t* ext = type == ArchiveType::Zip ? L".zip" : L".7z";
        std::printf("[%s: mixed]\n", type == ArchiveType::Zip ? "zip" : "7z");
        // mixed: the two big compressed files are stored, the rest is packed
        {
            CompressOptions co;
            co.type = type;
            std::vector<std::pair<uint64_t, uint64_t>> progress;
            co.progress = [&](uint64_t d, uint64_t t) { progress.push_back({d, t}); return true; };
            const fs::path arc = root / (std::wstring(L"mixed") + ext);
            auto rep = CreateArchive(arc, inputs, co);
            EXPECT(rep.ok() && rep.storedFiles == 2);
            auto m = Check(arc, src);
            EXPECT(Stored(m, "photo.jpg") && Stored(m, "Movie.MP4"));
            EXPECT(!Stored(m, "doc.txt") && !Stored(m, "more.txt") && !Stored(m, "tiny.png") && !Stored(m, "noext"));
            EXPECT(m["doc.txt"].packed < m["doc.txt"].size / 2);
            // progress only goes forward and ends at the total
            bool mono = !progress.empty();
            for (size_t i = 1; i < progress.size(); ++i) if (progress[i].first < progress[i - 1].first) mono = false;
            EXPECT(mono);
            EXPECT(progress.empty() || progress.back().first <= progress.back().second + 1);
        }
        // switched off: nothing is stored, packing a jpg only makes it bigger
        {
            CompressOptions co;
            co.type = type;
            co.storeCompressed = false;
            const fs::path arc = root / (std::wstring(L"off") + ext);
            auto rep = CreateArchive(arc, inputs, co);
            EXPECT(rep.ok() && rep.storedFiles == 0);
            auto m = Check(arc, src);
            EXPECT(!Stored(m, "photo.jpg"));
        }
        // level 0 stores everything anyway: smart store has nothing to do
        {
            CompressOptions co;
            co.type = type;
            co.level = 0;
            auto rep = CreateArchive(root / (std::wstring(L"l0") + ext), inputs, co);
            EXPECT(rep.ok() && rep.storedFiles == 0);
        }
        // only compressed files / none: one pass either way
        {
            CompressOptions co;
            co.type = type;
            auto rep = CreateArchive(root / (std::wstring(L"onlyc") + ext), {src / "photo.jpg", src / "Movie.MP4"}, co);
            EXPECT(rep.ok() && rep.storedFiles == 2);
            auto m = Check(root / (std::wstring(L"onlyc") + ext), src);
            EXPECT(Stored(m, "photo.jpg") && Stored(m, "Movie.MP4"));
            rep = CreateArchive(root / (std::wstring(L"noc") + ext), {src / "doc.txt", src / "more.txt"}, co);
            EXPECT(rep.ok() && rep.storedFiles == 0);
        }
        // a folder mixes in too (folder entries must survive the two passes)
        {
            WriteFile(src / "sub" / "deep" / "pic.jpg", Random(100 * 1024, 7));
            WriteFile(src / "sub" / "deep" / "note.txt", Text(5000, 8));
            fs::create_directories(src / "sub" / "empty");
            CompressOptions co;
            co.type = type;
            const fs::path arc = root / (std::wstring(L"folder") + ext);
            auto rep = CreateArchive(arc, {src / "sub"}, co);
            EXPECT(rep.ok() && rep.storedFiles == 1);
            auto m = Check(arc, src);
            EXPECT(Stored(m, "sub/deep/pic.jpg") && m.count("sub/empty") && m["sub/empty"].dir);
        }
        // cancel in the middle of the two passes: no archive, no scratch file
        {
            CompressOptions co;
            co.type = type;
            co.progress = [](uint64_t, uint64_t) { return false; };
            const fs::path arc = root / (std::wstring(L"cancel") + ext);
            bool cancelled = false;
            try { CreateArchive(arc, inputs, co); } catch (const Error& e) { cancelled = e.cancelled(); }
            EXPECT(cancelled && !fs::exists(arc) && !fs::exists(arc.wstring() + L".lycantmp"));
        }
        // no room for the first archive: the files are packed the usual way and a warning says so
        {
            CompressOptions co;
            co.type = type;
            SetTestFreeSpace(1000);
            const fs::path arc = root / (std::wstring(L"nospace") + ext);
            auto rep = CreateArchive(arc, inputs, co);
            SetTestFreeSpace(0);
            EXPECT(rep.ok() && rep.storedFiles == 0 && !rep.warnings.empty());
            Check(arc, src);
        }
    }

    // encrypted: the stored files are encrypted too; a 7z with hidden names stays hidden
    for (int variant = 0; variant < 3; ++variant) {
        const bool zip = variant == 0;
        const bool hide = variant == 2;
        std::printf("[encrypted %s%s]\n", zip ? "zip" : "7z", hide ? " (names hidden)" : "");
        CompressOptions co;
        co.type = zip ? ArchiveType::Zip : ArchiveType::SevenZip;
        co.password = L"Secret1";
        co.encryptFileNames = hide;
        const fs::path arc = root / (zip ? L"enc.zip" : hide ? L"enc_h.7z" : L"enc.7z");
        auto rep = CreateArchive(arc, inputs, co);
        EXPECT(rep.ok() && rep.storedFiles == 2);
        auto m = Check(arc, src, L"Secret1");
        // AES adds a header and a tag to each item: stored items are the size plus a small, fixed amount
        EXPECT(m["photo.jpg"].packed >= m["photo.jpg"].size && m["photo.jpg"].packed < m["photo.jpg"].size + 64);
        {
            auto s = ArchiveSource::Open(arc, [&] { OpenOptions o; o.password = PwFn(L"Secret1"); return o; }());
            bool allEnc = true;
            for (const Entry& e : s->Entries()) if (!e.isDir && !e.isEncrypted) allEnc = false;
            EXPECT(allEnc);
        }
        if (hide) {
            bool threw = false;
            try { ArchiveSource::Open(arc); } catch (const Error&) { threw = true; }
            EXPECT(threw);
        }
    }

    // 7z: solid or not is chosen from what is compressed
    {
        std::printf("[7z solid choice]\n");
        for (int i = 0; i < 8; ++i) WriteFile(src / "pics" / ("p" + std::to_string(i) + ".jpg"), Random(20 * 1024, 100 + i));  // small: not stored
        for (int i = 0; i < 4; ++i) WriteFile(src / "texts" / ("t" + std::to_string(i) + ".txt"), Text(40 * 1024, 200 + i));
        CompressOptions co;
        co.type = ArchiveType::SevenZip;
        CreateArchive(root / "pics.7z", {src / "pics"}, co);
        EXPECT(!ArchiveSource::Open(root / "pics.7z")->Caps().solid);   // all compressed: not solid, each picture can be read alone
        CreateArchive(root / "texts.7z", {src / "texts"}, co);
        EXPECT(ArchiveSource::Open(root / "texts.7z")->Caps().solid);   // text: solid
        co.solidSize = kSolidAll;
        CreateArchive(root / "pics_all.7z", {src / "pics"}, co);
        EXPECT(ArchiveSource::Open(root / "pics_all.7z")->Caps().solid);  // asked for: obeyed
        co.solidSize = kSolidAuto;
        co.profile = Profile::Smallest;
        CreateArchive(root / "pics_small.7z", {src / "pics"}, co);
        EXPECT(ArchiveSource::Open(root / "pics_small.7z")->Caps().solid);  // "smallest" keeps 7-Zip's own choice
    }

    // changing an archive: new files are stored / packed the same way, what was in it stays as it is
    {
        std::printf("[update]\n");
        for (ArchiveType type : {ArchiveType::Zip, ArchiveType::SevenZip}) {
            const wchar_t* ext = type == ArchiveType::Zip ? L".zip" : L".7z";
            const fs::path arc = root / (std::wstring(L"upd") + ext);
            CompressOptions co;
            co.type = type;
            CreateArchive(arc, {src / "doc.txt"}, co);
            const auto before = Check(arc, src);
            WriteFile(root / "in" / "added.jpg", Random(150 * 1024, 300));
            WriteFile(root / "in" / "added.txt", Text(60 * 1024, 301));
            UpdateOptions uo;
            auto rep = UpdateArchive(arc, {UpdateOp::Add(L"", {root / "in" / "added.jpg", root / "in" / "added.txt"})}, uo);
            EXPECT(rep.ok() && rep.storedFiles == 1);
            auto m = Check(arc, src, L"", {{"added.jpg", ReadFile(root / "in" / "added.jpg")}, {"added.txt", ReadFile(root / "in" / "added.txt")}});
            EXPECT(Stored(m, "added.jpg") && !Stored(m, "added.txt"));
            EXPECT(m["doc.txt"].packed == before.at("doc.txt").packed);  // the old item was copied
            // only a compressed file: one pass, stored
            WriteFile(root / "in" / "only.png", Random(90 * 1024, 302));
            rep = UpdateArchive(arc, {UpdateOp::Add(L"", {root / "in" / "only.png"})}, uo);
            EXPECT(rep.ok() && rep.storedFiles == 1);
            // switched off
            UpdateOptions off;
            off.compress.storeCompressed = false;
            WriteFile(root / "in" / "off.jpg", Random(90 * 1024, 303));
            rep = UpdateArchive(arc, {UpdateOp::Add(L"", {root / "in" / "off.jpg"})}, off);
            EXPECT(rep.ok() && rep.storedFiles == 0);
        }
    }

    // ZipCrypto (compatible profile + password): 7z.dll cannot copy ZipCrypto items between the two passes, so no store pass
    {
        std::printf("[zip ZipCrypto]\n");
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.profile = Profile::Compatible;
        co.password = L"Secret1";
        const fs::path arc = root / "crypto.zip";
        auto rep = CreateArchive(arc, inputs, co);
        EXPECT(rep.ok() && rep.storedFiles == 0);
        Check(arc, src, L"Secret1");
    }

    // presets: the setting is part of a preset; "smallest" does not store
    {
        std::printf("[presets]\n");
        for (const auto& p : BuiltinPresets()) EXPECT(p.storeCompressed == (p.name != L"Smallest size"));
        CompressPreset p;
        p.name = L"mine";
        p.storeCompressed = false;
        const fs::path file = root / "presets.ini";
        EXPECT(SavePresets(file, {p}));
        auto back = LoadPresets(file);
        EXPECT(back.size() == 1 && !back[0].storeCompressed);
        CompressPreset q = p;
        q.storeCompressed = true;
        EXPECT(!p.SameSettings(q));
        CompressOptions o;
        ApplyPreset(p, o);
        EXPECT(!o.storeCompressed);
    }

    // presets: the two checksum file settings are part of a preset (off unless asked; the built-in "e-mail / share" and "backup" ask)
    {
        std::printf("[presets: checksum files]\n");
        for (const auto& p : BuiltinPresets()) {
            const bool a = p.name == L"Email / share" || p.name == L"Backup", f = p.name == L"Backup";
            EXPECT(p.sumsArchive == a && p.sumsFiles == f);
        }
        CompressPreset p;
        p.name = L"sums";
        p.sumsArchive = true;
        const fs::path file = root / "presets_sums.ini";
        EXPECT(SavePresets(file, {p}));
        auto back = LoadPresets(file);
        EXPECT(back.size() == 1 && back[0].sumsArchive && !back[0].sumsFiles);
        CompressPreset q = p;
        q.sumsFiles = true;
        EXPECT(!p.SameSettings(q));
        std::ofstream(root / "old.ini") << "[old]\nformat=zip\n";   // a file of an earlier version has no such keys: off
        auto old = LoadPresets(root / "old.ini");
        EXPECT(old.size() == 1 && !old[0].sumsArchive && !old[0].sumsFiles);
    }

    fs::remove_all(root, ec);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("OK\n");
    return 0;
}
