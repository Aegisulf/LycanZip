#pragma once
// Saved passwords (the password manager, DESIGN.md "密碼管理員"). The list is kept in passwords.dat next to the settings, encrypted
// with DPAPI for the current Windows account: the file can be read only by this account on this PC (a portable copy moved to another
// PC cannot read it). With a master password (optional) the list is encrypted with it first, and that is put inside the DPAPI layer.
//   file  = "LZPW" | u32 version (1) | u32 scheme | DPAPI(content)
//           scheme 1: content = the list
//           scheme 2: content = u32 iterations | salt (16) | nonce (12) | tag (16) | AES-256-GCM(the list) with the key of
//                     PBKDF2-SHA256(master password as UTF-8, salt, iterations)
//   list  = u32 count, then per password: string password, string note, u64 added, u64 last used (FILETIME; 0 = never)
//   string = u32 byte length + UTF-8
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace lycan {

struct SavedPassword {
    std::wstring password;
    std::wstring note;       // optional: where the password is from
    uint64_t added = 0;      // FILETIME
    uint64_t lastUsed = 0;   // FILETIME, 0 = never
    bool operator==(const SavedPassword&) const = default;
};

// Unreadable: made by another account / PC, or damaged. Locked: under a master password (UnlockPasswords).
enum class PasswordFileState { Ok, Missing, Unreadable, Locked };

struct PasswordFile {
    PasswordFileState state = PasswordFileState::Missing;
    std::vector<SavedPassword> items;
    std::string locked;   // Locked: what UnlockPasswords needs
};

// The key of a master password: kept in memory while the list is unlocked (the master password itself is not kept)
struct MasterKey {
    uint32_t iterations = 0;
    std::string salt;   // 16 bytes
    std::string key;    // 32 bytes
    bool empty() const { return key.empty(); }
    void Clear();
};
constexpr uint32_t kMasterIterations = 600000;   // about a quarter of a second
constexpr size_t kMinMasterLength = 4;

PasswordFile LoadPasswords(const std::filesystem::path& file);
// Written to a temporary file first; with `master` the list is encrypted with it too
bool SavePasswords(const std::filesystem::path& file, const std::vector<SavedPassword>& items, const MasterKey* master = nullptr);

// A new master password: a random salt and the derived key (slow: call it off the UI thread)
MasterKey MakeMasterKey(const std::wstring& master, uint32_t iterations = kMasterIterations);
// Whether `master` is the password of `key` (derives again: slow)
bool CheckMasterKey(const MasterKey& key, const std::wstring& master);
// Opens the list of a Locked file with the master password; false when it is wrong (or the bytes are damaged). Slow.
bool UnlockPasswords(const std::string& locked, const std::wstring& master, std::vector<SavedPassword>& out, MasterKey& key);

// The list without the encryption (tests, and the layer under a future master password)
std::string SerializePasswords(const std::vector<SavedPassword>& items);
bool ParsePasswords(const std::string& bytes, std::vector<SavedPassword>& out);

// Import: a text file with one password per line (UTF-8 with or without BOM, UTF-16 LE with BOM, else the ANSI code page); empty
// lines are skipped, a line break at the end of a line is not part of the password, spaces are. Duplicates are dropped.
std::vector<std::wstring> ParsePasswordLines(const std::string& bytes);
// Export: one password per line, UTF-8 with BOM, CRLF (not encrypted). A password with a line break in it is left out.
std::string PasswordLinesText(const std::vector<SavedPassword>& items);

// The order in which saved passwords are tried: the most recently used first, then the most recently added
void SortForTrying(std::vector<SavedPassword>& items);
// A password was used: moves its time forward, or adds it. Returns true when it was not in the list.
bool TouchPassword(std::vector<SavedPassword>& items, const std::wstring& password, uint64_t now, bool addIfMissing);

uint64_t FileTimeNow();

}  // namespace lycan
