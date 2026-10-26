#include "lycan/hash.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <memory>
#include <mutex>

#include "lycan/error.h"
#include "archive_impl.h"

namespace lycan {
namespace {

std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Hex(const unsigned char* d, size_t n) {
    static const char* digits = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = digits[d[i] >> 4]; s[2 * i + 1] = digits[d[i] & 15]; }
    return s;
}

// ---- CRC-32 (the one of zip, 7z, rar, gzip, PNG): slicing by 8 --------------------------------------------------------------

struct CrcTables {
    uint32_t t[8][256];
    CrcTables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = t[0][i];
            for (int k = 1; k < 8; ++k) { c = t[0][c & 0xFF] ^ (c >> 8); t[k][i] = c; }
        }
    }
};

uint32_t CrcUpdate(uint32_t crc, const unsigned char* p, size_t n) {
    static const CrcTables tables;
    const auto& t = tables.t;
    while (n >= 8) {
        uint32_t a, b;
        memcpy(&a, p, 4);
        memcpy(&b, p + 4, 4);
        a ^= crc;
        crc = t[7][a & 0xFF] ^ t[6][(a >> 8) & 0xFF] ^ t[5][(a >> 16) & 0xFF] ^ t[4][a >> 24] ^
              t[3][b & 0xFF] ^ t[2][(b >> 8) & 0xFF] ^ t[1][(b >> 16) & 0xFF] ^ t[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) crc = t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

// ---- CNG providers (one per algorithm, kept for the life of the process) ---------------------------------------------------

struct Cng {
    BCRYPT_ALG_HANDLE alg = nullptr;
    DWORD objectLength = 0, hashLength = 0;
};

const Cng* CngFor(uint32_t algo) {
    static std::once_flag once[6];
    static Cng cng[6];
    static const wchar_t* ids[6] = {nullptr, BCRYPT_MD5_ALGORITHM, BCRYPT_SHA1_ALGORITHM, BCRYPT_SHA256_ALGORITHM, BCRYPT_SHA384_ALGORITHM, BCRYPT_SHA512_ALGORITHM};
    int i = 0;
    while (i < 6 && algo != (1u << i)) ++i;
    if (i == 0 || i >= 6) return nullptr;
    std::call_once(once[i], [&] {
        Cng c;
        if (BCryptOpenAlgorithmProvider(&c.alg, ids[i], nullptr, 0) != 0) { c.alg = nullptr; return; }
        DWORD got = 0;
        if (BCryptGetProperty(c.alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&c.objectLength), sizeof(DWORD), &got, 0) != 0 ||
            BCryptGetProperty(c.alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&c.hashLength), sizeof(DWORD), &got, 0) != 0) {
            BCryptCloseAlgorithmProvider(c.alg, 0);
            c.alg = nullptr;
            return;
        }
        cng[i] = c;
    });
    return cng[i].alg ? &cng[i] : nullptr;
}

}  // namespace

// ---- names and sets ------------------------------------------------------------------------------------------------------------

const wchar_t* HashAlgoName(uint32_t algo) {
    switch (algo) {
        case kHashCrc32: return L"CRC-32";
        case kHashMd5: return L"MD5";
        case kHashSha1: return L"SHA-1";
        case kHashSha256: return L"SHA-256";
        case kHashSha384: return L"SHA-384";
        case kHashSha512: return L"SHA-512";
    }
    return L"";
}

size_t HashHexLength(uint32_t algo) {
    switch (algo) {
        case kHashCrc32: return 8;
        case kHashMd5: return 32;
        case kHashSha1: return 40;
        case kHashSha256: return 64;
        case kHashSha384: return 96;
        case kHashSha512: return 128;
    }
    return 0;
}

std::vector<uint32_t> HashAlgoList(uint32_t mask) {
    std::vector<uint32_t> v;
    for (uint32_t bit = 1; bit <= kHashSha512; bit <<= 1)
        if (mask & bit) v.push_back(bit);
    return v;
}

uint32_t NormalizeHashAlgos(uint32_t mask) {
    mask &= kHashAll;
    return mask ? mask : kHashDefault;
}

// ---- the hasher -----------------------------------------------------------------------------------------------------------------

struct MultiHasher::Impl {
    struct One {
        uint32_t algo = 0;
        BCRYPT_HASH_HANDLE h = nullptr;
        std::vector<unsigned char> object;
        DWORD hashLength = 0;
    };
    std::vector<One> cng;
    bool crc = false;
    uint32_t crcState = 0xFFFFFFFFu;
    std::vector<uint32_t> order;
    bool failed = false;
};

