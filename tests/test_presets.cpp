// Compression presets (presets.ini) and the advanced compression options (method / dictionary / solid block / path mode / custom parameters)
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/presets.h"
#include "lycan/shellitems.h"

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
    std::ofstream(p, std::ios::binary) << s;
}

static bool HasEntry(const fs::path& archive, const std::wstring& needle) {
    auto src = ArchiveSource::Open(archive);
    for (auto& e : src->Entries())
        if (e.path.find(needle) != std::wstring::npos) return true;
    return false;
}

int main() {
    fs::path dir = fs::temp_directory_path() / (L"lycan_presets_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);

    // ---- Built-in presets ----
    auto builtins = BuiltinPresets();
    EXPECT(builtins.size() == 7);
    bool has7z = false, hasStore = false;
    for (auto& b : builtins) {
        EXPECT(b.builtin && IsValidPresetName(b.name));
        if (b.name == L"General 7z") { has7z = b.type == ArchiveType::SevenZip && b.profile == Profile::Balanced && b.method.empty() && b.level == -1; }
        if (b.name == L"Store only, no compression") hasStore = b.type == ArchiveType::Zip && b.level == 0;
    }
    EXPECT(has7z && hasStore);
    // the context menu lists the built-in presets from its own table (shellitems.h): it must match
    EXPECT(builtins.size() == std::size(kBuiltinPresetNames));
    for (size_t i = 0; i < builtins.size() && i < std::size(kBuiltinPresetNames); ++i) EXPECT(builtins[i].name == kBuiltinPresetNames[i]);
    // the Chinese names of the first versions give the current ones; other names stay as they are
    for (size_t i = 0; i < std::size(kBuiltinPresetNames); ++i) EXPECT(CanonicalBuiltinPresetName(kLegacyBuiltinPresetNames[i]) == kBuiltinPresetNames[i] && CanonicalBuiltinPresetName(kBuiltinPresetNames[i]) == kBuiltinPresetNames[i]);
    EXPECT(CanonicalBuiltinPresetName(L"My preset") == L"My preset");

    // ---- Name validation ----
    EXPECT(IsValidPresetName(L"我的備份") && !IsValidPresetName(L"") && !IsValidPresetName(L"a]b") && !IsValidPresetName(L" x") &&
           !IsValidPresetName(std::wstring(41, L'x')));

    // ---- Round trip ----
    CompressPreset a;
    a.name = L"我的 7z";
    a.type = ArchiveType::SevenZip;
    a.profile = Profile::Smallest;
    a.level = 7;
    a.method = L"PPMd";
    a.dictSize = 64ull << 20;
    a.wordSize = 8;
    a.solidSize = kSolidAll;
    a.encryptFileNames = true;
    a.volumeSize = 100ull << 20;
    a.threads = 4;
    a.pathMode = PathMode::Full;
    a.customParams = L"mx=9 fb=64";
    CompressPreset b;
    b.name = L"zip 加密";
    b.type = ArchiveType::Zip;
    b.zipEncryption = L"AES256";
    auto file = dir / "sub" / "presets.ini";
    EXPECT(SavePresets(file, {a, b, builtins[0]}));  // built-in ones are not written
    auto loaded = LoadPresets(file);
    EXPECT(loaded.size() == 2);
    if (loaded.size() == 2) {
        EXPECT(loaded[0].name == a.name && loaded[0].SameSettings(a));
        EXPECT(loaded[1].name == b.name && loaded[1].SameSettings(b));
    }
    EXPECT(!fs::exists(dir / "sub" / "presets.ini.tmp"));

    // listed in the context menu (in_menu): on by default, saved and loaded, not a compression setting
    EXPECT(a.inMenu && loaded.size() == 2 && loaded[0].inMenu && loaded[1].inMenu);
    {
        CompressPreset hidden = b;
        hidden.inMenu = false;
        EXPECT(hidden.SameSettings(b));
        EXPECT(SavePresets(dir / "sub" / "hidden.ini", {a, hidden}));
        auto h = LoadPresets(dir / "sub" / "hidden.ini");
        EXPECT(h.size() == 2 && h[0].inMenu && !h[1].inMenu);
    }

    // ---- Damaged / out of range / duplicate / same name as a built-in / not compatible with the format ----
    {
        std::ofstream f(dir / "bad.ini", std::ios::binary);
        f << "\xEF\xBB\xBF# c\r\nstray=1\r\n[General 7z]\r\nformat=zip\r\n[ok]\r\nformat=zip\r\nlevel=99\r\nmethod=Evil\r\nsolid=123\r\n"
             "volume=abc\r\nzip_encryption=DES\r\nencrypt_names=true\r\n[ok]\r\nformat=7z\r\n[tar]\r\nformat=tar.gz\r\nvolume=1000\r\n"
             "zip_encryption=AES256\r\n[bad]name]\r\n[7z only]\r\nformat=7z\r\nmethod=lzma2\r\nsolid=off\r\n";
    }
    auto bad = LoadPresets(dir / "bad.ini");
    EXPECT(bad.size() == 3);  // ok (the first one), tar, 7z only; the duplicate [ok], the built-in name "General 7z" and the name with ] are all skipped
    if (bad.size() == 3) {
        EXPECT(bad[0].name == L"ok" && bad[0].level == -1 && bad[0].method.empty() && bad[0].solidSize == kSolidAuto &&
               bad[0].volumeSize == 0 && bad[0].zipEncryption.empty() && !bad[0].encryptFileNames);
        EXPECT(bad[1].name == L"tar" && bad[1].volumeSize == 0 && bad[1].zipEncryption.empty());
        EXPECT(bad[2].method == L"LZMA2" && bad[2].solidSize == kSolidOff);
    }
    EXPECT(LoadPresets(dir / "nope.ini").empty());

    // ---- Applying ----
    {
        CompressOptions o;
        o.password = L"keep";
        ApplyPreset(a, o);
        EXPECT(o.type == ArchiveType::SevenZip && o.method == L"PPMd" && o.solidSize == kSolidAll && o.pathMode == PathMode::Full &&
               o.customParams == L"mx=9 fb=64" && o.password == L"keep");
    }

    // ---- Advanced compression options: really compress and open again ----
    Write(dir / "src" / "a.txt", std::string(20000, 'a'));
    Write(dir / "src" / "sub" / "b.txt", "bbb");
    struct Case { const char* label; CompressOptions o; const wchar_t* file; };
    std::vector<Case> cases;
    auto mk = [&](const char* label, const wchar_t* file, auto&& f) { CompressOptions o; f(o); cases.push_back({label, o, file}); };
    mk("7z LZMA2 + 字典 / 字組 / 非結實", L"c1.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.method = L"LZMA2"; o.dictSize = 4u << 20; o.wordSize = 64; o.solidSize = kSolidOff; });
    mk("7z PPMd", L"c2.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.method = L"PPMd"; o.dictSize = 16u << 20; o.wordSize = 6; });
    mk("7z BZip2 + 結實 1MB + 執行緒", L"c3.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.method = L"BZip2"; o.solidSize = 1 << 20; o.threads = 2; });
    mk("7z Copy", L"c4.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.method = L"Copy"; });
    mk("7z 全部結實", L"c5.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.solidSize = kSolidAll; });
    mk("zip BZip2", L"c6.zip", [](CompressOptions& o) { o.method = L"BZip2"; });
    mk("zip LZMA + 字典", L"c7.zip", [](CompressOptions& o) { o.method = L"LZMA"; o.dictSize = 1u << 20; });
    mk("zip Deflate64 + 字組", L"c8.zip", [](CompressOptions& o) { o.method = L"Deflate64"; o.wordSize = 128; });
    mk("zip Copy（僅儲存）", L"c9.zip", [](CompressOptions& o) { o.level = 0; });
    mk("自訂參數 mx", L"c10.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.customParams = L"-mx=9"; });
    mk("自訂參數 fb", L"c11.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.customParams = L"fb=32"; });
    mk("自訂參數 d", L"c12.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.customParams = L"d=1m"; });
    mk("自訂參數 mx+fb", L"c13.7z", [](CompressOptions& o) { o.type = ArchiveType::SevenZip; o.customParams = L"-mx=9 fb=32"; });
    for (auto& c : cases) {
        std::printf("-- %s\n", c.label);
        try {
            CreateArchive(dir / c.file, {dir / "src"}, c.o);
            auto src = ArchiveSource::Open(dir / c.file);
            EXPECT(src->Entries().size() == 4);  // src, src\a.txt, src\sub, src\sub\b.txt
        } catch (const Error& e) {
            std::fprintf(stderr, "FAIL %s: %s\n", c.label, e.what());
            ++failures;
        }
    }
    {   // Store only: packed size >= original size
        auto src = ArchiveSource::Open(dir / "c9.zip");
        uint64_t packed = 0, size = 0;
        for (auto& e : src->Entries()) if (!e.isDir) { packed += e.packedSize; size += e.size; }
        EXPECT(packed >= size);
    }
    {   // ZipCrypto given explicitly for zip
        CompressOptions o;
        o.password = L"pw";
        o.zipEncryption = L"ZipCrypto";
        auto rep = CreateArchive(dir / "zc.zip", {dir / "src"}, o);
        EXPECT(!rep.warnings.empty());
        EXPECT(ArchiveSource::Open(dir / "zc.zip")->CheckPassword(L"pw"));
    }
    {   // A custom parameter that is not recognised: fails and leaves no file
        CompressOptions o;
        o.type = ArchiveType::SevenZip;
        o.customParams = L"bogus_name=1";
        try { CreateArchive(dir / "bad1.7z", {dir / "src"}, o); EXPECT(false); } catch (const Error&) {}
        o.customParams = L"noequals";
        try { CreateArchive(dir / "bad2.7z", {dir / "src"}, o); EXPECT(false); } catch (const Error&) {}
        EXPECT(!fs::exists(dir / "bad1.7z") && !fs::exists(dir / "bad2.7z") && !fs::exists(dir / "bad1.7z.lycantmp"));
    }
    {   // Path mode: full path (without the drive letter)
        CompressOptions o;
        o.pathMode = PathMode::Full;
        CreateArchive(dir / "full.zip", {dir / "src"}, o);
        EXPECT(HasEntry(dir / "full.zip", L"lycan_presets_") && HasEntry(dir / "full.zip", L"src/a.txt"));
        EXPECT(!HasEntry(dir / "full.zip", L":"));
    }

    // ---- List of methods ----
    EXPECT(MethodsFor(ArchiveType::Zip).front() == L"Deflate" && MethodsFor(ArchiveType::SevenZip).front() == L"LZMA2" &&
           MethodsFor(ArchiveType::TarGz).empty());
    EXPECT(MethodHasDict(L"lzma2") && !MethodHasDict(L"BZip2") && MethodHasWordSize(L"Deflate") && !MethodHasWordSize(L"Copy"));

    std::error_code ec;
    fs::remove_all(dir, ec);
    if (failures == 0) std::puts("test_presets: all passed");
    return failures ? 1 : 0;
}
