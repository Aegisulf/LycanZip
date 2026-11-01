#include "lycan/passwords.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>

#include <algorithm>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;

namespace lycan {
namespace {

constexpr char kMagic[4] = {'L', 'Z', 'P', 'W'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kSchemeDpapi = 1;
constexpr uint32_t kSchemeMaster = 2;   // the master password inside the DPAPI layer
constexpr uint32_t kMaxItems = 100000;
constexpr size_t kSaltSize = 16, kNonceSize = 12, kTagSize = 16, kKeySize = 32;
constexpr char kMasterAad[] = "LycanZip master password v1";   // authenticated with the list, not encrypted
constexpr uint32_t kMaxString = 1u << 20;
// mixed into the DPAPI encryption: another program of the same account cannot just decrypt the file with an empty entropy
constexpr char kEntropy[] = "LycanZip saved passwords v1";

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromCodePage(const char* p, size_t len, UINT cp, DWORD flags = 0) {
    if (!len) return {};
    const int n = MultiByteToWideChar(cp, flags, p, static_cast<int>(len), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, flags, p, static_cast<int>(len), w.data(), n);
    return w;
}

void PutU32(std::string& out, uint32_t v) { for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF); }
void PutU64(std::string& out, uint64_t v) { for (int i = 0; i < 8; ++i) out += static_cast<char>((v >> (8 * i)) & 0xFF); }
void PutStr(std::string& out, const std::wstring& w) {
    const std::string s = ToUtf8(w);
    PutU32(out, static_cast<uint32_t>(s.size()));
    out += s;
}

struct Reader {
    const std::string& d;
    size_t p = 0;
    bool ok = true;
    uint64_t Num(int bytes) {
        if (!ok || p + bytes > d.size()) { ok = false; return 0; }
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(d[p + i])) << (8 * i);
        p += bytes;
        return v;
    }
    std::wstring Str() {
        const uint32_t n = static_cast<uint32_t>(Num(4));
        if (!ok || n > kMaxString || p + n > d.size()) { ok = false; return {}; }
        std::wstring w = FromCodePage(d.data() + p, n, CP_UTF8);
        p += n;
        return w;
    }
};

bool Dpapi(bool protect, const std::string& in, std::string& out) {
    DATA_BLOB src{static_cast<DWORD>(in.size()), reinterpret_cast<BYTE*>(const_cast<char*>(in.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kEntropy) - 1), reinterpret_cast<BYTE*>(const_cast<char*>(kEntropy))};
    DATA_BLOB result{};
    const BOOL ok = protect ? CryptProtectData(&src, L"LycanZip", &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result)
                            : CryptUnprotectData(&src, nullptr, &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &result);
    if (!ok) return false;
    out.assign(reinterpret_cast<const char*>(result.pbData), result.cbData);
    SecureZeroMemory(result.pbData, result.cbData);
    LocalFree(result.pbData);
    return true;
}

std::string RandomBytes(size_t n) {
    std::string s(n, '\0');
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(s.data()), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
    return s;
}

std::string DeriveKey(const std::wstring& master, const std::string& salt, uint32_t iterations) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) return {};
    std::string pw = ToUtf8(master), key(kKeySize, '\0');
    const NTSTATUS st = BCryptDeriveKeyPBKDF2(alg, reinterpret_cast<PUCHAR>(pw.data()), static_cast<ULONG>(pw.size()),
                                              reinterpret_cast<PUCHAR>(const_cast<char*>(salt.data())), static_cast<ULONG>(salt.size()),
                                              iterations, reinterpret_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0);
    SecureZeroMemory(pw.data(), pw.size());
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(st)) { SecureZeroMemory(key.data(), key.size()); return {}; }
    return key;
}

