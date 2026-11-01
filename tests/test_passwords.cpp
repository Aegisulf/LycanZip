// Saved passwords: the list format, the DPAPI file, import / export lines, the order of trying
#include <windows.h>

#include <cstdio>
#include <fstream>

#include "lycan/passwords.h"

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

static std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void WriteAll(const fs::path& p, const std::string& s) { std::ofstream(p, std::ios::binary) << s; }

int main() {
    const fs::path dir = fs::temp_directory_path() / (L"lycan_pwstore_" + std::to_wstring(GetCurrentProcessId()));
    fs::create_directories(dir);

    std::vector<SavedPassword> items = {
        {L"S3cret!", L"forum", 100, 300},
        {L"密碼 with spaces ", L"", 200, 0},     // non-ASCII, a space at the end is part of it
        {L"tab\there\nnewline", L"備註", 50, 400},  // anything may be in a password
    };

    // the list round-trips; damaged bytes are refused
    {
        const std::string bytes = SerializePasswords(items);
        std::vector<SavedPassword> back;
        EXPECT(ParsePasswords(bytes, back) && back == items);
        EXPECT(!ParsePasswords(bytes.substr(0, bytes.size() - 3), back) && back.empty());
        EXPECT(!ParsePasswords(bytes + "x", back));
        EXPECT(ParsePasswords(SerializePasswords({}), back) && back.empty());
    }

    // the file: encrypted with DPAPI (the password does not appear in it), read back by the same account
    const fs::path file = dir / "passwords.dat";
    EXPECT(LoadPasswords(file).state == PasswordFileState::Missing);
    EXPECT(SavePasswords(file, items));
    EXPECT(!fs::exists(fs::path(file).concat(L".tmp")));
    {
        const std::string raw = ReadAll(file);
        EXPECT(raw.substr(0, 4) == "LZPW" && raw.find("S3cret!") == std::string::npos && raw.find("forum") == std::string::npos);
        auto f = LoadPasswords(file);
        EXPECT(f.state == PasswordFileState::Ok && f.items == items);
        // damaged / another account: unreadable, nothing comes back
        std::string bad = raw;
        bad[bad.size() / 2] ^= 0x5A;
        WriteAll(dir / "bad.dat", bad);
        auto b = LoadPasswords(dir / "bad.dat");
        EXPECT(b.state == PasswordFileState::Unreadable && b.items.empty());
        WriteAll(dir / "notours.dat", "hello world, not a password file");
        EXPECT(LoadPasswords(dir / "notours.dat").state == PasswordFileState::Unreadable);
        std::string future = raw;
        future[4] = 9;   // a newer format version
        WriteAll(dir / "future.dat", future);
        EXPECT(LoadPasswords(dir / "future.dat").state == PasswordFileState::Unreadable);
    }

    // the master password (few iterations here: the real number is slow on purpose)
    {
        const fs::path mf = dir / "master.dat";
        MasterKey key = MakeMasterKey(L"主密碼1", 1000);
        EXPECT(!key.empty() && key.key.size() == 32 && key.salt.size() == 16 && key.iterations == 1000);
        EXPECT(MakeMasterKey(L"主密碼1", 1000).salt != key.salt);   // a new salt each time
        EXPECT(CheckMasterKey(key, L"主密碼1") && !CheckMasterKey(key, L"主密碼2") && !CheckMasterKey(MasterKey{}, L"主密碼1"));
        EXPECT(SavePasswords(mf, items, &key));
        const std::string raw = ReadAll(mf);
        EXPECT(raw.substr(0, 4) == "LZPW" && raw[8] == 2 && raw.find("S3cret!") == std::string::npos);
        // read back: locked until the master password is given; a wrong one opens nothing
        auto f = LoadPasswords(mf);
        EXPECT(f.state == PasswordFileState::Locked && f.items.empty() && !f.locked.empty());
        EXPECT(f.locked.find("S3cret!") == std::string::npos);   // under the DPAPI layer it is still encrypted
        std::vector<SavedPassword> out;
        MasterKey got;
        EXPECT(!UnlockPasswords(f.locked, L"wrong", out, got) && out.empty() && got.empty());
        EXPECT(UnlockPasswords(f.locked, L"主密碼1", out, got) && out == items);
        EXPECT(got.key == key.key && got.salt == key.salt && got.iterations == 1000);
        // saving again with the key from the unlock: still locked, same master password
        EXPECT(SavePasswords(mf, out, &got));
        auto again = LoadPasswords(mf);
        EXPECT(again.state == PasswordFileState::Locked && UnlockPasswords(again.locked, L"主密碼1", out, got) && out == items);
        // damaged inside the master layer: refused, not a crash
        std::string bad = again.locked;
        bad[bad.size() - 1] ^= 0x01;
        EXPECT(!UnlockPasswords(bad, L"主密碼1", out, got));
        EXPECT(!UnlockPasswords("xx", L"主密碼1", out, got) && !UnlockPasswords(std::string(40, '\0'), L"主密碼1", out, got));
        // turned off: saved without the key, readable without a master password
        EXPECT(SavePasswords(mf, items));
        EXPECT(LoadPasswords(mf).state == PasswordFileState::Ok);
        got.Clear();
        EXPECT(got.empty() && got.salt.empty());
    }

    // import: one per line, UTF-8 with / without BOM, UTF-16, ANSI; empty lines and duplicates dropped; CRLF not part of it
    {
        auto lines = ParsePasswordLines("one\r\ntwo\n\n  three \r\none\n");
        EXPECT(lines == (std::vector<std::wstring>{L"one", L"two", L"  three "}));
        EXPECT(ParsePasswordLines("\xEF\xBB\xBF\xE5\xAF\x86\xE7\xA2\xBC\r\n") == (std::vector<std::wstring>{L"密碼"}));
        const std::string utf16("\xFF\xFE" "a\0b\0\r\0\n\0c\0", 12);
        EXPECT(ParsePasswordLines(utf16) == (std::vector<std::wstring>{L"ab", L"c"}));
        EXPECT(ParsePasswordLines("").empty() && ParsePasswordLines("\r\n\r\n").empty());
        // export: UTF-8 with BOM, CRLF; a password with a line break cannot be a line and is left out
        const std::string text = PasswordLinesText(items);
        EXPECT(text.rfind("\xEF\xBB\xBF", 0) == 0 && text.find("S3cret!\r\n") != std::string::npos && text.find("tab") == std::string::npos);
        EXPECT(ParsePasswordLines(text) == (std::vector<std::wstring>{L"S3cret!", L"密碼 with spaces "}));
    }

    // the order of trying: the most recently used first, then the most recently added
    {
        auto v = items;
        SortForTrying(v);
        EXPECT(v[0].password == L"tab\there\nnewline" && v[1].password == L"S3cret!" && v[2].password.rfind(L"密", 0) == 0);
        EXPECT(!TouchPassword(v, L"S3cret!", 999, true));          // known: only its time moves
        SortForTrying(v);
        EXPECT(v[0].password == L"S3cret!" && v.size() == 3);
        EXPECT(!TouchPassword(v, L"new", 1000, false) && v.size() == 3);   // not added unless asked
        EXPECT(TouchPassword(v, L"new", 1000, true) && v.size() == 4 && v.back().added == 1000);
        EXPECT(FileTimeNow() > 130000000000000000ull);   // after 2012
    }

    fs::remove_all(dir);
    if (failures == 0) std::puts("test_passwords: all passed");
    return failures ? 1 : 0;
}
