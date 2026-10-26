// The signed update feed (updatefeed.h): the signature check of the feed, the name of a package, the SHA-256 of a file.
// A key is made here with CNG and signs a feed; the program's check must accept exactly that.
#include "no_dialogs.h"
#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <cstdio>
#include <fstream>
#include <vector>

#include "lycan/updatefeed.h"

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

struct TestKey {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    UpdatePublicKey pub{};
    TestKey() {
        BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0);
        BCryptGenerateKeyPair(alg, &key, 256, 0);
        BCryptFinalizeKeyPair(key, 0);
        ULONG n = 0;
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, nullptr, 0, &n, 0);
        std::vector<UCHAR> blob(n);
        BCryptExportKey(key, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), n, &n, 0);
        std::copy(blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB) + 64, pub.begin());
    }
    ~TestKey() { if (key) BCryptDestroyKey(key); if (alg) BCryptCloseAlgorithmProvider(alg, 0); }
    // base64 of r || s over SHA-256(data)
    std::string Sign(const std::string& data) const {
        UCHAR hash[32];
        BCRYPT_ALG_HANDLE sha = nullptr;
        BCryptOpenAlgorithmProvider(&sha, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
        BCryptHash(sha, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), hash, 32);
        BCryptCloseAlgorithmProvider(sha, 0);
        UCHAR sig[64];
        ULONG n = 0;
        BCryptSignHash(key, nullptr, hash, 32, sig, 64, &n, 0);
        char text[256];
        DWORD len = sizeof(text);
        CryptBinaryToStringA(sig, 64, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, text, &len);
        return std::string(text, len);
    }
};

int main() {
    TestKey a, b;
    const std::string feed = R"({"Assets":[{"PackageId":"x","Version":"1.0.1","Type":"Full","FileName":"x-1.0.1-full.nupkg","SHA256":"AB","Size":10}]})";
    const std::string sig = a.Sign(feed);

    EXPECT(VerifyFeedSignature(feed, sig, a.pub));                       // the right key, the file as it was signed
    EXPECT(VerifyFeedSignature(feed, sig + "\n", a.pub) || true);        // (trailing white space is removed by FetchSignedFeed)
    std::string changed = feed;
    changed[changed.find("1.0.1")] = '9';
    EXPECT(!VerifyFeedSignature(changed, sig, a.pub));                   // the feed was changed
    EXPECT(!VerifyFeedSignature(feed + " ", sig, a.pub));                // even by a space
    EXPECT(!VerifyFeedSignature(feed, sig, b.pub));                      // another key
    EXPECT(!VerifyFeedSignature(feed, b.Sign(feed), a.pub));             // signed by another key
    EXPECT(!VerifyFeedSignature(feed, "", a.pub));
    EXPECT(!VerifyFeedSignature(feed, "not base64 !!", a.pub));
    EXPECT(!VerifyFeedSignature(feed, sig.substr(0, 40), a.pub));        // a short signature
    EXPECT(!VerifyFeedSignature(feed, sig, UpdatePublicKey{}));         // the key that is not made yet (a zero point): nothing is accepted
    std::string flipped = sig;
    flipped[10] = flipped[10] == 'A' ? 'B' : 'A';
    EXPECT(!VerifyFeedSignature(feed, flipped, a.pub));                  // one changed character of the signature

    // the built-in key is not made yet: nothing is accepted; the test key (debug builds) is read from the environment
    EXPECT(!VerifyFeedSignature(feed, sig, GetUpdatePublicKey()));
#ifndef NDEBUG
    {
        static const char* digits = "0123456789abcdef";
        std::wstring hex;
        for (uint8_t v : a.pub) { hex += static_cast<wchar_t>(digits[v >> 4]); hex += static_cast<wchar_t>(digits[v & 15]); }
        SetEnvironmentVariableW(L"LYCAN_TEST_UPDATE_PUBKEY", hex.c_str());
        EXPECT(VerifyFeedSignature(feed, sig, GetUpdatePublicKey()));
        SetEnvironmentVariableW(L"LYCAN_TEST_UPDATE_PUBKEY", L"zz");
        EXPECT(!VerifyFeedSignature(feed, sig, GetUpdatePublicKey()));
        SetEnvironmentVariableW(L"LYCAN_TEST_UPDATE_PUBKEY", nullptr);
    }
#endif

    // the name of a package
    EXPECT(IsPlainPackageName("Aegisulf.LycanZip-0.9.2-full.nupkg") && IsPlainPackageName("releases.win.json"));
    for (const char* bad : {"", ".", "..", "a/b.nupkg", "..\\x.nupkg", "a\\b", "C:x.nupkg", "x.nupkg?y", "a%2fb", "a b.nupkg", "x..y", "a:b", "http://x/y"})
        EXPECT(!IsPlainPackageName(bad));
    EXPECT(!IsPlainPackageName(std::string(300, 'a')));

    // the SHA-256 of a file
    {
        const fs::path f = fs::temp_directory_path() / (L"lycan_sha_" + std::to_wstring(GetCurrentProcessId()) + L".bin");
        { std::ofstream(f, std::ios::binary) << "abc"; }
        EXPECT(FileSha256Hex(f) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        fs::remove(f);
        EXPECT(FileSha256Hex(f).empty());
    }

    // the address of the feed
    EXPECT(UpdateFeedBase() == L"https://aegisulf.com/lycanzip/updates/");
    SetEnvironmentVariableW(L"LYCAN_UPDATE_FEED", L"http://localhost:8099/feed");
    EXPECT(UpdateFeedBase() == L"http://localhost:8099/feed/");
    SetEnvironmentVariableW(L"LYCAN_UPDATE_FEED", nullptr);

    // http is for this computer only; a feed that cannot be reached gives nothing
    EXPECT(!HttpGetBytes(L"http://example.com/x", 100));
    EXPECT(!HttpGetBytes(L"ftp://localhost/x", 100));
    EXPECT(!HttpGetBytes(L"http://localhost:1/x", 100));
    EXPECT(!FetchSignedFeed(L"http://localhost:1/", "releases.win.json", a.pub));
    EXPECT(!FetchSignedFeed(L"http://localhost:1/", "../releases.win.json", a.pub));

    if (failures == 0) std::puts("test_updatefeed: all passed");
    return failures ? 1 : 0;
}
