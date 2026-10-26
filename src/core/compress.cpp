#include "lycan/compress.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <exception>
#include <map>
#include <memory>
#include <set>

#include "7zip/IPassword.h"
#include "lycan/error.h"
#include "lycan/incompressible.h"
#include "lycan/iso.h"
#include "lycan/probe.h"
#include "lycan/junk.h"
#include "lycan/security.h"
#include "lycan/archive_source.h"
#include "lycan/tempdir.h"
#include "lycan/testhooks.h"
#include "lycan/update.h"
#include "archive_impl.h"
#include "sevenzip.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

// ---- File list ----------------------------------------------------------------------

struct Item {
    fs::path path;            // source on disk
    std::wstring arc;         // path inside the archive (separated by '\\'; the handler converts it to what the format needs)
    bool isDir = false;
    uint64_t size = 0;
    FILETIME ctime{}, atime{}, mtime{};
    DWORD attrib = 0;
    HANDLE preopened = INVALID_HANDLE_VALUE;  // tar.* second stage: the temporary tar that is already open (owned)
    // Changing an existing archive: >= 0 = this item of the opened archive is kept as it is (no data is read from disk);
    // `renamed` = only its path (in `arc`) is new.
    int64_t srcIndex = -1;
    bool renamed = false;
    uint64_t arcMtime = 0;    // kept item: its time in the archive (FILETIME, 0 = unknown): the update modes compare it with the file on the disk
    mutable int8_t probe = -1;  // smart store: -1 = content not looked at yet, 1 = looks compressed already, 0 = does not
};

uint64_t U64(DWORD hi, DWORD lo) { return (static_cast<uint64_t>(hi) << 32) | lo; }

bool SamePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return Lower(fs::absolute(a, ec).lexically_normal().wstring()) == Lower(fs::absolute(b, ec).lexically_normal().wstring());
}

void Walk(const fs::path& dir, const std::wstring& arcDir, std::vector<Item>& items, CompressReport& rep,
          const std::vector<fs::path>& exclude, bool skipThumbsDb) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((ToLongPath(dir) / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        rep.errors.push_back(Message::Make(L"core.cannot_list_folder", dir.wstring()));
        return;
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        fs::path p = dir / name;
        std::wstring arc = arcDir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {  // symbolic link / junction: do not follow
            rep.skipped.push_back(Message::Make(L"core.skipped_link", p.wstring()));
            continue;
        }
        if (std::any_of(exclude.begin(), exclude.end(), [&](const fs::path& e) { return SamePath(e, p); })) {
            rep.skipped.push_back(Message::Make(L"core.skipped_output_itself", p.wstring()));
            continue;
        }
        const bool isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (skipThumbsDb && !isDirectory && IsThumbsDbName(name)) {
            ++rep.thumbsDbSkipped;
            continue;
        }
        Item it;
        it.path = p;
        it.arc = arc;
        it.isDir = isDirectory;
        it.size = it.isDir ? 0 : U64(fd.nFileSizeHigh, fd.nFileSizeLow);
        it.ctime = fd.ftCreationTime;
        it.atime = fd.ftLastAccessTime;
        it.mtime = fd.ftLastWriteTime;
        it.attrib = fd.dwFileAttributes;
        items.push_back(it);
        if (it.isDir) Walk(p, arc, items, rep, exclude, skipThumbsDb);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void CollectInput(const fs::path& input, std::vector<Item>& items, CompressReport& rep, const std::vector<fs::path>& exclude,
                  PathMode pathMode = PathMode::Relative, bool skipThumbsDb = false) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(ToLongPath(input).c_str(), GetFileExInfoStandard, &d))
        throw Error(HRESULT_FROM_WIN32(GetLastError()), Message::Make(L"core.input_not_found", input.wstring()));
    fs::path clean = input;
    if (!clean.has_filename()) clean = clean.parent_path();  // remove the trailing '\\'
    std::wstring name = clean.filename().wstring();
    // "." and ".." have no usable name of their own: resolve to the real folder (otherwise the archive would hold
    // entries such as "..\proj\a.txt", which no extractor accepts)
    if (name == L"." || name == L".." || name.empty()) {
        std::error_code rec;
        clean = fs::absolute(clean, rec).lexically_normal();
        if (!clean.has_filename()) clean = clean.parent_path();
        name = clean.filename().wstring();
    }
    if (name.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.cannot_compress_drive_root", input.wstring()));
    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        rep.skipped.push_back(Message::Make(L"core.skipped_link", input.wstring()));
        return;
    }
    if (pathMode == PathMode::Full) {
        // Full path (without the drive letter / UNC prefix): keep every folder level
        std::error_code aec;
        std::wstring rel = fs::absolute(clean, aec).lexically_normal().relative_path().wstring();
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        if (!rel.empty()) name = rel;
    }
    Item it;
    it.path = clean;
    it.arc = name;
    it.isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    it.size = it.isDir ? 0 : U64(d.nFileSizeHigh, d.nFileSizeLow);
    it.ctime = d.ftCreationTime;
    it.atime = d.ftLastAccessTime;
    it.mtime = d.ftLastWriteTime;
    it.attrib = d.dwFileAttributes;
    items.push_back(it);
    if (it.isDir) Walk(clean, name, items, rep, exclude, skipThumbsDb);
}

// ---- Streams ---------------------------------------------------------------------------

class ReadStream final : public ISequentialInStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(ISequentialInStream)
    Z7_IFACE_COM7_IMP(ISequentialInStream)
