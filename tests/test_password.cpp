// Password verification and conflict policy (Core API level)
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/error.h"
#include "lycan/extract.h"

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
static std::string Read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static PasswordFn Pw(std::wstring s) { return [s](std::wstring& o) { o = s; return true; }; }

static int Run();
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

static int Run() {
    fs::path root = fs::temp_directory_path() / (L"lycan_pw_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(root);
    Write(root / "src" / "a.txt", std::string(5000, 'a'));
    Write(root / "src" / "b.txt", "bbb");

    // ---- Password ----
    struct Case { const char* label; ArchiveType type; bool encNames; const wchar_t* file; };
    for (auto c : {Case{"zip AES-256", ArchiveType::Zip, false, L"aes.zip"}, Case{"7z", ArchiveType::SevenZip, false, L"p.7z"},
                   Case{"7z + 加密檔名", ArchiveType::SevenZip, true, L"hdr.7z"}}) {
        CompressOptions co;
        co.type = c.type;
        // zip supports ASCII passwords only; 7z uses a non-ASCII password to verify the UTF-16 handling
        const std::wstring pw = c.type == ArchiveType::Zip ? L"S3cret!" : L"S3cret密碼";
        co.password = pw;
        co.encryptFileNames = c.encNames;
        CreateArchive(root / c.file, {root / "src"}, co);
        std::printf("-- %s\n", c.label);

        if (c.encNames) {
            // Encrypted headers: no password -> cancelled (password needed); wrong -> wrongPassword; right -> success
            try { ArchiveSource::Open(root / c.file); EXPECT(false); }
            catch (const Error& e) { EXPECT(e.cancelled() && !e.wrongPassword()); }
            try { OpenOptions o; o.password = Pw(L"wrong"); ArchiveSource::Open(root / c.file, o); EXPECT(false); }
            catch (const Error& e) { EXPECT(e.wrongPassword()); }
            OpenOptions o;
            o.password = Pw(pw);
            auto src = ArchiveSource::Open(root / c.file, o);
            EXPECT(src->CheckPassword(pw));
        } else {
            auto src = ArchiveSource::Open(root / c.file);
            EXPECT(src->Caps().encrypted);
            EXPECT(src->CheckPassword(pw));
            EXPECT(!src->CheckPassword(L"wrong"));
            EXPECT(!src->CheckPassword(L""));
            EXPECT(src->CheckPassword(pw));  // the verification does not disturb the state

            // Wrong password at extraction: the report marks wrongPassword, and no broken file is left
            ExtractOptions eo;
            eo.password = Pw(L"wrong");
            auto rep = ExtractTo(*src, root / ("bad_" + fs::path(c.file).string()), eo);
            if (rep.ok() || !rep.wrongPassword) {
                std::fprintf(stderr, "  ok=%d wrongPassword=%d files=%llu errors=%zu\n", rep.ok(), rep.wrongPassword,
                             static_cast<unsigned long long>(rep.files), rep.errors.size());
                for (auto& s : rep.errors) std::fprintf(stderr, "  err: %s\n", ToUtf8(s).c_str());
            }
            EXPECT(!rep.ok() && rep.wrongPassword);
            EXPECT(!fs::exists(root / ("bad_" + fs::path(c.file).string()) / "src" / "a.txt"));
        }
    }

    // zip + non-ASCII password: refused beforehand with a clear message, no leftover file
    {
        CompressOptions bad;
        bad.type = ArchiveType::Zip;
        bad.password = L"密碼";
        try { CreateArchive(root / "nonascii.zip", {root / "src"}, bad); EXPECT(false); }
        catch (const Error& e) { EXPECT(std::string(e.what()).find("7z") != std::string::npos); }
        EXPECT(!fs::exists(root / "nonascii.zip") && !fs::exists(root / "nonascii.zip.lycantmp"));
    }

    // ZipCrypto has one check byte (1 wrong password in 256 gets past it); the verification unpacks a whole file with data and checks
    // its CRC, so none may pass. The saved passwords are tried one after another, so this matters. (An empty file has no data to
    // check; 7-Zip does not encrypt empty files, the check skips them anyway.)
    {
        Write(root / "zc" / "empty.txt", "");
        Write(root / "zc" / "small.txt", "tiny content");
        CompressOptions co;
        co.type = ArchiveType::Zip;
        co.password = L"right-one";
        co.zipEncryption = L"ZipCrypto";
        CreateArchive(root / "zc.zip", {root / "zc"}, co);
        auto src = ArchiveSource::Open(root / "zc.zip");
        EXPECT(src->CheckPassword(L"right-one"));
        int passed = 0;
        for (int i = 0; i < 2000; ++i)
            if (src->CheckPassword(L"wrong-" + std::to_wstring(i))) ++passed;
        if (passed) std::fprintf(stderr, "  %d wrong ZipCrypto passwords passed\n", passed);
        EXPECT(passed == 0);
        EXPECT(src->CheckPassword(L"right-one"));
    }

    // No encrypted file: CheckPassword is always true
    CompressOptions plain;
    CreateArchive(root / "plain.zip", {root / "src"}, plain);
    EXPECT(ArchiveSource::Open(root / "plain.zip")->CheckPassword(L"anything"));

    // ---- Conflict policy ----
    auto src = ArchiveSource::Open(root / "plain.zip");
    auto Fresh = [&](const char* name) {
        fs::path d = root / name;
        Write(d / "src" / "a.txt", "OLD");  // existing file
        return d;
    };

    {   // Fail (default): report an error, the existing file is not touched
        auto d = Fresh("c_fail");
        auto rep = ExtractTo(*src, d, {});
        EXPECT(!rep.ok() && Read(d / "src" / "a.txt") == "OLD" && Read(d / "src" / "b.txt") == "bbb");
    }
    {   // Skip
        auto d = Fresh("c_skip");
        ExtractOptions eo; eo.overwrite = OverwriteMode::Skip;
        auto rep = ExtractTo(*src, d, eo);
        EXPECT(rep.ok() && Read(d / "src" / "a.txt") == "OLD" && rep.skipped.size() == 1);
    }
    {   // Overwrite
        auto d = Fresh("c_over");
        ExtractOptions eo; eo.overwrite = OverwriteMode::Overwrite;
        auto rep = ExtractTo(*src, d, eo);
        EXPECT(rep.ok() && Read(d / "src" / "a.txt") == std::string(5000, 'a'));
    }
    {   // Rename: two times in a row both give a new name
        auto d = Fresh("c_ren");
        ExtractOptions eo; eo.overwrite = OverwriteMode::Rename;
        auto rep1 = ExtractTo(*src, d, eo);
        auto rep2 = ExtractTo(*src, d, eo);
        EXPECT(rep1.ok() && rep2.ok() && rep1.renamed.size() == 1 && rep2.renamed.size() == 2);
        EXPECT(Read(d / "src" / "a.txt") == "OLD");
        EXPECT(Read(d / "src" / "a (2).txt") == std::string(5000, 'a'));
        EXPECT(Read(d / "src" / "a (3).txt") == std::string(5000, 'a'));
        EXPECT(Read(d / "src" / "b (2).txt") == "bbb");
    }
    {   // Ask: ask file by file, "apply to all", cancel
        auto d = Fresh("c_ask");
        Write(d / "src" / "b.txt", "OLDB");
        int asked = 0;
        ExtractOptions eo;
        eo.overwrite = OverwriteMode::Ask;
        eo.onConflict = [&](const ConflictInfo& ci) {
            ++asked;
            EXPECT(ci.existingSize > 0 && ci.newSize > 0);
            return ConflictAnswer{ConflictAction::Overwrite, false};
        };
        auto rep = ExtractTo(*src, d, eo);
        EXPECT(asked == 2 && rep.ok() && Read(d / "src" / "a.txt") == std::string(5000, 'a'));

        auto d2 = Fresh("c_ask_all");
        Write(d2 / "src" / "b.txt", "OLDB");
        asked = 0;
        eo.onConflict = [&](const ConflictInfo&) { ++asked; return ConflictAnswer{ConflictAction::Skip, true}; };
        rep = ExtractTo(*src, d2, eo);
        EXPECT(asked == 1 && rep.skipped.size() == 2 && Read(d2 / "src" / "b.txt") == "OLDB");  // asked only once

        auto d3 = Fresh("c_ask_cancel");
        eo.onConflict = [&](const ConflictInfo&) { return ConflictAnswer{ConflictAction::Cancel, false}; };
        try { ExtractTo(*src, d3, eo); EXPECT(false); } catch (const Error& e) { EXPECT(e.cancelled()); }
        EXPECT(Read(d3 / "src" / "a.txt") == "OLD");

        // Ask but no callback -> same as Fail
        auto d4 = Fresh("c_ask_nocb");
        ExtractOptions e2; e2.overwrite = OverwriteMode::Ask;
        EXPECT(!ExtractTo(*src, d4, e2).ok());
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::puts("test_password: all passed");
    return failures ? 1 : 0;
}