MultiHasher::MultiHasher(uint32_t algos) : impl_(new Impl) {
    algos = NormalizeHashAlgos(algos);
    impl_->order = HashAlgoList(algos);
    for (uint32_t a : impl_->order) {
        if (a == kHashCrc32) { impl_->crc = true; continue; }
        const Cng* c = CngFor(a);
        Impl::One one;
        one.algo = a;
        if (!c) { impl_->failed = true; impl_->cng.push_back(std::move(one)); continue; }
        one.object.resize(c->objectLength);
        one.hashLength = c->hashLength;
        if (BCryptCreateHash(c->alg, &one.h, one.object.data(), c->objectLength, nullptr, 0, 0) != 0) { one.h = nullptr; impl_->failed = true; }
        impl_->cng.push_back(std::move(one));
    }
}

MultiHasher::~MultiHasher() {
    for (auto& o : impl_->cng)
        if (o.h) BCryptDestroyHash(o.h);
    delete impl_;
}

void MultiHasher::Update(const void* data, size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    if (impl_->crc) impl_->crcState = CrcUpdate(impl_->crcState, p, size);
    for (auto& o : impl_->cng) {
        if (!o.h) continue;
        size_t left = size;
        const unsigned char* q = p;
        while (left) {
            const ULONG n = static_cast<ULONG>((std::min<size_t>)(left, 1u << 30));
            BCryptHashData(o.h, const_cast<PUCHAR>(q), n, 0);
            q += n;
            left -= n;
        }
    }
}

std::vector<std::string> MultiHasher::Finish() {
    std::vector<std::string> out;
    for (uint32_t a : impl_->order) {
        if (a == kHashCrc32) {
            const uint32_t c = ~impl_->crcState;
            const unsigned char be[4] = {static_cast<unsigned char>(c >> 24), static_cast<unsigned char>(c >> 16), static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c)};
            out.push_back(Hex(be, 4));
            continue;
        }
        for (auto& o : impl_->cng) {
            if (o.algo != a) continue;
            std::vector<unsigned char> d(o.hashLength);
            if (o.h && BCryptFinishHash(o.h, d.data(), o.hashLength, 0) == 0) out.push_back(Hex(d.data(), d.size()));
            else out.push_back(std::string());
        }
    }
    return out;
}

std::string HashHex(uint32_t algo, const void* data, size_t size) {
    MultiHasher h(algo);
    h.Update(data, size);
    auto v = h.Finish();
    return v.empty() ? std::string() : v[0];
}

// ---- files -----------------------------------------------------------------------------------------------------------------------

namespace {

[[noreturn]] void ThrowCancelled() { throw Error(static_cast<long>(0x80004004L), Message::Make(L"core.cancelled")); }

std::wstring SystemMessage(DWORD code) {
    wchar_t* msg = nullptr;
    const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                                   reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    std::wstring s = n ? std::wstring(msg, n) : L"Error " + std::to_wstring(code);
    if (msg) LocalFree(msg);
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n' || s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

}  // namespace

HashReport HashFiles(const std::vector<std::filesystem::path>& files, const HashOptions& options) {
    HashReport rep;
    rep.algos = NormalizeHashAlgos(options.algos);
    uint64_t total = 0;
    for (auto const& f : files) {
        std::error_code ec;
        const auto sz = std::filesystem::file_size(f, ec);
        if (!ec) total += sz;
    }
    uint64_t done = 0;
    std::vector<unsigned char> buf(1u << 20);
    for (auto const& f : files) {
        HashFileResult r;
        r.path = f.wstring();
        HANDLE h = CreateFileW(f.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            r.error = SystemMessage(GetLastError());
        } else {
            BY_HANDLE_FILE_INFORMATION info{};
            if (GetFileInformationByHandle(h, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) r.error = Message::Make(L"core.is_folder");
            MultiHasher hasher(rep.algos);
            uint64_t read = 0;
            while (r.error.empty()) {
                DWORD got = 0;
                if (!ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) { r.error = SystemMessage(GetLastError()); break; }
                if (got == 0) break;
                hasher.Update(buf.data(), got);
                read += got;
                done += got;
                if (options.progress && !options.progress(done, (std::max)(total, done))) { CloseHandle(h); ThrowCancelled(); }
            }
            CloseHandle(h);
            r.size = read;
            rep.bytes += read;
            if (r.error.empty()) r.hex = hasher.Finish();
        }
        if (!r.ok()) ++rep.failed;
        rep.files.push_back(std::move(r));
    }
    return rep;
}

// ---- entries of an archive -----------------------------------------------------------------------------------------------------

namespace {

class HashOutStream final : public ISequentialOutStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)
    Z7_IFACE_COM7_IMP(ISequentialOutStream)
public:
    HashOutStream(MultiHasher& h, uint64_t& count) : h_(h), count_(count) {}
private:
    MultiHasher& h_;
    uint64_t& count_;
};

Z7_COM7F_IMF(HashOutStream::Write(const void* data, UInt32 size, UInt32* processed)) {
    h_.Update(data, size);
    count_ += size;
    if (processed) *processed = size;
    return S_OK;
}

class HashTarget final : public ExtractTarget {
public:
    HashTarget(ArchiveSource::Impl& impl, const HashOptions& o, HashReport& rep, std::vector<size_t>& slotOf, bool hasPassword)
        : impl_(impl), o_(o), rep_(rep), slotOf_(slotOf), hasPassword_(hasPassword) {}