// AES-256-GCM: encrypt `in` (the tag goes to `tag`) or decrypt it (checks `tag`: false for a wrong key or damaged bytes)
bool AesGcm(bool encrypt, const std::string& key, const std::string& nonce, std::string& tag, const std::string& in, std::string& out) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                                         static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0)) &&
        BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), static_cast<ULONG>(key.size()), 0))) {
        tag.resize(kTagSize);
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
        BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char*>(nonce.data()));
        info.cbNonce = static_cast<ULONG>(nonce.size());
        info.pbAuthData = reinterpret_cast<PUCHAR>(const_cast<char*>(kMasterAad));
        info.cbAuthData = static_cast<ULONG>(sizeof(kMasterAad) - 1);
        info.pbTag = reinterpret_cast<PUCHAR>(tag.data());
        info.cbTag = static_cast<ULONG>(tag.size());
        out.assign(in.size(), '\0');
        ULONG done = 0;
        PUCHAR src = reinterpret_cast<PUCHAR>(const_cast<char*>(in.data())), dst = reinterpret_cast<PUCHAR>(out.data());
        const NTSTATUS st = encrypt ? BCryptEncrypt(k, src, static_cast<ULONG>(in.size()), &info, nullptr, 0, dst, static_cast<ULONG>(out.size()), &done, 0)
                                    : BCryptDecrypt(k, src, static_cast<ULONG>(in.size()), &info, nullptr, 0, dst, static_cast<ULONG>(out.size()), &done, 0);
        ok = BCRYPT_SUCCESS(st) && done == in.size();
        if (!ok) { SecureZeroMemory(out.data(), out.size()); out.clear(); }
    }
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace

void MasterKey::Clear() {
    SecureZeroMemory(key.data(), key.size());
    key.clear();
    salt.clear();
    iterations = 0;
}

MasterKey MakeMasterKey(const std::wstring& master, uint32_t iterations) {
    MasterKey k;
    k.salt = RandomBytes(kSaltSize);
    if (k.salt.empty()) return {};
    k.iterations = iterations;
    k.key = DeriveKey(master, k.salt, iterations);
    if (k.key.empty()) return {};
    return k;
}

bool CheckMasterKey(const MasterKey& key, const std::wstring& master) {
    if (key.empty()) return false;
    std::string again = DeriveKey(master, key.salt, key.iterations);
    const bool same = again.size() == key.key.size() && !again.empty() &&
                      std::equal(again.begin(), again.end(), key.key.begin());
    SecureZeroMemory(again.data(), again.size());
    return same;
}

bool UnlockPasswords(const std::string& locked, const std::wstring& master, std::vector<SavedPassword>& out, MasterKey& key) {
    out.clear();
    Reader r{locked};
    const uint32_t iterations = static_cast<uint32_t>(r.Num(4));
    if (!r.ok || iterations == 0 || iterations > 10000000 || locked.size() < 4 + kSaltSize + kNonceSize + kTagSize) return false;
    const std::string salt = locked.substr(4, kSaltSize), nonce = locked.substr(4 + kSaltSize, kNonceSize);
    std::string tag = locked.substr(4 + kSaltSize + kNonceSize, kTagSize);
    const std::string sealed = locked.substr(4 + kSaltSize + kNonceSize + kTagSize);
    MasterKey k;
    k.iterations = iterations;
    k.salt = salt;
    k.key = DeriveKey(master, salt, iterations);
    if (k.key.empty()) return false;
    std::string plain;
    if (!AesGcm(false, k.key, nonce, tag, sealed, plain)) { k.Clear(); return false; }
    const bool ok = ParsePasswords(plain, out);
    SecureZeroMemory(plain.data(), plain.size());
    if (!ok) { out.clear(); k.Clear(); return false; }
    key = std::move(k);
    return true;
}

uint64_t FileTimeNow() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string SerializePasswords(const std::vector<SavedPassword>& items) {
    std::string out;
    PutU32(out, static_cast<uint32_t>(items.size()));
    for (auto const& it : items) {
        PutStr(out, it.password);
        PutStr(out, it.note);
        PutU64(out, it.added);
        PutU64(out, it.lastUsed);
    }
    return out;
}

bool ParsePasswords(const std::string& bytes, std::vector<SavedPassword>& out) {
    out.clear();
    Reader r{bytes};
    const uint32_t n = static_cast<uint32_t>(r.Num(4));
    if (!r.ok || n > kMaxItems) return false;
    for (uint32_t i = 0; i < n; ++i) {
        SavedPassword s;
        s.password = r.Str();
        s.note = r.Str();
        s.added = r.Num(8);
        s.lastUsed = r.Num(8);
        if (!r.ok) { out.clear(); return false; }
        if (!s.password.empty()) out.push_back(std::move(s));
    }
    return r.p == bytes.size();
}

