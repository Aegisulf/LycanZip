#include "lycan/updatefeed.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <vector>

#include "lycan/version.h"

namespace lycan {
namespace {

// The key of the release computer (the design notes "Installer and automatic update"). All zero until the key is made: a zero point is not on the
// curve, so no signature is accepted and no update is installed.
constexpr UpdatePublicKey kPublicKey = {{ 0xB2, 0xEA, 0x85, 0xEC, 0x1F, 0x47, 0x8E, 0xFB, 0x50, 0x35, 0x23, 0xB6, 0xF8, 0x5C, 0x3B, 0x11, 0x7C, 0xA3, 0x44, 0x93, 0xB6, 0xF9, 0xF0, 0xE1, 0xEA, 0xA1, 0x6E, 0xBD, 0x8C, 0x30, 0x21, 0x97, 0xCC, 0x0A, 0xD9, 0x96, 0xAD, 0x31, 0x4B, 0x1E, 0x2B, 0xAA, 0x72, 0x23, 0x2C, 0x44, 0x3A, 0xC6, 0x0A, 0x1A, 0xA6, 0x75, 0x3E, 0x16, 0x66, 0x53, 0x8C, 0x50, 0x50, 0xAA, 0x65, 0x6E, 0xDB, 0xA8 }};

bool FromHex(const std::wstring& hex, UpdatePublicKey& out) {
    if (hex.size() != out.size() * 2) return false;
    for (size_t i = 0; i < out.size(); ++i) {
        unsigned v = 0;
        for (int k = 0; k < 2; ++k) {
            const wchar_t c = hex[i * 2 + static_cast<size_t>(k)];
            unsigned d;
            if (c >= L'0' && c <= L'9') d = static_cast<unsigned>(c - L'0');
            else if (c >= L'a' && c <= L'f') d = static_cast<unsigned>(c - L'a' + 10);
            else if (c >= L'A' && c <= L'F') d = static_cast<unsigned>(c - L'A' + 10);
            else return false;
            v = v * 16 + d;
        }
        out[i] = static_cast<uint8_t>(v);
    }
    return true;
}

struct HttpHandle {
    HINTERNET h = nullptr;
    explicit HttpHandle(HINTERNET x = nullptr) : h(x) {}
    ~HttpHandle() { if (h) WinHttpCloseHandle(h); }
    HttpHandle(const HttpHandle&) = delete;
    HttpHandle& operator=(const HttpHandle&) = delete;
};

bool IsLocalHost(const std::wstring& host) { return host == L"localhost" || host == L"127.0.0.1" || host == L"[::1]"; }

// One GET; `sink` gets the pieces of the body (false = stop). total = Content-Length (0 when not given).
bool Get(const std::wstring& url, uint64_t maxBytes, const std::function<bool(const char*, size_t)>& sink,
         const std::function<void(uint64_t)>& onTotal) {
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256], path[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https && !(uc.nScheme == INTERNET_SCHEME_HTTP && IsLocalHost(host))) return false;   // plain http is for tests on this computer only
    const std::wstring ua = std::wstring(L"LycanZip/") + LYCAN_VERSION_WSTRING;
    HttpHandle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return false;
    WinHttpSetTimeouts(session.h, 8000, 8000, 15000, 30000);
    HttpHandle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn.h) return false;
    std::wstring full = path;
    if (uc.lpszExtraInfo && uc.dwExtraInfoLength) full.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    HttpHandle req(WinHttpOpenRequest(conn.h, L"GET", full.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) return false;
    DWORD opt = WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) return false;
    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX) || status != 200) return false;
    uint64_t total = 0;
    wchar_t lenText[32];
    DWORD lsz = sizeof(lenText);
    if (WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, lenText, &lsz, WINHTTP_NO_HEADER_INDEX)) total = _wcstoui64(lenText, nullptr, 10);
    if (total > maxBytes) return false;
    if (onTotal) onTotal(total);
    uint64_t got = 0;
    std::vector<char> buf(64 * 1024);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) return false;
        if (avail == 0) break;
        avail = std::min<DWORD>(avail, static_cast<DWORD>(buf.size()));
        DWORD n = 0;
        if (!WinHttpReadData(req.h, buf.data(), avail, &n)) return false;
        got += n;
        if (got > maxBytes) return false;
        if (!sink(buf.data(), n)) return false;
    }
    return total == 0 || got == total;
}

bool Base64Decode(const std::string& text, std::vector<uint8_t>& out) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr) || n == 0) return false;
    out.resize(n);
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64, out.data(), &n, nullptr, nullptr)) return false;
    out.resize(n);
    return true;
}