public:
    ReadStream(HANDLE h, bool owns) : h_(h), owns_(owns) {}
    ~ReadStream() { if (owns_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
private:
    HANDLE h_;
    bool owns_;
};

Z7_COM7F_IMF(ReadStream::Read(void* data, UInt32 size, UInt32* processed)) {
    DWORD n = 0;
    if (processed) *processed = 0;
    if (!ReadFile(h_, data, size, &n, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (processed) *processed = n;
    return S_OK;
}

// A single output file (seekable, needed by 7z / zip to write the headers back).
class FileWriteStream final : public IOutStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(IOutStream)
    Z7_IFACE_COM7_IMP(ISequentialOutStream)
    Z7_IFACE_COM7_IMP(IOutStream)
public:
    explicit FileWriteStream(HANDLE h) : h_(h) {}
    ~FileWriteStream() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
private:
    HANDLE h_;
};

Z7_COM7F_IMF(FileWriteStream::Write(const void* data, UInt32 size, UInt32* processed)) {
    DWORD n = 0;
    if (processed) *processed = 0;
    if (TestWriteShouldFail(size)) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    if (!WriteFile(h_, data, size, &n, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (processed) *processed = n;
    return S_OK;
}
Z7_COM7F_IMF(FileWriteStream::Seek(Int64 offset, UInt32 origin, UInt64* newPos)) {
    if (origin > 2) return STG_E_INVALIDFUNCTION;
    LARGE_INTEGER in, out;
    in.QuadPart = offset;
    if (!SetFilePointerEx(h_, in, &out, origin)) return HRESULT_FROM_WIN32(GetLastError());
    if (newPos) *newPos = static_cast<UInt64>(out.QuadPart);
    return S_OK;
}
Z7_COM7F_IMF(FileWriteStream::SetSize(UInt64 newSize)) {
    LARGE_INTEGER cur, zero{};
    if (!SetFilePointerEx(h_, zero, &cur, FILE_CURRENT)) return HRESULT_FROM_WIN32(GetLastError());
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(newSize);
    if (!SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) || !SetEndOfFile(h_)) return HRESULT_FROM_WIN32(GetLastError());
    SetFilePointerEx(h_, cur, nullptr, FILE_BEGIN);
    return S_OK;
}

// Wraps an output stream for the BZip2 encoder of 7z.dll: when its (multi-threaded) encoder gets an error from the output, the
// threads wait for each other and the call never returns (found with a real full disk, 2026-10-14). So the first error is only
// remembered, the rest of the data is dropped, and the caller reports the error once the encoder is done.
class DeferErrorStream final : public IOutStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(IOutStream)
    Z7_IFACE_COM7_IMP(ISequentialOutStream)
    Z7_IFACE_COM7_IMP(IOutStream)
public:
    explicit DeferErrorStream(IOutStream* inner) : inner_(inner) {}
    HRESULT Failed() const { return failed_; }
private:
    CMyComPtr<IOutStream> inner_;
    HRESULT failed_ = S_OK;
};

Z7_COM7F_IMF(DeferErrorStream::Write(const void* data, UInt32 size, UInt32* processed)) {
    if (failed_ == S_OK) {
        UInt32 n = 0;
        const HRESULT hr = inner_->Write(data, size, &n);
        if (hr == S_OK) { if (processed) *processed = n; return S_OK; }
        failed_ = hr;
    }
    if (processed) *processed = size;
    return S_OK;
}
Z7_COM7F_IMF(DeferErrorStream::Seek(Int64 offset, UInt32 origin, UInt64* newPos)) {
    if (failed_ != S_OK) { if (newPos) *newPos = 0; return S_OK; }
    return inner_->Seek(offset, origin, newPos);
}
Z7_COM7F_IMF(DeferErrorStream::SetSize(UInt64 newSize)) { return failed_ != S_OK ? S_OK : inner_->SetSize(newSize); }

// Split output: one logical stream cut into <prefix>.001 / .002 ... of a fixed size (seekable, 7z needs to write its headers back).
class MultiVolWriteStream final : public IOutStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(IOutStream)
    Z7_IFACE_COM7_IMP(ISequentialOutStream)
    Z7_IFACE_COM7_IMP(IOutStream)
public:
    MultiVolWriteStream(fs::path prefix, uint64_t volSize) : prefix_(std::move(prefix)), vol_(volSize) {}
    ~MultiVolWriteStream() { for (auto& [i, h] : handles_) CloseHandle(h); }
    std::vector<fs::path> Files() const {
        std::vector<fs::path> v;
        for (uint64_t i = 0; i < count_; ++i) v.push_back(NameOf(i));
        return v;
    }
    fs::path NameOf(uint64_t i) const {
        wchar_t suffix[16];
        swprintf_s(suffix, L".%03llu", static_cast<unsigned long long>(i + 1));
        return fs::path(prefix_.wstring() + suffix);
    }
private:
    HANDLE Vol(uint64_t i) {
        auto it = handles_.find(i);
        if (it != handles_.end()) return it->second;
        HANDLE h = CreateFileW(ToLongPath(NameOf(i)).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return h;
        handles_[i] = h;
        count_ = std::max(count_, i + 1);
        return h;
    }
    fs::path prefix_;
    uint64_t vol_;
    uint64_t pos_ = 0, size_ = 0, count_ = 0;
    std::map<uint64_t, HANDLE> handles_;
};

Z7_COM7F_IMF(MultiVolWriteStream::Write(const void* data, UInt32 size, UInt32* processed)) {
    if (processed) *processed = 0;
    auto p = static_cast<const char*>(data);
    UInt32 left = size;
    while (left) {
        uint64_t vi = pos_ / vol_, off = pos_ % vol_;
        HANDLE h = Vol(vi);
        if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        UInt32 chunk = static_cast<UInt32>(std::min<uint64_t>(left, vol_ - off));
        if (TestWriteShouldFail(chunk)) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
        LARGE_INTEGER o;
        o.QuadPart = static_cast<LONGLONG>(off);
        DWORD w = 0;
        if (!SetFilePointerEx(h, o, nullptr, FILE_BEGIN) || !WriteFile(h, p, chunk, &w, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        pos_ += w;
        p += w;
        left -= w;
        if (w != chunk) break;
    }
    size_ = std::max(size_, pos_);
    if (processed) *processed = size - left;
    return S_OK;
}
Z7_COM7F_IMF(MultiVolWriteStream::Seek(Int64 offset, UInt32 origin, UInt64* newPos)) {
    Int64 base = origin == STREAM_SEEK_SET ? 0 : origin == STREAM_SEEK_CUR ? static_cast<Int64>(pos_)
               : origin == STREAM_SEEK_END ? static_cast<Int64>(size_) : -1;
    if (base < 0 || base + offset < 0) return STG_E_INVALIDFUNCTION;
    pos_ = static_cast<uint64_t>(base + offset);
    if (newPos) *newPos = pos_;
    return S_OK;
}
Z7_COM7F_IMF(MultiVolWriteStream::SetSize(UInt64 newSize)) {
    size_ = newSize;
    uint64_t need = newSize ? (newSize + vol_ - 1) / vol_ : 0;
    // Cut: delete the extra volumes, the last one is kept only up to the end
    for (auto it = handles_.begin(); it != handles_.end();) {
        if (it->first >= need) {
            CloseHandle(it->second);
            DeleteFileW(ToLongPath(NameOf(it->first)).c_str());
            it = handles_.erase(it);
        } else {
            ++it;
        }
    }
    count_ = need;
    if (need) {
        HANDLE h = Vol(need - 1);
        if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER o;
        o.QuadPart = static_cast<LONGLONG>(newSize - (need - 1) * vol_);
        if (!SetFilePointerEx(h, o, nullptr, FILE_BEGIN) || !SetEndOfFile(h)) return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

// ---- Update callback ---------------------------------------------------------------------------

class UpdateCb final : public IArchiveUpdateCallback2, public ICryptoGetTextPassword2, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_3(IArchiveUpdateCallback2, IArchiveUpdateCallback, ICryptoGetTextPassword2)
    Z7_IFACE_COM7_IMP(IProgress)
    Z7_IFACE_COM7_IMP(IArchiveUpdateCallback)
    Z7_IFACE_COM7_IMP(IArchiveUpdateCallback2)
    Z7_IFACE_COM7_IMP(ICryptoGetTextPassword2)
public:
    UpdateCb(const std::vector<Item>& items, const std::wstring& password, const ProgressFn& progress, CompressReport& rep,
             IInArchive* existing)
        : items_(items), existing_(existing), password_(password), progress_(progress), rep_(rep) {}
    std::exception_ptr error;  // exception from the progress callback, rethrown once 7z.dll has returned
    ~UpdateCb() { SecureZeroMemory(password_.data(), password_.size() * sizeof(wchar_t)); }

private:
    const std::vector<Item>& items_;
    IInArchive* existing_;  // the opened archive that Items with srcIndex >= 0 come from
    std::wstring password_;
    const ProgressFn& progress_;
    CompressReport& rep_;
    UInt64 total_ = 0;
};

Z7_COM7F_IMF(UpdateCb::SetTotal(UInt64 total)) {
    total_ = total;
    return S_OK;
}
Z7_COM7F_IMF(UpdateCb::SetCompleted(const UInt64* done)) {
    try {
        if (done && progress_ && !progress_(*done, total_)) return E_ABORT;
    } catch (...) {
        error = std::current_exception();
        return E_ABORT;
    }
    return S_OK;
}
Z7_COM7F_IMF(UpdateCb::GetUpdateItemInfo(UInt32 index, Int32* newData, Int32* newProps, UInt32* indexInArchive)) {
    if (index < items_.size() && items_[index].srcIndex >= 0) {
        // an item of the archive that is being changed: its packed data is copied as it is
        *newData = 0;
        *newProps = items_[index].renamed ? 1 : 0;
        *indexInArchive = static_cast<UInt32>(items_[index].srcIndex);
        return S_OK;
    }
    *newData = 1;
    *newProps = 1;
    *indexInArchive = static_cast<UInt32>(-1);
    return S_OK;
}
Z7_COM7F_IMF(UpdateCb::GetProperty(UInt32 index, PROPID id, PROPVARIANT* value)) {
    PropVariantInit(value);
    if (index >= items_.size()) return E_INVALIDARG;
    const Item& it = items_[index];
    if (it.srcIndex >= 0 && !(it.renamed && id == kpidPath)) {
        // everything but the new name of a renamed item is what the archive already says
        return existing_ ? existing_->GetProperty(static_cast<UInt32>(it.srcIndex), id, value) : E_FAIL;
    }
    switch (id) {
        case kpidPath:
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocStringLen(it.arc.data(), static_cast<UINT>(it.arc.size()));
            return value->bstrVal ? S_OK : E_OUTOFMEMORY;
        case kpidIsDir:
            value->vt = VT_BOOL;
            value->boolVal = it.isDir ? VARIANT_TRUE : VARIANT_FALSE;
            break;
        case kpidSize:
            value->vt = VT_UI8;
            value->uhVal.QuadPart = it.size;
            break;
        case kpidAttrib:
            value->vt = VT_UI4;
            value->ulVal = it.attrib;
            break;
        case kpidCTime: value->vt = VT_FILETIME; value->filetime = it.ctime; break;
        case kpidATime: value->vt = VT_FILETIME; value->filetime = it.atime; break;
        case kpidMTime: value->vt = VT_FILETIME; value->filetime = it.mtime; break;
        default: break;  // VT_EMPTY
    }
    return S_OK;
}
Z7_COM7F_IMF(UpdateCb::GetStream(UInt32 index, ISequentialInStream** stream)) {
    *stream = nullptr;
    if (index >= items_.size()) return E_INVALIDARG;
    const Item& it = items_[index];
    if (it.isDir || it.srcIndex >= 0) return S_OK;
    if (it.preopened != INVALID_HANDLE_VALUE) {
        CMyComPtr<ISequentialInStream> s(new ReadStream(it.preopened, false));
        *stream = s.Detach();
        return S_OK;
    }
    HANDLE h = CreateFileW(ToLongPath(it.path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        rep_.errors.push_back(Message::Make(L"core.cannot_read_file", it.path.wstring(), GetLastError()));
        return S_FALSE;  // skipped file: go on with the next
    }
    CMyComPtr<ISequentialInStream> s(new ReadStream(h, true));
    *stream = s.Detach();
    return S_OK;
}
Z7_COM7F_IMF(UpdateCb::SetOperationResult(Int32)) { return S_OK; }
Z7_COM7F_IMF(UpdateCb::GetVolumeSize(UInt32, UInt64*)) { return S_FALSE; }  // volumes are handled by MultiVolWriteStream outside
Z7_COM7F_IMF(UpdateCb::GetVolumeStream(UInt32, ISequentialOutStream** s)) {
    *s = nullptr;
    return S_FALSE;
}
Z7_COM7F_IMF(UpdateCb::CryptoGetTextPassword2(Int32* defined, BSTR* password)) {
    *defined = password_.empty() ? 0 : 1;
    *password = SysAllocStringLen(password_.data(), static_cast<UINT>(password_.size()));
    return *password ? S_OK : E_OUTOFMEMORY;
}

// ---- Properties -----------------------------------------------------------------------------

struct Prop {
    std::wstring name;
    PROPVARIANT v;
};

class PropList {
public:
    ~PropList() { for (auto& p : props_) PropVariantClear(&p.v); }
    void Num(const wchar_t* n, UInt32 val) {
        Prop p{n, {}};
        PropVariantInit(&p.v);
        p.v.vt = VT_UI4;
        p.v.ulVal = val;
        Put(p);
    }
    void Str(const wchar_t* n, const wchar_t* val) {
        Prop p{n, {}};
        PropVariantInit(&p.v);
        p.v.vt = VT_BSTR;
        p.v.bstrVal = SysAllocString(val);
        Put(p);
    }
    void Bool(const wchar_t* n, bool val) {
        Prop p{n, {}};
        PropVariantInit(&p.v);
        p.v.vt = VT_BOOL;
        p.v.boolVal = val ? VARIANT_TRUE : VARIANT_FALSE;
        Put(p);
    }
    HRESULT Apply(IOutArchive* arc) {
        CMyComPtr<ISetProperties> sp;
        arc->QueryInterface(IID_ISetProperties, reinterpret_cast<void**>(&sp));
        if (!sp || props_.empty()) return S_OK;
        std::vector<const wchar_t*> names;
        std::vector<PROPVARIANT> vals;
        for (auto& p : props_) {
            names.push_back(p.name.c_str());
            vals.push_back(p.v);
        }
        return sp->SetProperties(names.data(), vals.data(), static_cast<UInt32>(props_.size()));
    }
private:
    // The properties are set in this order (custom parameters may override the defaults; x / mt are not passed to 7-Zip twice)
    void Put(Prop& p) {
        for (auto& e : props_)
            if (_wcsicmp(e.name.c_str(), p.name.c_str()) == 0) {
                PropVariantClear(&e.v);
                e = p;
                return;
            }
        props_.push_back(p);
    }
    std::vector<Prop> props_;
};

// Custom parameters: separated by spaces, "name=value"; the name may have a leading -m ("-mx=9" / "mx=9" both work).
// Entries without "=" and entries with control characters are refused, so that strings that cannot be understood are not passed on.
void ParseCustomParams(const std::wstring& text, PropList& props) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && iswspace(text[i])) ++i;
        size_t j = i;
        while (j < text.size() && !iswspace(text[j])) ++j;
        if (j == i) break;
        std::wstring tok = text.substr(i, j - i);
        i = j;
        if (tok.rfind(L"-m", 0) == 0) tok.erase(0, 2);
        else if (!tok.empty() && tok[0] == L'-') tok.erase(0, 1);
        size_t eq = tok.find(L'=');
        if (eq == std::wstring::npos || eq == 0)
            throw Error(E_INVALIDARG, Message::Make(L"core.custom_param_format", tok));
        std::wstring name = tok.substr(0, eq), val = tok.substr(eq + 1);
        for (wchar_t ch : name + val)
            if (ch < 0x20 || ch == L'"') throw Error(E_INVALIDARG, Message::Make(L"core.custom_param_bad_char"));
        // a plain number is passed as a number (7-Zip wants x, fb, mt ... as numbers, not as strings)
        const bool numeric = !val.empty() && val.size() <= 9 && std::all_of(val.begin(), val.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
        if (numeric) props.Num(name.c_str(), static_cast<UInt32>(std::stoul(val)));
        else props.Str(name.c_str(), val.c_str());
    }
}

bool IsStoreCandidate(const Item& it);

constexpr uint64_t kAutoSolidBlock = 256ull << 20;  // solid block size when CompressOptions::solidSize is kSolidAuto

int LevelFor(const CompressOptions& o) {
    if (o.level >= 0) return std::min(o.level, 9);
    switch (o.profile) {
        case Profile::Fastest: return 1;
        case Profile::Smallest: return 9;
        default: return 5;
    }
}

const wchar_t* FormatNameFor(ArchiveType t) {
    switch (t) {
        case ArchiveType::Zip: return L"zip";
        case ArchiveType::SevenZip: return L"7z";
        case ArchiveType::Tar: return L"tar";
        case ArchiveType::TarGz: return L"gzip";
        case ArchiveType::TarXz: return L"xz";
        case ArchiveType::TarBz2: return L"bzip2";
        case ArchiveType::Iso: return L"iso";
    }
    return L"";
}

// Makes one archive of one format from a set of items and writes it to outStream.
// existing: the opened archive that is being changed (Items with srcIndex >= 0 refer to it); null = a new archive.
void UpdateOnce(const wchar_t* formatName, const std::vector<Item>& items, IOutStream* outStream, const CompressOptions& o,
                bool applyPassword, CompressReport& rep, IInArchive* existing = nullptr) {
    auto& lib = SevenZipLib::Get();
    const FormatInfo* f = lib.FindFormat(formatName);
    if (!f || !f->canUpdate) throw Error(E_NOTIMPL, Message::Make(L"core.format_cannot_create", formatName));
    CMyComPtr<IOutArchive> arc;
    if (existing) existing->QueryInterface(IID_IOutArchive, reinterpret_cast<void**>(&arc));
    else arc = lib.CreateOutArchive(f->clsid);
    if (!arc) throw Error(E_FAIL, existing ? Message::Make(L"core.format_cannot_modify") : Message::Make(L"core.cannot_create_handler"));

    PropList props;
    std::wstring fn = formatName;
    if (fn != L"tar") {
        props.Num(L"x", static_cast<UInt32>(LevelFor(o)));
        if (o.threads) props.Num(L"mt", o.threads);
    }
    const bool isZip = fn == L"zip", is7z = fn == L"7z";
    if ((isZip || is7z) && !o.method.empty()) {
        if (isZip) props.Str(L"m", o.method.c_str());
        else props.Str(L"0", o.method.c_str());
    }
    if (isZip || is7z) {
        const bool ppmd = _wcsicmp(o.method.c_str(), L"PPMd") == 0;
        if (o.dictSize) props.Str(ppmd ? L"mem" : L"d", (std::to_wstring(o.dictSize) + L"b").c_str());
        if (o.wordSize) props.Num(ppmd ? L"o" : L"fb", o.wordSize);
    }
    if (is7z && o.solidSize == kSolidAuto && o.profile != Profile::Smallest && o.level != 0) {
        // What goes to the compressor decides: mostly files that are compressed already -> no solid blocks (nothing to gain,
        // and each file can then be read alone, e.g. for thumbnails); otherwise solid, but a block stops growing at a fixed size
        // so that reading one file never means unpacking gigabytes before it.
        uint64_t all = 0, packed = 0;
        for (const Item& it : items) {
            if (it.srcIndex >= 0 || it.isDir || it.preopened != INVALID_HANDLE_VALUE) continue;
            all += it.size;
            if (IsAlreadyCompressedName(it.arc) || (o.storeCompressed && IsStoreCandidate(it))) packed += it.size;
        }
        if (all && packed * 10 >= all * 7) props.Str(L"s", L"off");
        else props.Str(L"s", (std::to_wstring(kAutoSolidBlock) + L"b").c_str());
    }
    if (is7z && o.solidSize != kSolidAuto) {
        if (o.solidSize == kSolidOff) props.Str(L"s", L"off");
        else if (o.solidSize == kSolidAll) props.Str(L"s", L"on");
        else props.Str(L"s", (std::to_wstring(o.solidSize) + L"b").c_str());
    }
    ParseCustomParams(o.customParams, props);
    std::wstring password = applyPassword ? o.password : L"";
    if (fn == L"zip" && !password.empty()) {
        if (!o.zipEncryption.empty()) {
            props.Str(L"em", o.zipEncryption.c_str());
            if (_wcsicmp(o.zipEncryption.c_str(), L"ZipCrypto") == 0)
                rep.warnings.push_back(Message::Make(L"core.zipcrypto_weak"));
        } else if (o.profile == Profile::Compatible) {
            props.Str(L"em", L"ZipCrypto");
            rep.warnings.push_back(Message::Make(L"core.zipcrypto_weak_compat"));
        } else {
            props.Str(L"em", L"AES256");
        }
    }
    if (fn == L"7z" && !password.empty() && o.encryptFileNames) props.Bool(L"he", true);
    if (props.Apply(arc) != S_OK)
        throw Error(E_INVALIDARG, o.customParams.empty() ? Message::Make(L"core.set_properties_failed")
                                                         : Message::Make(L"core.set_properties_failed_custom"));

    UpdateCb* raw = new UpdateCb(items, password, o.progress, rep, existing);
    CMyComPtr<IArchiveUpdateCallback> cb(raw);
    // BZip2 anywhere (the format itself, or the method of a zip / 7z) runs threads that hang on an output error: see DeferErrorStream
    std::wstring paramsLower = o.customParams;
    for (wchar_t& c : paramsLower) c = static_cast<wchar_t>(towlower(c));
    const bool bzip2 = fn == L"bzip2" || _wcsicmp(o.method.c_str(), L"BZip2") == 0 || paramsLower.find(L"bzip2") != std::wstring::npos;
    CMyComPtr<IOutStream> guarded;
    IOutStream* target = outStream;
    DeferErrorStream* guard = nullptr;
    if (bzip2) { guard = new DeferErrorStream(outStream); guarded = guard; target = guard; }
    HRESULT hr = arc->UpdateItems(target, static_cast<UInt32>(items.size()), cb);
    if (guard && guard->Failed() != S_OK && hr != E_ABORT) hr = guard->Failed();
    if (raw->error) std::rethrow_exception(raw->error);
    if (hr == E_ABORT) throw Error(E_ABORT, Message::Make(L"core.cancelled"));
    if (hr != S_OK) {
        const Error probe(hr, std::string());
        throw Error(hr, probe.diskFull() ? Message::Make(L"core.disk_full_archive") : existing ? Message::Make(L"core.update_failed") : Message::Make(L"core.create_failed"));
    }
}

// Reads back the archive of the first pass of smart store (the handle belongs to the stream that wrote it).
class HandleInStream final : public IInStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(IInStream)
    Z7_IFACE_COM7_IMP(ISequentialInStream)
    Z7_IFACE_COM7_IMP(IInStream)
public:
    explicit HandleInStream(HANDLE h) : h_(h) {}
private:
    HANDLE h_;
};

Z7_COM7F_IMF(HandleInStream::Read(void* data, UInt32 size, UInt32* processed)) {
    DWORD n = 0;
    if (processed) *processed = 0;
    if (!ReadFile(h_, data, size, &n, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (processed) *processed = n;
    return S_OK;
}
Z7_COM7F_IMF(HandleInStream::Seek(Int64 offset, UInt32 origin, UInt64* newPos)) {
    if (origin > 2) return STG_E_INVALIDFUNCTION;
    LARGE_INTEGER in, out;
    in.QuadPart = offset;
    if (!SetFilePointerEx(h_, in, &out, origin)) return HRESULT_FROM_WIN32(GetLastError());
    if (newPos) *newPos = static_cast<UInt64>(out.QuadPart);
    return S_OK;
}

// A new file that is compressed already: by its extension, otherwise by a look at its content (probe.h). The answer is kept in
// the item, so a file is looked at once however many times it is asked about.
bool IsStoreCandidate(const Item& it) {
    if (it.srcIndex >= 0 || it.isDir || it.preopened != INVALID_HANDLE_VALUE || it.size < kStoreMinBytes) return false;
    if (IsAlreadyCompressedName(it.arc)) return true;
    if (it.probe < 0) it.probe = FileLooksIncompressible(it.path, it.size) ? 1 : 0;
    return it.probe == 1;
}

// UpdateOnce plus "smart store" (CompressOptions::storeCompressed): new files that are compressed already are put in as they
// are. 7z.dll sets the method for a whole call, so a mix of stored and packed files takes two calls: the first builds an
// archive with everything that stays (old items, folders) and the files to store, with level 0; the second adds the packed
// files to that archive (what is in it is copied as it is). `keptBytes`: size of the old data that the first call copies.
void UpdateSmart(const wchar_t* formatName, const std::vector<Item>& items, IOutStream* out, const CompressOptions& o,
                 CompressReport& rep, IInArchive* existing = nullptr, uint64_t keptBytes = 0) {
    const bool zipOr7z = _wcsicmp(formatName, L"zip") == 0 || _wcsicmp(formatName, L"7z") == 0;
    // 7z.dll fails (E_FAIL) when the second call copies ZipCrypto items of the first, so such archives are made in one pass
    const bool zipCrypto = _wcsicmp(formatName, L"zip") == 0 && !o.password.empty() &&
                           (o.zipEncryption.empty() ? o.profile == Profile::Compatible : _wcsicmp(o.zipEncryption.c_str(), L"ZipCrypto") == 0);
    size_t nStored = 0, nPacked = 0;
    uint64_t storedBytes = 0, packedBytes = 0;
    if (zipOr7z && !zipCrypto && o.storeCompressed && o.level != 0) {
        for (const Item& it : items) {
            if (it.srcIndex >= 0 || it.isDir) continue;
            if (IsStoreCandidate(it)) { ++nStored; storedBytes += it.size; }
            else { ++nPacked; packedBytes += it.size; }
        }
    }
    if (!nStored) { UpdateOnce(formatName, items, out, o, true, rep, existing); return; }

    CompressOptions plain = o;  // for the files that are stored: level 0, nothing else to tune
    plain.level = 0;
    plain.method.clear();
    plain.dictSize = 0;
    plain.wordSize = 0;
    plain.customParams.clear();
    plain.solidSize = kSolidAuto;
    if (!nPacked) {
        UpdateOnce(formatName, items, out, plain, true, rep, existing);
        rep.storedFiles += static_cast<uint32_t>(nStored);
        return;
    }

    // Mixed. The first archive lives in a scratch file that Windows deletes when it is closed.
    const fs::path scratch = ScratchDirectory(o.tempDir);
    uint64_t have = 0;
    if (ScratchFreeBytes(scratch, have) && storedBytes + keptBytes + (1u << 20) > have) {
        rep.warnings.push_back(Message::Make(L"core.temp_space_plain"));
        UpdateOnce(formatName, items, out, o, true, rep, existing);
        return;
    }
    HANDLE h = CreateScratchFile(L"lycanzip_s_", o.tempDir);
    if (h == INVALID_HANDLE_VALUE) { UpdateOnce(formatName, items, out, o, true, rep, existing); return; }
    CMyComPtr<IOutStream> mid(new FileWriteStream(h));  // owns the handle

    std::vector<Item> first, packed;
    for (const Item& it : items) {
        if (it.srcIndex >= 0 || it.isDir || IsStoreCandidate(it)) first.push_back(it);
        else packed.push_back(it);
    }
    const uint64_t total = storedBytes + packedBytes;
    {
        CompressOptions c1 = plain;
        c1.encryptFileNames = false;  // the header of this archive is only read back here; the last call encrypts the final one
        if (o.progress) c1.progress = [&](uint64_t d, uint64_t) { return o.progress((std::min)(d, storedBytes), total); };
        CompressReport r1;
        UpdateOnce(formatName, first, mid, c1, true, r1, existing);
        for (auto& e : r1.errors) rep.errors.push_back(e);
    }

    auto& lib = SevenZipLib::Get();
    const FormatInfo* f = lib.FindFormat(formatName);
    CMyComPtr<IInArchive> a = lib.CreateInArchive(f->clsid);
    CMyComPtr<IInStream> rs(new HandleInStream(h));
    const UInt64 scan = 1 << 23;
    LARGE_INTEGER zero{};
    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
    if (!a || a->Open(rs, &scan, nullptr) != S_OK) throw Error(E_FAIL, Message::Make(L"core.cannot_read_back_temp"));
    UInt32 n = 0;
    a->GetNumberOfItems(&n);
    std::vector<Item> second;
    for (UInt32 i = 0; i < n; ++i) {
        Item k;
        k.srcIndex = i;
        second.push_back(std::move(k));
    }
    for (Item& p : packed) second.push_back(std::move(p));
    CompressOptions c2 = o;
    if (o.progress) c2.progress = [&](uint64_t d, uint64_t) { return o.progress(storedBytes + (std::min)(d, packedBytes), total); };
    UpdateOnce(formatName, second, out, c2, true, rep, a);
    a->Close();
    rep.storedFiles += static_cast<uint32_t>(nStored);
}

HANDLE CreateTempFile(const fs::path& dir) { return CreateScratchFile(L"lycanzip_c_", dir); }

std::wstring TarNameFor(const fs::path& output) {
    // foo.tar.gz / foo.tgz -> foo.tar
    std::wstring stem = output.stem().wstring();
    std::wstring ext = Lower(output.extension().wstring());
    if (ext == L".tgz" || ext == L".tbz2" || ext == L".tbz" || ext == L".txz") return stem + L".tar";
    if (Lower(fs::path(stem).extension().wstring()) == L".tar") return stem;
    return stem + L".tar";
}

// What 7-Zip's zip encryption accepts as a password; a clear message instead of its bare E_INVALIDARG
void CheckZipPassword(const CompressOptions& options) {
    // Encryption in zip (AES / ZipCrypto) takes ASCII passwords only; 7-Zip answers E_INVALIDARG for other characters, so say it in plain words
    if (options.type == ArchiveType::Zip &&
        std::any_of(options.password.begin(), options.password.end(), [](wchar_t c) { return c > 0x7F; }))
        throw Error(E_INVALIDARG, Message::Make(L"core.zip_password_ascii"));
    // 7-Zip's zip AES handler rejects passwords longer than 99 characters (ZipCrypto and 7z have no such limit)
    if (options.type == ArchiveType::Zip && options.password.size() > 99 && options.profile != Profile::Compatible &&
        (options.zipEncryption.empty() || _wcsicmp(options.zipEncryption.c_str(), L"AES256") == 0))
        throw Error(E_INVALIDARG, Message::Make(L"core.zip_aes_password_long"));
}

}  // namespace

bool GuessArchiveType(const fs::path& file, ArchiveType& type) {
    std::wstring name = Lower(file.filename().wstring());
    auto ends = [&](const wchar_t* s) { std::wstring e = s; return name.size() >= e.size() && name.compare(name.size() - e.size(), e.size(), e) == 0; };
    if (ends(L".tar.gz") || ends(L".tgz")) type = ArchiveType::TarGz;
    else if (ends(L".tar.xz") || ends(L".txz")) type = ArchiveType::TarXz;
    else if (ends(L".tar.bz2") || ends(L".tbz2") || ends(L".tbz")) type = ArchiveType::TarBz2;
    else if (ends(L".tar")) type = ArchiveType::Tar;
    else if (ends(L".iso")) type = ArchiveType::Iso;
    else if (ends(L".zip")) type = ArchiveType::Zip;
    else if (ends(L".7z")) type = ArchiveType::SevenZip;
    else return false;
    return true;
}

CompressReport CreateArchive(const fs::path& output, const std::vector<fs::path>& inputs, const CompressOptions& options) {
    CompressReport rep;
    if (inputs.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.no_input"));
    const bool isIso = options.type == ArchiveType::Iso;
    const bool isTarFamily = options.type != ArchiveType::Zip && options.type != ArchiveType::SevenZip && !isIso;
    if (isIso && options.volumeSize) throw Error(E_INVALIDARG, Message::Make(L"core.iso_no_volumes"));
    if (isIso && !options.password.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.iso_no_password"));
    if (options.volumeSize && options.volumeSize < 64 * 1024) throw Error(E_INVALIDARG, Message::Make(L"core.volume_too_small"));
    if (options.volumeSize && isTarFamily) throw Error(E_INVALIDARG, Message::Make(L"core.volumes_zip_7z_only"));
    if (!options.password.empty() && isTarFamily) throw Error(E_INVALIDARG, Message::Make(L"core.tar_no_password"));
    CheckZipPassword(options);

    // the output file (also used for volumes) must not be one of the inputs
    std::vector<fs::path> exclude{output};
    for (int i = 1; i <= 3; ++i) {
        wchar_t suf[16];
        swprintf_s(suf, L".%03d", i);
        exclude.push_back(fs::path(output.wstring() + suf));
    }

    std::vector<Item> items;
    for (auto& in : inputs) CollectInput(in, items, rep, exclude, options.pathMode, options.skipThumbsDb);
    if (items.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.nothing_to_compress"));
    for (auto& it : items) {
        if (it.isDir) ++rep.dirs;
        else { ++rep.files; rep.bytesIn += it.size; }
    }

    // check of the target file
    std::error_code ec;
    std::vector<fs::path> targets;
    if (options.volumeSize) targets.push_back(fs::path(output.wstring() + L".001"));
    else targets.push_back(output);
    for (auto& t : targets)
        if (fs::exists(ToLongPath(t), ec) && !options.overwrite)
            throw Error(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), Message::Make(L"core.target_exists", t.wstring()));
    if (output.has_parent_path()) fs::create_directories(ToLongPath(output.parent_path()), ec);

    // Write under a temporary name; after success rename it to the final name; on failure clean up
    fs::path tmpOut = output;
    tmpOut += L".lycantmp";
    CMyComPtr<IOutStream> out;
    HANDLE rawOut = INVALID_HANDLE_VALUE;   // the output file (owned by `out`), for the ISO writer
    MultiVolWriteStream* multi = nullptr;
    std::vector<fs::path> created;

    auto cleanup = [&] {
        if (multi) created = multi->Files();  // when split output: get the volumes that were created before the stream is released
        out.Release();
        std::error_code e2;
        fs::remove(ToLongPath(tmpOut), e2);
        for (auto& c : created) fs::remove(ToLongPath(c), e2);
    };

    try {
        if (options.volumeSize) {
            multi = new MultiVolWriteStream(output, options.volumeSize);
            out = multi;
        } else {
            HANDLE outHandle = CreateFileW(ToLongPath(tmpOut).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (outHandle == INVALID_HANDLE_VALUE)
                throw Error(HRESULT_FROM_WIN32(GetLastError()), Message::Make(L"core.cannot_create_output", tmpOut.wstring()));
            out = new FileWriteStream(outHandle);  // the handle now belongs to it
            rawOut = outHandle;
        }

        if (isIso) {
            std::vector<IsoItem> iso;
            for (const auto& it : items) {
                IsoItem x;
                x.source = it.path;
                x.path = it.arc;
                x.isDir = it.isDir;
                x.size = it.size;
                x.mtime = (static_cast<uint64_t>(it.mtime.dwHighDateTime) << 32) | it.mtime.dwLowDateTime;
                iso.push_back(std::move(x));
            }
            IsoOptions io;
            io.label = options.isoLabel.empty() ? output.stem().wstring() : options.isoLabel;
            io.keepTimes = options.isoKeepTimes;
            io.progress = options.progress;
            IsoResult ir = WriteIso(rawOut, iso, io);
            for (auto& e : ir.errors) rep.errors.push_back(e);
        } else if (!isTarFamily || options.type == ArchiveType::Tar) {
            UpdateSmart(FormatNameFor(options.type), items, out, options, rep);
        } else {
            // tar.gz / tar.xz / tar.bz2: two stages (tar into a temporary file first, then compress that into one stream)
            // The uncompressed tar is as big as all the input: check the room first, before anything is written
            const fs::path scratch = ScratchDirectory(options.tempDir);
            uint64_t need = 0, have = 0;
            for (const auto& it : items) need += it.size + 1024;  // + the tar headers
            if (ScratchFreeBytes(scratch, have) && need > have) throw Error(Error::kTempSpace, MakeTempSpaceMessage(need, have, scratch));
            HANDLE tmpTar = CreateTempFile(options.tempDir);
            if (tmpTar == INVALID_HANDLE_VALUE) throw Error(E_FAIL, Message::Make(L"core.cannot_create_temp"));
            CMyComPtr<IOutStream> tarOut(new FileWriteStream(tmpTar));  // owns the handle; the temporary file is deleted when it is released
            // progress: the tar stage is the first half, the compression stage the second half
            CompressOptions stage = options;
            if (options.progress) {
                stage.progress = [&](uint64_t d, uint64_t t) { return options.progress(d / 2, t); };
            }
            UpdateOnce(L"tar", items, tarOut, stage, false, rep);
            LARGE_INTEGER zero{};
            LARGE_INTEGER sz;
            GetFileSizeEx(tmpTar, &sz);
            SetFilePointerEx(tmpTar, zero, nullptr, FILE_BEGIN);

            Item tarItem;
            tarItem.arc = TarNameFor(output);
            tarItem.size = static_cast<uint64_t>(sz.QuadPart);
            tarItem.attrib = FILE_ATTRIBUTE_NORMAL;
            tarItem.preopened = tmpTar;
            GetSystemTimeAsFileTime(&tarItem.mtime);
            tarItem.ctime = tarItem.atime = tarItem.mtime;
            if (options.progress) {
                uint64_t half = rep.bytesIn;
                // Second half: the staged tar (a little larger than the input because of its headers) is compressed.
                // Scale by the stage's own total so the overall figure never passes the grand total.
                const uint64_t tarSize = tarItem.size;
                stage.progress = [&, half, tarSize](uint64_t d, uint64_t t) {
                    const uint64_t tot = t ? t : tarSize;
                    const uint64_t part = tot ? static_cast<uint64_t>(static_cast<long double>((std::min)(d, tot)) / static_cast<long double>(tot) * static_cast<long double>(half - half / 2)) : 0;
                    return options.progress(half / 2 + part, half ? half : t);
                };
            }
            UpdateOnce(FormatNameFor(options.type), {tarItem}, out, stage, false, rep);
        }
    } catch (...) {
        cleanup();
        throw;
    }

    // Done: close the streams, rename, statistics
    if (multi) {
        created = multi->Files();
        out.Release();
        for (auto& c : created) {
            WIN32_FILE_ATTRIBUTE_DATA d;
            if (GetFileAttributesExW(ToLongPath(c).c_str(), GetFileExInfoStandard, &d)) rep.bytesOut += U64(d.nFileSizeHigh, d.nFileSizeLow);
            rep.outputs.push_back(c);
        }
        created.clear();
    } else {
        out.Release();  // release the handle
        std::error_code e2;
        auto size = fs::file_size(ToLongPath(tmpOut), e2);
        if (!e2) rep.bytesOut = size;
        if (options.overwrite) fs::remove(ToLongPath(output), e2);
        fs::rename(ToLongPath(tmpOut), ToLongPath(output), e2);
        if (e2) {
            fs::remove(ToLongPath(tmpOut), e2);
            throw Error(E_FAIL, Message::Make(L"core.cannot_rename_temp_output", output.wstring()));
        }
        rep.outputs.push_back(output);
    }
    return rep;
}

// ---- Changing an existing archive (see update.h) ------------------------------------------------------------

UpdateOp UpdateOp::Add(std::wstring intoFolder, std::vector<fs::path> inputs) {
    UpdateOp o;
    o.kind = Kind::Add;
    o.path = std::move(intoFolder);
    o.inputs = std::move(inputs);
    return o;
}
UpdateOp UpdateOp::Delete(std::wstring path) {
    UpdateOp o;
    o.kind = Kind::Delete;
    o.path = std::move(path);
    return o;
}
UpdateOp UpdateOp::Rename(std::wstring from, std::wstring to) {
    UpdateOp o;
    o.kind = Kind::Rename;
    o.path = std::move(from);
    o.newPath = std::move(to);
    return o;
}
UpdateOp UpdateOp::MakeDir(std::wstring path) {
    UpdateOp o;
    o.kind = Kind::MakeDir;
    o.path = std::move(path);
    return o;
}
UpdateOp UpdateOp::Replace(std::wstring path, fs::path file) {
    UpdateOp o;
    o.kind = Kind::Replace;
    o.path = std::move(path);
    o.file = std::move(file);
    return o;
}

namespace {

// Paths inside the plan: '\\' separated, no leading or trailing separator.
std::wstring NormArcPath(std::wstring p) {
    std::replace(p.begin(), p.end(), L'/', L'\\');
    const size_t b = p.find_first_not_of(L'\\'), e = p.find_last_not_of(L'\\');
    return b == std::wstring::npos ? std::wstring() : p.substr(b, e - b + 1);
}

bool IsUnder(const std::wstring& path, const std::wstring& folder) {
    return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == L'\\';
}

// A name that may go into an archive: no empty parts, no "." / "..", nothing Windows cannot extract.
void CheckNewArcPath(const std::wstring& p) {
    bool bad = p.empty();
    size_t start = 0;
    while (!bad && start <= p.size()) {
        size_t end = p.find(L'\\', start);
        if (end == std::wstring::npos) end = p.size();
        const std::wstring part = p.substr(start, end - start);
        if (part.empty() || part == L"." || part == L"..") bad = true;
        start = end + 1;
    }
    for (wchar_t c : p)
        if (c < 0x20 || c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|') bad = true;
    if (bad) throw Error(E_INVALIDARG, Message::Make(L"core.invalid_name", p));
}

Item ItemFromDisk(const fs::path& file, const std::wstring& arc) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(ToLongPath(file).c_str(), GetFileExInfoStandard, &d))
        throw Error(HRESULT_FROM_WIN32(GetLastError()), Message::Make(L"core.file_not_found", file.wstring()));
    Item it;
    it.path = file;
    it.arc = arc;
    it.isDir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    it.size = it.isDir ? 0 : U64(d.nFileSizeHigh, d.nFileSizeLow);
    it.ctime = d.ftCreationTime;
    it.atime = d.ftLastAccessTime;
    it.mtime = d.ftLastWriteTime;
    it.attrib = d.dwFileAttributes;
    return it;
}

struct Plan {
    std::vector<Item> items;           // what the new archive holds, in order (kept items have srcIndex >= 0)
    std::vector<uint32_t> dropped;     // items of the old archive that are gone (deleted, or their data replaced)
    uint64_t newBytes = 0;
    bool changed = false;
};

struct Prepared {
    std::unique_ptr<ArchiveSource> src;
    CompressOptions co;
    ArchiveType type = ArchiveType::Zip;
    Plan plan;
    UpdateReport rep;
};

uint64_t FileTimeValue(const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// Times of the file on the disk and of the item in the archive. A zip keeps the local time in steps of 2 seconds without a time zone, so a difference of
// 2 seconds is none, and neither is one of exactly an hour (the clock was put forward or back since the file was added).
bool TimesDiffer(uint64_t disk, uint64_t arc, bool zip) {
    if (!arc) return true;
    constexpr uint64_t kSec = 10000000ull;
    const uint64_t diff = disk > arc ? disk - arc : arc - disk;
    if (diff <= 2 * kSec) return false;
    if (zip) {
        const uint64_t hour = 3600 * kSec;
        if ((diff > hour ? diff - hour : hour - diff) <= 2 * kSec) return false;
    }
    return true;
}

bool DiskNewer(uint64_t disk, uint64_t arc, bool zip) { return TimesDiffer(disk, arc, zip) && (!arc || disk > arc); }

void BuildPlan(Prepared& pr, const fs::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& uo) {
    Plan& p = pr.plan;
    UpdateReport& rep = pr.rep;
    // Names without a charset mark (old zip / tar) are written as they were decoded: every item gets its name again,
    // otherwise the new names (UTF-8) and the old ones (a legacy code page) would mix in one archive.
    const bool renameAll = pr.src->NameEncoding().applicable;
    for (const Entry& e : pr.src->Entries()) {
        Item it;
        it.srcIndex = e.index;
        it.arc = NormArcPath(e.path);
        it.isDir = e.isDir;
        it.size = e.size;
        it.arcMtime = e.mtimeFileTime;
        it.renamed = renameAll;
        p.items.push_back(std::move(it));
    }
    std::vector<fs::path> exclude{archive};
    {
        fs::path t = archive;
        t += L".lycantmp";
        exclude.push_back(t);
    }
    auto drop = [&](const Item& it) { if (it.srcIndex >= 0) p.dropped.push_back(static_cast<uint32_t>(it.srcIndex)); };
    auto findExact = [&](const std::wstring& path) -> int {
        for (size_t i = 0; i < p.items.size(); ++i)
            if (p.items[i].arc == path) return static_cast<int>(i);
        return -1;
    };
    auto exists = [&](const std::wstring& path) {
        for (const Item& it : p.items)
            if (it.arc == path || IsUnder(it.arc, path)) return true;
        return false;
    };

    for (const UpdateOp& op : ops) {
        switch (op.kind) {
            case UpdateOp::Kind::Delete: {
                const std::wstring path = NormArcPath(op.path);
                if (path.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.nothing_to_delete"));
                size_t n = 0;
                for (size_t i = 0; i < p.items.size();) {
                    if (p.items[i].arc == path || IsUnder(p.items[i].arc, path)) {
                        drop(p.items[i]);
                        p.items.erase(p.items.begin() + static_cast<std::ptrdiff_t>(i));
                        ++n;
                    } else {
                        ++i;
                    }
                }
                if (!n) throw Error(E_INVALIDARG, Message::Make(L"core.not_in_archive", path));
                rep.deleted += static_cast<uint32_t>(n);
                p.changed = true;
                break;
            }
            case UpdateOp::Kind::Rename: {
                const std::wstring from = NormArcPath(op.path), to = NormArcPath(op.newPath);
                if (from.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.nothing_to_rename"));
                CheckNewArcPath(to);
                if (from == to) break;
                if (IsUnder(to, from)) throw Error(E_INVALIDARG, Message::Make(L"core.cannot_move_into_itself", from));
                if (exists(to)) throw Error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), Message::Make(L"core.name_taken", to));
                size_t n = 0;
                for (Item& it : p.items) {
                    if (it.arc == from || IsUnder(it.arc, from)) {
                        it.arc = to + it.arc.substr(from.size());
                        if (it.srcIndex >= 0) it.renamed = true;
                        ++n;
                    }
                }
                if (!n) throw Error(E_INVALIDARG, Message::Make(L"core.not_in_archive", from));
                rep.renamed += static_cast<uint32_t>(n);
                p.changed = true;
                break;
            }
            case UpdateOp::Kind::MakeDir: {
                const std::wstring path = NormArcPath(op.path);
                CheckNewArcPath(path);
                if (exists(path)) throw Error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS), Message::Make(L"core.name_taken", path));
                Item it;
                it.arc = path;
                it.isDir = true;
                it.attrib = FILE_ATTRIBUTE_DIRECTORY;
                GetSystemTimeAsFileTime(&it.mtime);
                it.ctime = it.atime = it.mtime;
                p.items.push_back(std::move(it));
                ++rep.dirsCreated;
                p.changed = true;
                break;
            }
            case UpdateOp::Kind::Replace: {
                const std::wstring path = NormArcPath(op.path);
                const int at = findExact(path);
                if (at < 0) throw Error(E_INVALIDARG, Message::Make(L"core.not_in_archive", path));
                if (p.items[static_cast<size_t>(at)].isDir) throw Error(E_INVALIDARG, Message::Make(L"core.is_folder_cannot_replace", path));
                Item fresh = ItemFromDisk(op.file, path);
                if (fresh.isDir) throw Error(E_INVALIDARG, Message::Make(L"core.replacement_is_folder", op.file.wstring()));
                drop(p.items[static_cast<size_t>(at)]);
                p.items.erase(p.items.begin() + at);
                p.newBytes += fresh.size;
                rep.bytesIn += fresh.size;
                p.items.push_back(std::move(fresh));
                ++rep.replaced;
                p.changed = true;
                break;
            }
            case UpdateOp::Kind::Add: {
                const std::wstring dest = NormArcPath(op.path);
                if (!dest.empty()) CheckNewArcPath(dest);
                if (op.inputs.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.nothing_to_add"));
                std::vector<Item> fresh;
                CompressReport crep;
                for (const fs::path& in : op.inputs)
                    CollectInput(in, fresh, crep, exclude, PathMode::Relative, pr.co.skipThumbsDb);
                for (auto& s : crep.skipped) rep.skipped.push_back(s);
                for (auto& s : crep.errors) rep.errors.push_back(s);
                rep.thumbsDbSkipped += crep.thumbsDbSkipped;
                const bool zip = pr.type == ArchiveType::Zip;
                std::set<std::wstring> seen;          // what the folders hold on the disk (Synchronize)
                std::vector<std::wstring> roots;      // the folders that were added: what Synchronize covers
                for (Item& it : fresh) {
                    const bool top = it.arc.find(L'\\') == std::wstring::npos;
                    if (!dest.empty()) it.arc = dest + L"\\" + it.arc;
                    if (uo.mode == UpdateMode::Synchronize) {
                        seen.insert(it.arc);
                        if (top && it.isDir) roots.push_back(it.arc);
                    }
                    const int at = findExact(it.arc);
                    if (at >= 0) {
                        Item& old = p.items[static_cast<size_t>(at)];
                        if (old.isDir != it.isDir)
                            throw Error(HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
                                        Message::Make(old.isDir ? L"core.folder_name_taken" : L"core.file_name_taken", it.arc));
                        if (old.isDir) continue;  // the folder is there already: what goes into it is added below
                        if (uo.mode == UpdateMode::AddReplace && !uo.replaceSameName)
                            throw Error(HRESULT_FROM_WIN32(ERROR_FILE_EXISTS), Message::Make(L"core.file_name_taken", it.arc));
                        if (uo.mode != UpdateMode::AddReplace) {
                            const uint64_t disk = FileTimeValue(it.mtime);
                            const bool take = uo.mode == UpdateMode::Synchronize ? (old.size != it.size || TimesDiffer(disk, old.arcMtime, zip))
                                                                                  : DiskNewer(disk, old.arcMtime, zip);
                            if (!take) { ++rep.unchanged; continue; }
                        }
                        drop(old);
                        p.items.erase(p.items.begin() + at);
                        ++rep.replaced;
                    } else if (uo.mode == UpdateMode::Freshen) {
                        if (!it.isDir) ++rep.notAdded;
                        continue;  // only what is in the archive is brought up to date
                    } else if (it.isDir) {
                        ++rep.dirsCreated;
                    } else {
                        ++rep.added;
                    }
                    if (!it.isDir) {
                        p.newBytes += it.size;
                        rep.bytesIn += it.size;
                    }
                    p.items.push_back(std::move(it));
                    p.changed = true;
                }
                // Synchronize: what the archive holds below the added folders and the disk does not
                if (uo.mode == UpdateMode::Synchronize && !roots.empty()) {
                    if (!crep.errors.empty()) {
                        rep.warnings.push_back(Message::Make(L"core.nothing_deleted_unreadable"));
                    } else {
                        for (size_t i = 0; i < p.items.size();) {
                            const Item& it = p.items[i];
                            bool under = false;
                            for (const std::wstring& r : roots) if (IsUnder(it.arc, r)) { under = true; break; }
                            if (it.srcIndex >= 0 && under && !seen.count(it.arc)) {
                                rep.deletedPaths.push_back(it.arc);
                                ++rep.deleted;
                                drop(it);
                                p.items.erase(p.items.begin() + static_cast<std::ptrdiff_t>(i));
                                p.changed = true;
                            } else {
                                ++i;
                            }
                        }
                    }
                }
                break;
            }
        }
    }
}

bool BlockOf(IInArchive* a, uint32_t index, uint64_t& block) {
    PROPVARIANT v;
    PropVariantInit(&v);
    bool ok = false;
    if (a->GetProperty(index, kpidBlock, &v) == S_OK) {
        if (v.vt == VT_UI4) { block = v.ulVal; ok = true; }
        else if (v.vt == VT_UI8) { block = v.uhVal.QuadPart; ok = true; }
    }
    PropVariantClear(&v);
    return ok;
}

// Opens the archive and works out what the operations do. Throws for a bad operation; nothing is written.
void Prepare(Prepared& pr, const fs::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& uo) {
    std::error_code ec;
    if (!fs::exists(ToLongPath(archive), ec))
        throw Error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), Message::Make(L"core.archive_not_found", archive.wstring()));
    pr.co = uo.compress;
    pr.co.progress = uo.progress;
    OpenOptions oo;
    oo.nameCodepage = uo.nameCodepage;
    if (!pr.co.password.empty()) {
        const std::wstring pw = pr.co.password;
        oo.password = [pw](std::wstring& out) { out = pw; return true; };
    }
    pr.src = ArchiveSource::Open(archive, oo);
    const Capabilities caps = pr.src->Caps();
    if (!caps.canModify) throw Error(E_NOTIMPL, Message::Make(L"core.archive_cannot_modify", pr.src->FormatName()));
    if (caps.encrypted) {
        if (pr.co.password.empty()) throw Error(E_INVALIDARG, Message::Make(L"core.password_needed_to_modify"));
        if (!pr.src->CheckPassword(pr.co.password)) throw Error(Error::kWrongPassword, Message::Make(L"core.wrong_password", archive.wstring()));
    }
    const std::wstring fmt = Lower(pr.src->FormatName());
    pr.type = fmt == L"zip" ? ArchiveType::Zip : fmt == L"7z" ? ArchiveType::SevenZip : ArchiveType::Tar;
    pr.co.type = pr.type;
    pr.co.volumeSize = 0;
    if (!pr.co.password.empty() && pr.type == ArchiveType::Tar) throw Error(E_INVALIDARG, Message::Make(L"core.tar_no_password_short"));
    CheckZipPassword(pr.co);
    // a 7z whose names are encrypted asked for the password while opening: keep it that way
    pr.co.encryptFileNames = pr.type == ArchiveType::SevenZip && !pr.co.password.empty() && pr.src->impl().passwordAsked;
    BuildPlan(pr, archive, ops, uo);
}

}  // namespace

UpdateCost EstimateUpdate(const fs::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& options) {
    Prepared pr;
    Prepare(pr, archive, ops, options);
    UpdateCost c;
    std::error_code ec;
    c.archiveBytes = fs::file_size(ToLongPath(archive), ec);
    c.newBytes = pr.plan.newBytes;
    // Solid 7z: a block that loses an item is unpacked and packed again with whatever stays in it
    if (pr.type == ArchiveType::SevenZip && pr.src->Caps().solid && !pr.plan.dropped.empty()) {
        IInArchive* a = pr.src->impl().arc;
        std::set<uint64_t> hit;
        for (uint32_t d : pr.plan.dropped) {
            uint64_t b = 0;
            if (BlockOf(a, d, b)) hit.insert(b);
        }
        if (!hit.empty()) {
            std::set<uint32_t> gone(pr.plan.dropped.begin(), pr.plan.dropped.end());
            for (const Item& it : pr.plan.items) {
                uint64_t b = 0;
                if (it.srcIndex >= 0 && !gone.count(static_cast<uint32_t>(it.srcIndex)) &&
                    BlockOf(a, static_cast<uint32_t>(it.srcIndex), b) && hit.count(b))
                    c.recompressBytes += it.size;
            }
        }
    }
    return c;
}

UpdateReport PreviewUpdate(const fs::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& options) {
    Prepared pr;
    Prepare(pr, archive, ops, options);
    pr.rep.changed = pr.plan.changed;
    return pr.rep;
}

UpdateReport UpdateArchive(const fs::path& archive, const std::vector<UpdateOp>& ops, const UpdateOptions& options) {
    Prepared pr;
    Prepare(pr, archive, ops, options);
    UpdateReport& rep = pr.rep;
    if (!pr.plan.changed) return rep;
    rep.changed = true;

    std::error_code ec;
    rep.archiveBefore = fs::file_size(ToLongPath(archive), ec);
    // The new archive is built next to the old one (the swap needs the same volume): room for both
    const fs::path folder = archive.has_parent_path() ? archive.parent_path() : fs::path(L".");
    uint64_t have = 0;
    const uint64_t need = rep.archiveBefore + pr.plan.newBytes + (1u << 20);
    if (ScratchFreeBytes(folder, have) && need > have)
        throw Error(HRESULT_FROM_WIN32(ERROR_DISK_FULL), Message::Make(L"core.disk_full_modify", need >> 20, have >> 20));

    fs::path tmp = archive;
    tmp += L".lycantmp";
    HANDLE h = CreateFileW(ToLongPath(tmp).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error(HRESULT_FROM_WIN32(GetLastError()), Message::Make(L"core.cannot_create_temp_file", tmp.wstring()));
    CMyComPtr<IOutStream> out(new FileWriteStream(h));  // takes the handle
    CompressReport crep;
    try {
        UpdateSmart(FormatNameFor(pr.type), pr.plan.items, out, pr.co, crep, pr.src->impl().arc, rep.archiveBefore);
    } catch (...) {
        out.Release();
        std::error_code e2;
        fs::remove(ToLongPath(tmp), e2);
        throw;
    }
    out.Release();
    for (auto& s : crep.errors) rep.errors.push_back(s);
    for (auto& s : crep.warnings) rep.warnings.push_back(s);
    rep.storedFiles = crep.storedFiles;

    pr.src.reset();  // the old archive has to be closed before it is replaced
    const std::wstring bak = archive.wstring() + L".bak";
    const BOOL swapped = ReplaceFileW(ToLongPath(archive).c_str(), ToLongPath(tmp).c_str(),
                                      options.keepBackup ? ToLongPath(bak).c_str() : nullptr,
                                      REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr);
    if (!swapped) {
        const DWORD err = GetLastError();
        std::error_code e2;
        fs::remove(ToLongPath(tmp), e2);
        throw Error(HRESULT_FROM_WIN32(err), Message::Make(L"core.cannot_replace_archive", archive.wstring()));
    }
    rep.archiveAfter = fs::file_size(ToLongPath(archive), ec);
    return rep;
}

std::vector<std::wstring> MethodsFor(ArchiveType type) {
    switch (type) {
        case ArchiveType::Zip: return {L"Deflate", L"Deflate64", L"BZip2", L"LZMA", L"PPMd", L"Copy"};
        case ArchiveType::SevenZip: return {L"LZMA2", L"LZMA", L"PPMd", L"BZip2", L"Deflate", L"Copy"};
        default: return {};
    }
}

bool MethodHasDict(const std::wstring& m) {
    return _wcsicmp(m.c_str(), L"LZMA") == 0 || _wcsicmp(m.c_str(), L"LZMA2") == 0 || _wcsicmp(m.c_str(), L"PPMd") == 0;
}

bool MethodHasWordSize(const std::wstring& m) {
    return MethodHasDict(m) || _wcsicmp(m.c_str(), L"Deflate") == 0 || _wcsicmp(m.c_str(), L"Deflate64") == 0;
}

}  // namespace lycan