PasswordFile LoadPasswords(const fs::path& file) {
    PasswordFile f;
    std::error_code ec;
    if (!fs::exists(file, ec)) return f;
    f.state = PasswordFileState::Unreadable;
    std::ifstream in(file, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), {});
    if (data.size() < 12 || std::memcmp(data.data(), kMagic, 4) != 0) return f;
    Reader head{data};
    head.p = 4;
    const uint32_t version = static_cast<uint32_t>(head.Num(4)), scheme = static_cast<uint32_t>(head.Num(4));
    if (version != kVersion || (scheme != kSchemeDpapi && scheme != kSchemeMaster)) return f;
    std::string plain;
    if (!Dpapi(false, data.substr(12), plain)) return f;   // another account / PC, or damaged
    if (scheme == kSchemeMaster) {
        f.state = PasswordFileState::Locked;
        f.locked = std::move(plain);
        return f;
    }
    const bool ok = ParsePasswords(plain, f.items);
    SecureZeroMemory(plain.data(), plain.size());
    if (!ok) { f.items.clear(); return f; }
    f.state = PasswordFileState::Ok;
    return f;
}

bool SavePasswords(const fs::path& file, const std::vector<SavedPassword>& items, const MasterKey* master) {
    const bool withMaster = master && !master->empty();
    std::string plain = SerializePasswords(items), sealed;
    if (withMaster) {
        // the list encrypted with the master key, then that inside the DPAPI layer
        const std::string nonce = RandomBytes(kNonceSize);
        std::string tag, cipher;
        const bool enc = !nonce.empty() && AesGcm(true, master->key, nonce, tag, plain, cipher);
        SecureZeroMemory(plain.data(), plain.size());
        if (!enc) return false;
        plain.clear();
        PutU32(plain, master->iterations);
        plain += master->salt + nonce + tag + cipher;
    }
    const bool ok = Dpapi(true, plain, sealed);
    SecureZeroMemory(plain.data(), plain.size());
    if (!ok) return false;
    std::string data(kMagic, 4);
    PutU32(data, kVersion);
    PutU32(data, withMaster ? kSchemeMaster : kSchemeDpapi);
    data += sealed;
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path tmp = fs::path(file).concat(L".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out.good()) return false;
    }
    fs::rename(tmp, file, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

std::vector<std::wstring> ParsePasswordLines(const std::string& bytes) {
    std::wstring text;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        for (size_t i = 2; i + 1 < bytes.size(); i += 2) text += static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) | (static_cast<unsigned char>(bytes[i + 1]) << 8));
    } else {
        size_t start = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF ? 3 : 0;
        text = FromCodePage(bytes.data() + start, bytes.size() - start, CP_UTF8, MB_ERR_INVALID_CHARS);
        if (text.empty() && bytes.size() > start) text = FromCodePage(bytes.data() + start, bytes.size() - start, CP_ACP);
    }
    std::vector<std::wstring> out;
    size_t a = 0;
    while (a <= text.size()) {
        size_t e = text.find(L'\n', a);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(a, e - a);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty() && std::find(out.begin(), out.end(), line) == out.end()) out.push_back(std::move(line));
        a = e + 1;
    }
    return out;
}

std::string PasswordLinesText(const std::vector<SavedPassword>& items) {
    std::string out = "\xEF\xBB\xBF";
    for (auto const& it : items) {
        if (it.password.find_first_of(L"\r\n") != std::wstring::npos) continue;
        out += ToUtf8(it.password) + "\r\n";
    }
    return out;
}

void SortForTrying(std::vector<SavedPassword>& items) {
    std::stable_sort(items.begin(), items.end(), [](SavedPassword const& a, SavedPassword const& b) {
        if (a.lastUsed != b.lastUsed) return a.lastUsed > b.lastUsed;
        return a.added > b.added;
    });
}

bool TouchPassword(std::vector<SavedPassword>& items, const std::wstring& password, uint64_t now, bool addIfMissing) {
    for (auto& it : items)
        if (it.password == password) { it.lastUsed = now; return false; }
    if (!addIfMissing || password.empty()) return false;
    items.push_back({password, L"", now, now});
    return true;
}

}  // namespace lycan