// SHA-256 of a buffer (CNG)
bool Sha256(const void* data, size_t size, uint8_t out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptHash(alg, nullptr, 0, static_cast<PUCHAR>(const_cast<void*>(data)), static_cast<ULONG>(size), out, 32) == 0;
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

}  // namespace

UpdatePublicKey GetUpdatePublicKey() {
#ifndef NDEBUG
    wchar_t buf[256];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_TEST_UPDATE_PUBKEY", buf, static_cast<DWORD>(std::size(buf)));
    UpdatePublicKey k{};
    if (n > 0 && n < std::size(buf) && FromHex(std::wstring(buf, n), k)) return k;
#endif
    return kPublicKey;
}

bool VerifyFeedSignature(const std::string& feed, const std::string& signatureBase64, const UpdatePublicKey& key) {
    std::vector<uint8_t> sig;
    if (!Base64Decode(signatureBase64, sig) || sig.size() != 64) return false;
    uint8_t hash[32];
    if (!Sha256(feed.data(), feed.size(), hash)) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) == 0) {
        std::vector<uint8_t> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
        auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
        header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        header->cbKey = 32;
        std::copy(key.begin(), key.end(), blob.begin() + static_cast<std::ptrdiff_t>(sizeof(BCRYPT_ECCKEY_BLOB)));
        if (BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &hKey, blob.data(), static_cast<ULONG>(blob.size()), 0) == 0)   // fails when the point is not on the curve
            ok = BCryptVerifySignature(hKey, nullptr, hash, 32, sig.data(), 64, 0) == 0;
    }
    if (hKey) BCryptDestroyKey(hKey);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string FileSha256Hex(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::string hex;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 && BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<char> buf(1 << 20);
        bool ok = true;
        while (in) {
            in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            const auto got = in.gcount();
            if (got > 0 && BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(got), 0) != 0) { ok = false; break; }
        }
        UCHAR digest[32];
        if (ok && in.eof() && BCryptFinishHash(h, digest, sizeof(digest), 0) == 0) {
            static const char* digits = "0123456789abcdef";
            for (UCHAR b : digest) { hex += digits[b >> 4]; hex += digits[b & 15]; }
        }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

std::wstring UpdateFeedBase() {
    wchar_t buf[512];
    const DWORD n = GetEnvironmentVariableW(L"LYCAN_UPDATE_FEED", buf, static_cast<DWORD>(std::size(buf)));
    std::wstring base = n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring(L"https://aegisulf.com/lycanzip/updates/");
    if (!base.empty() && base.back() != L'/') base += L'/';
    return base;
}

std::optional<std::string> HttpGetBytes(const std::wstring& url, size_t maxBytes) {
    std::string body;
    if (!Get(url, maxBytes, [&](const char* p, size_t n) { body.append(p, n); return true; }, nullptr)) return std::nullopt;
    return body;
}

bool HttpDownloadFile(const std::wstring& url, const std::filesystem::path& to, uint64_t maxBytes, const std::function<bool(uint64_t, uint64_t)>& progress) {
    std::error_code ec;
    std::filesystem::create_directories(to.parent_path(), ec);
    std::ofstream out(to, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    uint64_t total = 0, got = 0;
    bool ok = Get(url, maxBytes, [&](const char* p, size_t n) {
        out.write(p, static_cast<std::streamsize>(n));
        got += n;
        return out.good() && (!progress || progress(got, total));
    }, [&](uint64_t t) { total = t; });
    out.close();
    if (!ok || !out) { std::filesystem::remove(to, ec); return false; }
    return true;
}

bool IsPlainPackageName(const std::string& name) {
    if (name.empty() || name.size() > 200 || name == "." || name == "..") return false;
    for (unsigned char c : name) {
        if (c < 0x21 || c > 0x7e) return false;                          // no spaces, no control characters, ASCII only
        if (c == '/' || c == '\\' || c == ':' || c == '?' || c == '#' || c == '%' || c == '*' || c == '"' || c == '<' || c == '>' || c == '|') return false;
    }
    return name.find("..") == std::string::npos;
}

std::optional<std::string> FetchSignedFeed(const std::wstring& base, const std::string& name, const UpdatePublicKey& key) {
    if (!IsPlainPackageName(name)) return std::nullopt;
    const std::wstring wname(name.begin(), name.end());
    auto feed = HttpGetBytes(base + wname, 1 << 20);
    auto sig = HttpGetBytes(base + wname + L".sig", 4096);
    if (!feed || !sig) return std::nullopt;
    while (!sig->empty() && std::isspace(static_cast<unsigned char>(sig->back()))) sig->pop_back();
    if (!VerifyFeedSignature(*feed, *sig, key)) return std::nullopt;
    return feed;
}

}  // namespace lycan