    CMyComPtr<ISequentialOutStream> Begin(uint32_t index) override {
        cur_ = kNone;
        if (index >= slotOf_.size() || slotOf_[index] == kNone) return nullptr;
        HashFileResult& r = rep_.files[slotOf_[index]];
        if (impl_.entries[index].isEncrypted && !hasPassword_) {
            r.error = Message::Make(L"core.encrypted_needs_password");
            return nullptr;
        }
        cur_ = index;
        count_ = 0;
        hasher_.reset(new MultiHasher(rep_.algos));
        return new HashOutStream(*hasher_, count_);
    }

    void End(uint32_t index, int opResult) override {
        if (cur_ != index) return;
        cur_ = kNone;
        HashFileResult& r = rep_.files[slotOf_[index]];
        if (!OpResultUsable(opResult)) {
            r.error = OpResultMessage(opResult);
        } else {
            r.hex = hasher_->Finish();
            r.size = count_;
            rep_.bytes += count_;
        }
        hasher_.reset();
    }

    static constexpr uint32_t kNone = 0xFFFFFFFFu;

private:
    ArchiveSource::Impl& impl_;
    const HashOptions& o_;
    HashReport& rep_;
    std::vector<size_t>& slotOf_;
    bool hasPassword_;
    uint32_t cur_ = kNone;
    uint64_t count_ = 0;
    std::unique_ptr<MultiHasher> hasher_;
};

}  // namespace

HashReport HashEntries(ArchiveSource& src, const std::vector<uint32_t>& indices, const HashOptions& options) {
    HashReport rep;
    rep.algos = NormalizeHashAlgos(options.algos);
    auto& impl = src.impl();
    std::vector<size_t> slotOf(impl.entries.size(), static_cast<size_t>(HashTarget::kNone));
    std::vector<uint32_t> wanted;
    for (uint32_t i : indices) {
        if (i >= impl.entries.size() || slotOf[i] != static_cast<size_t>(HashTarget::kNone)) continue;
        const Entry& e = impl.entries[i];
        if (e.isDir) continue;
        HashFileResult r;
        r.path = e.path;
        r.size = e.size;
        if (e.isSymlink || e.isHardLink) r.error = Message::Make(L"core.link_no_content");
        slotOf[i] = rep.files.size();
        rep.files.push_back(std::move(r));
        if (rep.files.back().ok()) wanted.push_back(i);
    }
    if (!wanted.empty()) {
        HashTarget target(impl, options, rep, slotOf, static_cast<bool>(options.password));
        RunExtract(impl, wanted, false, target, options.progress, options.password);
    }
    for (auto const& f : rep.files)
        if (!f.ok()) ++rep.failed;
    return rep;
}

// ---- comparing and checksum files ----------------------------------------------------------------------------------------------

uint32_t HashAlgosForLength(size_t n) {
    uint32_t mask = 0;
    for (uint32_t a : HashAlgoList(kHashAll))
        if (HashHexLength(a) == n) mask |= a;
    return mask;
}

std::string NormalizeHashText(std::wstring_view text) {
    std::string out;
    size_t i = 0;
    while (i < text.size() && std::iswspace(text[i])) ++i;
    if (i + 1 < text.size() && text[i] == L'0' && (text[i + 1] == L'x' || text[i + 1] == L'X')) i += 2;
    for (; i < text.size(); ++i) {
        const wchar_t c = text[i];
        if (std::iswspace(c) || c == L'-' || c == L':' || c == L'"' || c == L'\'' || c == 0x200B || c == 0xFEFF) continue;
        if (c >= L'0' && c <= L'9') out += static_cast<char>(c);
        else if (c >= L'a' && c <= L'f') out += static_cast<char>(c);
        else if (c >= L'A' && c <= L'F') out += static_cast<char>(c - L'A' + L'a');
        else return {};
    }
    return out;
}

