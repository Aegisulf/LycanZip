// This file defines INITGUID: the IID_* constants of the 7-Zip interfaces are produced in this translation unit only.
#define INITGUID
#include "sevenzip.h"

#include <bcrypt.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

#include "lycan/error.h"

namespace lycan {
namespace {

std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

#ifdef LYCAN_PINNED_7Z_SHA256
// SHA-256 of a file as lower case hex (empty when the file cannot be read)
std::string FileSha256(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
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
#endif

// Builds that ship 7z.dll (LYCAN_PINNED_7Z_SHA256 = the hash of the one they were built with) load only that file: from the folder of
// the exe, never from another 7-Zip on the computer, and only when the hash matches. LYCAN_ALLOW_OTHER_7Z=1 turns the pin off (development).
std::filesystem::path FindDll() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    wchar_t env[MAX_PATH * 4];
    if (DWORD n = GetEnvironmentVariableW(L"LYCAN_7Z_DLL", env, static_cast<DWORD>(std::size(env)));
        n > 0 && n < std::size(env)) {
        candidates.emplace_back(env);
    }
    candidates.push_back(ExeDir() / L"7z.dll");
    bool pinned = false;
#ifdef LYCAN_PINNED_7Z_SHA256
    wchar_t other[8];
    const DWORD otherLen = GetEnvironmentVariableW(L"LYCAN_ALLOW_OTHER_7Z", other, static_cast<DWORD>(std::size(other)));
    pinned = !(otherLen == 1 && other[0] == L'1');
#endif
    if (!pinned) {
        if (const wchar_t* pf = _wgetenv(L"ProgramFiles")) candidates.push_back(fs::path(pf) / L"7-Zip" / L"7z.dll");
        if (const wchar_t* up = _wgetenv(L"USERPROFILE"))
            candidates.push_back(fs::path(up) / L"scoop" / L"apps" / L"7zip" / L"current" / L"7z.dll");
    }
    for (auto& c : candidates) {
        std::error_code ec;
        if (!fs::exists(c, ec)) continue;
#ifdef LYCAN_PINNED_7Z_SHA256
        if (pinned && FileSha256(c) != LYCAN_PINNED_7Z_SHA256)
            throw Error(HRESULT_FROM_WIN32(ERROR_INVALID_DLL), Message::Make(L"core.dll_not_pinned", c.wstring()));
#endif
        return c;
    }
    throw Error(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND), Message::Make(pinned ? L"core.dll_missing_pinned" : L"core.dll_not_found"));
}

std::wstring VariantString(const PROPVARIANT& v) {
    if (v.vt == VT_BSTR && v.bstrVal) return std::wstring(v.bstrVal, SysStringLen(v.bstrVal));
    return {};
}

}  // namespace

SevenZipLib& SevenZipLib::Get() {
    static SevenZipLib instance;
    return instance;
}

SevenZipLib::SevenZipLib() {
    dllPath_ = FindDll();
    module_ = LoadLibraryExW(dllPath_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module_) throw Error(HRESULT_FROM_WIN32(GetLastError()), Message::Make(L"core.dll_load_failed", dllPath_.wstring()));

    createObject_ = reinterpret_cast<CreateObjectFn>(GetProcAddress(module_, "CreateObject"));
    auto getNum = reinterpret_cast<GetNumberOfFormatsFn>(GetProcAddress(module_, "GetNumberOfFormats"));
    auto getProp = reinterpret_cast<GetHandlerProperty2Fn>(GetProcAddress(module_, "GetHandlerProperty2"));
    if (!createObject_ || !getNum || !getProp) throw Error(E_FAIL, Message::Make(L"core.dll_missing_exports"));

    UINT32 count = 0;
    if (getNum(&count) != S_OK) throw Error(E_FAIL, Message::Make(L"core.dll_formats_failed"));
    for (UINT32 i = 0; i < count; ++i) {
        FormatInfo f;
        PROPVARIANT v;
        PropVariantInit(&v);
        if (getProp(i, NArchive::NHandlerPropID::kName, &v) == S_OK) f.name = VariantString(v);
        PropVariantClear(&v);

        if (getProp(i, NArchive::NHandlerPropID::kClassID, &v) == S_OK && v.vt == VT_BSTR &&
            SysStringByteLen(v.bstrVal) == sizeof(GUID)) {
            memcpy(&f.clsid, v.bstrVal, sizeof(GUID));
        }
        PropVariantClear(&v);

        if (getProp(i, NArchive::NHandlerPropID::kUpdate, &v) == S_OK && v.vt == VT_BOOL)
            f.canUpdate = v.boolVal != VARIANT_FALSE;
        PropVariantClear(&v);

        if (getProp(i, NArchive::NHandlerPropID::kExtension, &v) == S_OK) {
            std::wstring all = VariantString(v), cur;
            for (wchar_t c : all + L' ') {
                if (c == L' ') {
                    if (!cur.empty()) f.extensions.push_back(cur);
                    cur.clear();
                } else if (c != L'*') {
                    cur.push_back(static_cast<wchar_t>(std::towlower(c)));
                }
            }
        }
        PropVariantClear(&v);
        formats_.push_back(std::move(f));
    }
}

CMyComPtr<IInArchive> SevenZipLib::CreateInArchive(const GUID& clsid) const {
    CMyComPtr<IInArchive> arc;
    if (createObject_(&clsid, &IID_IInArchive, reinterpret_cast<void**>(&arc)) != S_OK) return nullptr;
    return arc;
}

CMyComPtr<IOutArchive> SevenZipLib::CreateOutArchive(const GUID& clsid) const {
    CMyComPtr<IOutArchive> arc;
    if (createObject_(&clsid, &IID_IOutArchive, reinterpret_cast<void**>(&arc)) != S_OK) return nullptr;
    return arc;
}

const FormatInfo* SevenZipLib::FindFormat(const std::wstring& lowerName) const {
    for (auto& f : formats_) {
        std::wstring n = f.name;
        std::transform(n.begin(), n.end(), n.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (n == lowerName) return &f;
    }
    return nullptr;
}

}  // namespace lycan