namespace {

bool AllHex(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

std::string LowerAscii(std::string_view s) {
    std::string o(s);
    for (auto& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

uint32_t BsdAlgo(std::string_view w) {
    const std::string l = LowerAscii(w);
    if (l == "md5") return kHashMd5;
    if (l == "sha1") return kHashSha1;
    if (l == "sha256") return kHashSha256;
    if (l == "sha384") return kHashSha384;
    if (l == "sha512") return kHashSha512;
    if (l == "crc32") return kHashCrc32;
    return 0;
}

}  // namespace

bool ParseSumLine(std::string_view line, SumLine& out) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) line.remove_suffix(1);
    if (line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") line.remove_prefix(3);
    if (line.empty() || line.front() == '#' || line.front() == ';') return false;
    // BSD: SHA256 (name) = hex
    const size_t paren = line.find(" (");
    if (paren != std::string_view::npos && paren > 0 && paren <= 8) {
        const uint32_t algo = BsdAlgo(line.substr(0, paren));
        const size_t close = line.rfind(") = ");
        if (algo && close != std::string_view::npos && close > paren + 2) {
            const std::string_view hex = line.substr(close + 4);
            if (AllHex(hex) && HashHexLength(algo) == hex.size()) {
                out.hex = LowerAscii(hex);
                out.name = Utf8ToWide(line.substr(paren + 2, close - paren - 2));
                out.algo = algo;
                return true;
            }
        }
    }
    // GNU: hex, a space, then " name" (text) or "*name" (binary)
    size_t i = 0;
    while (i < line.size() && AllHex(line.substr(i, 1))) ++i;
    if (i == 0 || !HashAlgosForLength(i) || i + 2 > line.size() || line[i] != ' ') return false;
    out.hex = LowerAscii(line.substr(0, i));
    out.algo = 0;
    const std::string_view rest = line.substr(i + 1);
    const std::string_view name = (rest[0] == '*' || rest[0] == ' ') ? rest.substr(1) : rest;
    if (name.empty()) return false;
    out.name = Utf8ToWide(name);
    return true;
}

std::string FormatSumLine(const std::string& hex, const std::wstring& name) {
    return hex + " *" + WideToUtf8(name);
}

namespace {

std::wstring NameFor(const HashFileResult& f, const std::filesystem::path& base) {
    std::wstring name = f.path;
    if (!base.empty()) {
        std::error_code ec;
        const auto rel = std::filesystem::path(f.path).lexically_relative(base);
        if (!rel.empty() && rel.wstring().rfind(L"..", 0) != 0) name = rel.wstring();
    }
    if (!base.empty()) std::replace(name.begin(), name.end(), L'\\', L'/');  // a checksum file is portable with '/'
    return name;
}

const char* BsdAlgoName(uint32_t algo) {
    switch (algo) {
        case kHashCrc32: return "CRC32";
        case kHashMd5: return "MD5";
        case kHashSha1: return "SHA1";
        case kHashSha256: return "SHA256";
        case kHashSha384: return "SHA384";
        case kHashSha512: return "SHA512";
    }
    return "";
}

}  // namespace

std::string HashReportText(const HashReport& report, uint32_t algo, const std::filesystem::path& base) {
    const auto list = HashAlgoList(report.algos);
    size_t at = list.size();
    for (size_t i = 0; i < list.size(); ++i) if (list[i] == algo) at = i;
    std::string out;
    if (at == list.size()) return out;
    for (auto const& f : report.files)
        if (f.ok() && f.hex.size() == list.size()) out += FormatSumLine(f.hex[at], NameFor(f, base)) + "\n";
    return out;
}

std::string HashReportTextAll(const HashReport& report, const std::filesystem::path& base) {
    const auto list = HashAlgoList(report.algos);
    std::string out;
    for (auto const& f : report.files) {
        if (!f.ok() || f.hex.size() != list.size()) continue;
        for (size_t i = 0; i < list.size(); ++i) out += std::string(BsdAlgoName(list[i])) + " (" + WideToUtf8(NameFor(f, base)) + ") = " + f.hex[i] + "\n";
    }
    return out;
}

}  // namespace lycan
