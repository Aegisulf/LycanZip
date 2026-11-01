#include <algorithm>
#include <exception>

#include "archive_impl.h"
#include "lycan/encoding.h"
#include "lycan/error.h"
#include "lycan/security.h"
#include "lycan/shellitems.h"
#include "lycan/tempdir.h"
#include "lycan/testhooks.h"

namespace lycan {
namespace {

// ---- Input stream: an IInStream on a Win32 file handle (random access) -----------------

class FileInStream final : public IInStream, public IStreamGetSize, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_2(IInStream, IStreamGetSize)
    Z7_IFACE_COM7_IMP(ISequentialInStream)
    Z7_IFACE_COM7_IMP(IInStream)
    Z7_IFACE_COM7_IMP(IStreamGetSize)
public:
    explicit FileInStream(HANDLE h) : h_(h) {}
    ~FileInStream() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
private:
    HANDLE h_;
};

Z7_COM7F_IMF(FileInStream::Read(void* data, UInt32 size, UInt32* processed)) {
    DWORD n = 0;
    if (processed) *processed = 0;
    if (!ReadFile(h_, data, size, &n, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (processed) *processed = n;
    return S_OK;
}

Z7_COM7F_IMF(FileInStream::Seek(Int64 offset, UInt32 origin, UInt64* newPos)) {
    if (origin > 2) return STG_E_INVALIDFUNCTION;
    LARGE_INTEGER in, out;
    in.QuadPart = offset;
    if (!SetFilePointerEx(h_, in, &out, origin)) return HRESULT_FROM_WIN32(GetLastError());
    if (newPos) *newPos = static_cast<UInt64>(out.QuadPart);
    return S_OK;
}

Z7_COM7F_IMF(FileInStream::GetSize(UInt64* size)) {
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h_, &s)) return HRESULT_FROM_WIN32(GetLastError());
    *size = static_cast<UInt64>(s.QuadPart);
    return S_OK;
}

// ---- Open callback (supplies the password) -------------------------------------------------------

class PasswordHolder {
public:
    // err: where an exception thrown by the callback is parked. It must not travel through 7z.dll's frames
    // (that would terminate the process); the caller rethrows it after the 7-Zip call returns.
    explicit PasswordHolder(PasswordFn fn, bool* askedFlag = nullptr, std::exception_ptr* err = nullptr)
        : fn_(std::move(fn)), flag_(askedFlag), err_(err) {}
    HRESULT Get(BSTR* out) {
        if (flag_) *flag_ = true;
        if (!asked_) {
            asked_ = true;
            try {
                if (!fn_ || !fn_(pw_)) return E_ABORT;
            } catch (...) {
                if (err_) *err_ = std::current_exception();
                return E_ABORT;
            }
            have_ = true;
        }
        if (!have_) return E_ABORT;
        *out = SysAllocStringLen(pw_.data(), static_cast<UINT>(pw_.size()));
        return *out ? S_OK : E_OUTOFMEMORY;
    }
    ~PasswordHolder() { SecureZeroMemory(pw_.data(), pw_.size() * sizeof(wchar_t)); }
private:
    PasswordFn fn_;
    bool* flag_;
    std::exception_ptr* err_;
    std::wstring pw_;
    bool asked_ = false, have_ = false;
};

// Open callback: supplies the password, and through IArchiveOpenVolumeCallback lets split volumes (.7z.001, .part1.rar, .z01 ...) find the other volumes in the same folder.
class OpenCb final : public IArchiveOpenCallback,
                     public IArchiveOpenVolumeCallback,
                     public ICryptoGetTextPassword,
                     public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_3(IArchiveOpenCallback, IArchiveOpenVolumeCallback, ICryptoGetTextPassword)
    Z7_IFACE_COM7_IMP(IArchiveOpenCallback)
    Z7_IFACE_COM7_IMP(IArchiveOpenVolumeCallback)
    Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
public:
    OpenCb(PasswordFn fn, std::filesystem::path file, bool* askedFlag, std::exception_ptr* err)
        : pw_(std::move(fn), askedFlag, err), file_(std::move(file)) {}
private:
    PasswordHolder pw_;
    std::filesystem::path file_;
};

Z7_COM7F_IMF(OpenCb::SetTotal(const UInt64*, const UInt64*)) { return S_OK; }
Z7_COM7F_IMF(OpenCb::SetCompleted(const UInt64*, const UInt64*)) { return S_OK; }
Z7_COM7F_IMF(OpenCb::CryptoGetTextPassword(BSTR* password)) { return pw_.Get(password); }

Z7_COM7F_IMF(OpenCb::GetProperty(PROPID id, PROPVARIANT* value)) {
    PropVariantInit(value);
    if (id == kpidName) {
        auto name = file_.filename().wstring();
        value->vt = VT_BSTR;
        value->bstrVal = SysAllocStringLen(name.data(), static_cast<UINT>(name.size()));
        return value->bstrVal ? S_OK : E_OUTOFMEMORY;
    }
    if (id == kpidIsDir) {
        value->vt = VT_BOOL;
        value->boolVal = VARIANT_FALSE;
    }
    return S_OK;
}

Z7_COM7F_IMF(OpenCb::GetStream(const wchar_t* name, IInStream** stream)) {
    *stream = nullptr;
    std::filesystem::path p = file_.parent_path() / name;
    HANDLE h = CreateFileW(ToLongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return S_FALSE;  // that volume does not exist
    CMyComPtr<IInStream> s(new FileInStream(h));
    *stream = s.Detach();
    return S_OK;
}

// ---- Property reading helpers -------------------------------------------------------------

struct Prop {
    PROPVARIANT v;
    Prop() { PropVariantInit(&v); }
    ~Prop() { PropVariantClear(&v); }
    Prop(const Prop&) = delete;
    uint64_t U64(uint64_t dflt = 0) const {
        switch (v.vt) {
            case VT_UI1: return v.bVal;
            case VT_UI2: return v.uiVal;
            case VT_UI4: return v.ulVal;
            case VT_UI8: return v.uhVal.QuadPart;
            case VT_I4: return static_cast<uint64_t>(v.lVal);
            case VT_I8: return static_cast<uint64_t>(v.hVal.QuadPart);
            default: return dflt;
        }
    }
    bool Bool() const { return v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE; }
    uint64_t FileTime() const {
        return v.vt == VT_FILETIME
                   ? (static_cast<uint64_t>(v.filetime.dwHighDateTime) << 32) | v.filetime.dwLowDateTime
                   : 0;
    }
    std::wstring Str() const {
        return v.vt == VT_BSTR && v.bstrVal ? std::wstring(v.bstrVal, SysStringLen(v.bstrVal)) : std::wstring();
    }
};

bool IsUnixSymlink(uint32_t attr) {
    constexpr uint32_t kUnixExt = 0x8000;
    return (attr & kUnixExt) && (((attr >> 16) & 0170000) == 0120000);
}

std::wstring DefaultSingleName(const std::filesystem::path& archive) {
    // foo.txt.gz -> foo.txt; without an extension the file name is kept
    auto stem = archive.stem().wstring();
    return stem.empty() ? archive.filename().wstring() : stem;
}

void LoadEntries(ArchiveSource::Impl& impl) {
    UInt32 n = 0;
    if (impl.arc->GetNumberOfItems(&n) != S_OK) throw Error(E_FAIL, Message::Make(L"core.get_items_failed"));
    impl.entries.reserve(n);
    for (UInt32 i = 0; i < n; ++i) {
        Entry e;
        e.index = i;
        Prop p;
        impl.arc->GetProperty(i, kpidPath, &p.v);
        e.path = p.Str();
        if (e.path.empty()) e.path = DefaultSingleName(impl.file);
        std::replace(e.path.begin(), e.path.end(), L'\\', L'/');
        Prop dir, size, pack, attr, mtime, enc;
        impl.arc->GetProperty(i, kpidIsDir, &dir.v);
        impl.arc->GetProperty(i, kpidSize, &size.v);
        impl.arc->GetProperty(i, kpidPackSize, &pack.v);
        impl.arc->GetProperty(i, kpidAttrib, &attr.v);
        impl.arc->GetProperty(i, kpidMTime, &mtime.v);
        impl.arc->GetProperty(i, kpidEncrypted, &enc.v);
        // tar keeps the Unix mode in kpidPosixAttrib (kpidAttrib has no file-type bits) and the link target in
        // kpidSymLink / kpidHardLink; zip with Unix extra data carries the mode inside kpidAttrib.
        Prop crcProp;
        impl.arc->GetProperty(i, kpidCRC, &crcProp.v);
        Prop posix, symTarget, hardTarget, methodProp;
        impl.arc->GetProperty(i, kpidPosixAttrib, &posix.v);
        impl.arc->GetProperty(i, kpidSymLink, &symTarget.v);
        impl.arc->GetProperty(i, kpidHardLink, &hardTarget.v);
        e.isDir = dir.Bool();
        e.size = size.U64();
        e.packedSize = pack.U64();
        e.attributes = static_cast<uint32_t>(attr.U64());
        e.mtimeFileTime = mtime.FileTime();
        e.isEncrypted = enc.Bool();
        e.isSymlink = (e.attributes & FILE_ATTRIBUTE_REPARSE_POINT) || IsUnixSymlink(e.attributes) ||
                      (posix.v.vt != VT_EMPTY && (static_cast<uint32_t>(posix.U64()) & 0170000) == 0120000) || !symTarget.Str().empty();
        e.isHardLink = !hardTarget.Str().empty();
        if (crcProp.v.vt == VT_UI4 && !e.isDir) { e.hasCrc = true; e.crc = crcProp.v.ulVal; }
        if (!e.isDir && impl.arc->GetProperty(i, kpidMethod, &methodProp.v) == S_OK) {
            const std::wstring m = methodProp.Str();
            if (!m.empty()) {
                auto it = std::find(impl.methodNames.begin(), impl.methodNames.end(), m);
                if (it != impl.methodNames.end()) e.methodIdx = static_cast<uint16_t>(it - impl.methodNames.begin());
                else if (impl.methodNames.size() < 4000) { e.methodIdx = static_cast<uint16_t>(impl.methodNames.size()); impl.methodNames.push_back(m); }
            }
        }
        if (e.isEncrypted) impl.caps.encrypted = true;
        impl.entries.push_back(std::move(e));
    }
    Prop solid;
    if (impl.arc->GetArchiveProperty(kpidSolid, &solid.v) == S_OK && solid.Bool()) {
        impl.caps.solid = true;
        impl.caps.randomAccess = false;
    }
}

// ---- Extract callback -----------------------------------------------------------------

class ExtractCb final : public IArchiveExtractCallback, public ICryptoGetTextPassword, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_2(IArchiveExtractCallback, ICryptoGetTextPassword)
    Z7_IFACE_COM7_IMP(IProgress)
    Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
    Z7_IFACE_COM7_IMP(ICryptoGetTextPassword)
public:
    ExtractCb(ExtractTarget& t, const ProgressFn& p, PasswordFn pw)
        : target_(t), progress_(p), pw_(std::move(pw), nullptr, &error) {}
    std::exception_ptr error;

private:
    ExtractTarget& target_;
    const ProgressFn& progress_;
    PasswordHolder pw_;
    UInt64 total_ = 0;
    UInt32 current_ = 0;
};

Z7_COM7F_IMF(ExtractCb::SetTotal(UInt64 total)) {
    total_ = total;
    return S_OK;
}

Z7_COM7F_IMF(ExtractCb::SetCompleted(const UInt64* done)) {
    try {
        if (done && progress_ && !progress_(*done, total_)) return E_ABORT;
    } catch (...) {
        error = std::current_exception();  // rethrown by RunExtract once 7z.dll has returned
        return E_ABORT;
    }
    return S_OK;
}

Z7_COM7F_IMF(ExtractCb::GetStream(UInt32 index, ISequentialOutStream** out, Int32 askMode)) {
    *out = nullptr;
    current_ = index;
    if (askMode != NArchive::NExtract::NAskMode::kExtract) return S_OK;
    try {
        CMyComPtr<ISequentialOutStream> s = target_.Begin(index);
        if (s) *out = s.Detach();
    } catch (...) {
        error = std::current_exception();
        return E_ABORT;
    }
    return S_OK;
}

Z7_COM7F_IMF(ExtractCb::PrepareOperation(Int32)) { return S_OK; }

Z7_COM7F_IMF(ExtractCb::SetOperationResult(Int32 opRes)) {
    try {
        target_.End(current_, opRes);
    } catch (...) {
        error = std::current_exception();
        return E_ABORT;
    }
    return S_OK;
}

Z7_COM7F_IMF(ExtractCb::CryptoGetTextPassword(BSTR* password)) { return pw_.Get(password); }

// ---- Output stream into a memory sink -------------------------------------------------------

class SinkOutStream final : public ISequentialOutStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)
    Z7_IFACE_COM7_IMP(ISequentialOutStream)
public:
    explicit SinkOutStream(ByteSink& s) : sink_(s) {}
private:
    ByteSink& sink_;
};

Z7_COM7F_IMF(SinkOutStream::Write(const void* data, UInt32 size, UInt32* processed)) {
    try {
        sink_.Write(data, size);
    } catch (const Error& e) {
        if (processed) *processed = 0;
        return static_cast<HRESULT>(e.hr());  // keeps the cause (disk full, ...) for the caller
    } catch (...) {
        if (processed) *processed = 0;
        return E_FAIL;
    }
    if (processed) *processed = size;
    return S_OK;
}

class SingleSinkTarget final : public ExtractTarget {
public:
    SingleSinkTarget(ByteSink& s, uint32_t index) : sink_(s), index_(index) {}
    CMyComPtr<ISequentialOutStream> Begin(uint32_t index) override {
        if (index != index_) return nullptr;
        return new SinkOutStream(sink_);
    }
    void End(uint32_t, int opRes) override { result = opRes; }
    int result = -1;
private:
    ByteSink& sink_;
    uint32_t index_;
};

// Several items, each into the sink that the caller gives for it (ArchiveSource::ReadEntries)
class MultiSinkTarget final : public ExtractTarget {
public:
    MultiSinkTarget(const std::function<ByteSink*(uint32_t)>& begin, const std::function<void(uint32_t, bool)>& end) : begin_(begin), end_(end) {}
    CMyComPtr<ISequentialOutStream> Begin(uint32_t index) override {
        ByteSink* s = begin_(index);
        current_ = s ? index : static_cast<uint32_t>(-1);
        if (!s) return nullptr;
        return new SinkOutStream(*s);
    }
    void End(uint32_t index, int opRes) override {
        if (index == current_) { end_(index, OpResultUsable(opRes)); current_ = static_cast<uint32_t>(-1); }
    }

private:
    const std::function<ByteSink*(uint32_t)>& begin_;
    const std::function<void(uint32_t, bool)>& end_;
    uint32_t current_ = static_cast<uint32_t>(-1);
};

}  // namespace

Message OpResultMessage(int r) {
    using namespace NArchive::NExtract::NOperationResult;
    switch (r) {
        case kOK: return Message::Make(L"core.op_ok");
        case kUnsupportedMethod: return Message::Make(L"core.op_unsupported_method");
        case kDataError: return Message::Make(L"core.op_data_error");
        case kCRCError: return Message::Make(L"core.op_crc_error");
        case kUnavailable: return Message::Make(L"core.op_unavailable");
        case kUnexpectedEnd: return Message::Make(L"core.op_unexpected_end");
        case kDataAfterEnd: return Message::Make(L"core.op_data_after_end");
        case kIsNotArc: return Message::Make(L"core.op_not_archive");
        case kHeadersError: return Message::Make(L"core.op_headers_error");
        case kWrongPassword: return Message::Make(L"core.op_wrong_password");
        default: return Message::Make(L"core.op_unknown");
    }
}

void RunExtract(ArchiveSource::Impl& impl, const std::vector<uint32_t>& indices, bool test,
                ExtractTarget& target, const ProgressFn& progress, const PasswordFn& password) {
    // AddRef/Release are private in the 7-Zip macros, so hold it through an interface pointer
    ExtractCb* raw = new ExtractCb(target, progress, password ? password : impl.password);
    CMyComPtr<IArchiveExtractCallback> cb(raw);
    HRESULT hr;
    if (indices.empty()) {
        hr = impl.arc->Extract(nullptr, static_cast<UInt32>(-1), test ? 1 : 0, cb);
    } else {
        // 7-Zip needs sorted, unique, in-range indices (anything else can crash inside 7z.dll)
        std::vector<uint32_t> sorted;
        for (uint32_t i : indices)
            if (i < impl.entries.size()) sorted.push_back(i);
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        if (sorted.empty()) return;  // every index was invalid: extract nothing (an empty list would mean "all")
        hr = impl.arc->Extract(sorted.data(), static_cast<UInt32>(sorted.size()), test ? 1 : 0, cb);
    }
    if (raw->error) std::rethrow_exception(raw->error);
    if (hr == E_ABORT) throw Error(E_ABORT, Message::Make(L"core.cancelled"));
    if (hr != S_OK) {
        const Error probe(hr, std::string());
        throw Error(hr, probe.diskFull() ? Message::Make(L"core.disk_full_extract") : Message::Make(L"core.extract_failed"));
    }
}

// ---- ArchiveSource -------------------------------------------------------------

ArchiveSource::ArchiveSource() : impl_(std::make_unique<Impl>()) {}
ArchiveSource::~ArchiveSource() {
    if (impl_ && impl_->arc) impl_->arc->Close();
}

namespace {

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return s;
}

// Orders the formats by extension; when onlyFormat is not empty only the format of that name is tried (for the inner layer of a double-layer format).
std::vector<const FormatInfo*> FormatOrder(const std::filesystem::path& file, const std::wstring& onlyFormat) {
    auto& lib = SevenZipLib::Get();
    std::wstring ext = Lower(file.extension().wstring());
    if (!ext.empty()) ext.erase(0, 1);
    std::vector<const FormatInfo*> order;
    auto allowed = [&](const FormatInfo& f) { return onlyFormat.empty() || Lower(f.name) == onlyFormat; };
    // Formats with no signature accept almost any data (Base64 opens every plain-text file as an "archive"), so 7-Zip
    // itself only tries them when the extension matches. Do the same: they are skipped in the "try everything" pass.
    static const wchar_t* const kExtensionOnly[] = {L"base64", L"coff", L"ihex", L"lzma", L"lzma86", L"mbr", L"split", L"hash"};
    auto extensionOnly = [&](const FormatInfo& f) {
        const std::wstring n = Lower(f.name);
        return std::any_of(std::begin(kExtensionOnly), std::end(kExtensionOnly), [&](const wchar_t* x) { return n == x; });
    };
    for (auto& f : lib.Formats())
        if (allowed(f) && std::find(f.extensions.begin(), f.extensions.end(), ext) != f.extensions.end())
            order.push_back(&f);
    for (auto& f : lib.Formats())
        if (allowed(f) && std::find(order.begin(), order.end(), &f) == order.end() && (!onlyFormat.empty() || !extensionOnly(f)))
            order.push_back(&f);
    return order;
}

// Tries to open with each format; on success fills impl.arc / impl.format and returns true.
bool TryOpen(ArchiveSource::Impl& impl, IInStream* stream, const std::filesystem::path& nameForCb,
             const std::filesystem::path& extHint, const OpenOptions& options, const std::wstring& onlyFormat) {
    auto& lib = SevenZipLib::Get();
    const UInt64 maxCheckStart = 1 << 22;
    for (auto* f : FormatOrder(extHint, onlyFormat)) {
        auto arc = lib.CreateInArchive(f->clsid);
        if (!arc) continue;

        // The names in zip / tar have no encoding mark: BEFORE opening, ask the handler to decode them as CP437,
        // so that the strings from GetProperty can be turned back into the original bytes without loss, then handed to the encoding detection (see ResolveNames).
        bool rawCp = false;
        std::wstring lname = Lower(f->name);
        if (lname == L"zip" || lname == L"tar") {
            CMyComPtr<ISetProperties> sp;
            arc.QueryInterface(IID_ISetProperties, &sp);
            if (sp) {
                const wchar_t* pname[] = {L"cp"};
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_UI4;
                v.ulVal = kCpOem437;
                rawCp = sp->SetProperties(pname, &v, 1) == S_OK;
            }
        }

        UInt64 pos;
        stream->Seek(0, STREAM_SEEK_SET, &pos);
        std::exception_ptr callbackError;
        CMyComPtr<IArchiveOpenCallback> cb(new OpenCb(options.password, nameForCb, &impl.passwordAsked, &callbackError));
        HRESULT hr = arc->Open(stream, &maxCheckStart, cb);
        if (callbackError) std::rethrow_exception(callbackError);
        if (hr == E_ABORT) throw Error(E_ABORT, Message::Make(L"core.cancelled_no_password"));
        if (hr != S_OK) continue;
        impl.arc = arc;
        impl.format = f->name;
        impl.rawCp437 = rawCp;
        return true;
    }
    return false;
}

bool IsSingleStreamCompressor(const std::wstring& format) {
    static const wchar_t* names[] = {L"gzip", L"bzip2", L"xz", L"lzma", L"z", L"zstd", L"lz4", L"lz5", L"lizard", L"brotli"};
    auto f = Lower(format);
    return std::any_of(std::begin(names), std::end(names), [&](const wchar_t* n) { return f == n; });
}

// The name or the content says the inner layer is tar: foo.tar.gz / foo.tgz / an inner item named *.tar
bool LooksTarred(const std::filesystem::path& file, const std::vector<Entry>& entries) {
    auto ext = Lower(file.extension().wstring());
    static const wchar_t* tarExt[] = {L".tgz", L".tbz", L".tbz2", L".txz", L".tlz", L".taz", L".tzst"};
    if (std::any_of(std::begin(tarExt), std::end(tarExt), [&](const wchar_t* e) { return ext == e; })) return true;
    if (Lower(file.stem().extension().wstring()) == L".tar") return true;
    return entries.size() == 1 && Lower(std::filesystem::path(entries[0].path).extension().wstring()) == L".tar";
}

class HandleSink final : public ByteSink {
public:
    HandleSink(HANDLE h, std::filesystem::path dir) : h_(h), dir_(std::move(dir)) {}
    void Write(const void* d, size_t n) override {
        auto p = static_cast<const char*>(d);
        while (n) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), w = 0;
            if (TestWriteShouldFail(chunk)) throw FullError();
            if (!WriteFile(h_, p, chunk, &w, nullptr)) {
                const DWORD e = GetLastError();
                if (e == ERROR_DISK_FULL || e == ERROR_HANDLE_DISK_FULL) throw FullError();
                throw Error(HRESULT_FROM_WIN32(e), Message::Make(L"core.temp_write_failed"));
            }
            p += w;
            n -= w;
        }
    }
private:
    // the scratch folder is full: the size needed is not known (the tar is being unpacked), the user is asked where else to go
    Error FullError() {
        uint64_t have = 0;
        ScratchFreeBytes(dir_, have);
        message = MakeTempSpaceMessage(0, have, dir_);  // 7-Zip only hands the HRESULT back: ExpandInnerTar restores the message
        return Error(Error::kTempSpace, message);
    }
public:
    Message message;
private:
    HANDLE h_;
    std::filesystem::path dir_;
};

// Unpacks the content of a single-stream archive (gz/xz/bz2 ...) into a delete-on-close temporary file and opens it again as tar.
// On success it replaces the content of impl; on failure (the content is not tar) impl stays as it was and false is returned.
bool ExpandInnerTar(ArchiveSource::Impl& impl, const OpenOptions& options) {
    HANDLE h = CreateScratchFile(L"lycanzip_", options.tempDir);
    if (h == INVALID_HANDLE_VALUE) return false;
    CMyComPtr<IInStream> backing(new FileInStream(h));  // owns the handle; the temporary file is deleted when it is released

    HandleSink sink(h, ScratchDirectory(options.tempDir));
    SingleSinkTarget target(sink, 0);
    try {
        RunExtract(impl, {0}, false, target, options.progress, options.password);
    } catch (const Error& e) {
        if (e.tempSpace() && !sink.message.empty()) throw Error(Error::kTempSpace, sink.message);
        throw;
    }
    if (!OpResultUsable(target.result)) return false;

    ArchiveSource::Impl inner;
    inner.file = impl.file;
    if (!TryOpen(inner, backing, impl.file, std::filesystem::path(L"x.tar"), options, L"tar")) return false;

    std::wstring outer = impl.format;
    impl.arc->Close();
    impl.arc = inner.arc;
    impl.backing = backing;
    impl.rawCp437 = inner.rawCp437;
    impl.format = L"tar (" + outer + L")";
    impl.entries.clear();
    impl.caps = Capabilities{};
    LoadEntries(impl);
    return true;
}

// Split volumes (.7z.001 etc.): the Split handler joins the volumes into one virtual file; the real archive is inside it and needs one more open.
bool ExpandSplit(ArchiveSource::Impl& impl, const OpenOptions& options) {
    CMyComPtr<IInArchiveGetStream> gs;
    impl.arc.QueryInterface(IID_IInArchiveGetStream, &gs);
    if (!gs) return false;
    CMyComPtr<ISequentialInStream> seq;
    if (gs->GetStream(0, &seq) != S_OK || !seq) return false;
    CMyComPtr<IInStream> joined;
    seq.QueryInterface(IID_IInStream, &joined);
    if (!joined) return false;

    ArchiveSource::Impl inner;
    inner.file = impl.file;
    if (!TryOpen(inner, joined, impl.file, std::filesystem::path(impl.entries[0].path), options, L"")) return false;
    impl.outer = impl.arc;  // keep the volume streams alive
    impl.backing = joined;
    impl.rawCp437 = inner.rawCp437;
    impl.arc = inner.arc;
    impl.format = inner.format + L" (split)";
    impl.entries.clear();
    impl.caps = Capabilities{};
    LoadEntries(impl);
    return true;
}

void NormalizeSlashes(std::wstring& s) { std::replace(s.begin(), s.end(), L'\\', L'/'); }

// Lets the handler decode names without an encoding mark as CP437 (CP437 has a unique character for each of the 256 bytes, so the original bytes come back unchanged),
// collects the raw bytes and hands them to the encoding detection. Only for zip / tar; names of the other formats are Unicode anyway.
void ResolveNames(ArchiveSource::Impl& impl, const OpenOptions& options) {
    impl.nameInfo = {};
    impl.rawNames.clear();
    std::wstring fmt = Lower(impl.format);
    if (fmt.rfind(L"zip", 0) != 0 && fmt.rfind(L"tar", 0) != 0) return;

    if (!impl.rawCp437) return;  // could not set cp=437 before opening (an old 7z.dll): keep the default decoding of the handler

    impl.rawNames.assign(impl.entries.size(), std::string());
    bool any = false;
    for (size_t i = 0; i < impl.entries.size(); ++i) {
        Prop p;
        if (impl.arc->GetProperty(static_cast<UInt32>(i), kpidPath, &p.v) != S_OK) continue;
        std::wstring w = p.Str();
        if (w.empty()) continue;  // a single-stream format keeps the name that was derived
        std::string raw;
        bool hasHigh = TryRecoverRawBytes(w, raw) &&
                       std::any_of(raw.begin(), raw.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; });
        if (hasHigh) {
            impl.rawNames[i] = raw;
            any = true;
        } else {
            NormalizeSlashes(w);
            impl.entries[i].path = w;  // plain ASCII, or Unicode already (UTF-8 flag)
        }
    }
    if (!any) return;

    impl.nameInfo.applicable = true;
    if (options.nameCodepage) {
        impl.nameInfo.codepage = options.nameCodepage;
    } else {
        EncodingGuess g = DetectNameEncoding(impl.rawNames, GetACP());
        impl.nameInfo.codepage = g.codepage ? g.codepage : GetOEMCP();  // not detected: keep the 7-Zip default (OEM) behaviour
        impl.nameInfo.autoDetected = true;
        impl.nameInfo.ambiguous = g.ambiguous;
    }
    for (size_t i = 0; i < impl.entries.size(); ++i) {
        if (impl.rawNames[i].empty()) continue;
        std::wstring w = DecodeName(impl.rawNames[i], impl.nameInfo.codepage);
        NormalizeSlashes(w);
        impl.entries[i].path = w;
    }
}

}  // namespace

std::unique_ptr<ArchiveSource> ArchiveSource::Open(const std::filesystem::path& file, const OpenOptions& options) {
    HANDLE h = CreateFileW(ToLongPath(file).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error(HRESULT_FROM_WIN32(GetLastError()), Message::Make(L"core.cannot_open_file", file.wstring()));
    CMyComPtr<IInStream> stream(new FileInStream(h));

    std::unique_ptr<ArchiveSource> src(new ArchiveSource());
    auto& impl = *src->impl_;
    impl.file = file;
    impl.password = options.password;

    if (!TryOpen(impl, stream, file, file, options, L"")) {
        // A handler asked for a password and still cannot open it: the password that was given is wrong (7z / rar with encrypted headers)
        if (impl.passwordAsked && options.password)
            throw Error(Error::kWrongPassword, Message::Make(L"core.wrong_password", file.wstring()));
        throw Error(S_FALSE, Message::Make(L"core.unknown_format", file.wstring()));
    }
    LoadEntries(impl);

    if (Lower(impl.format) == L"split" && impl.entries.size() == 1) ExpandSplit(impl, options);

    if (IsSingleStreamCompressor(impl.format) && impl.entries.size() == 1 && !impl.entries[0].isDir &&
        LooksTarred(file, impl.entries)) {
        ExpandInnerTar(impl, options);  // on failure keep the single-stream view
    }
    ResolveNames(impl, options);
    // What UpdateArchive can change: a plain zip / 7z / tar (not a volume of a set, not a layer inside gz / xz / bz2)
    {
        const std::wstring f = Lower(impl.format);
        Prop isVolume, volumes;
        impl.arc->GetArchiveProperty(kpidIsVolume, &isVolume.v);
        impl.arc->GetArchiveProperty(kpidNumVolumes, &volumes.v);
        impl.caps.canModify = (f == L"zip" || f == L"7z" || f == L"tar") && !impl.outer && !impl.backing && !isVolume.Bool() &&
                              volumes.U64() <= 1;
    }
    return src;
}

bool ArchiveSource::CheckPassword(const std::wstring& password) {
    // Test-read the smallest encrypted file that is not empty: cheapest, and the whole file is unpacked and its CRC checked. An empty
    // file has no data to check: ZipCrypto would then rest on its one check byte alone and let about 1 wrong password in 256 through
    // (too many when saved passwords are tried one after another). Only when every encrypted file is empty is an empty one used.
    const Entry* smallest = nullptr;
    for (auto& e : impl_->entries) {
        if (e.isDir || !e.isEncrypted) continue;
        const bool better = !smallest || (smallest->size == 0 && e.size > 0) || (e.size > 0 && e.size < smallest->size);
        if (better) smallest = &e;
    }
    if (!smallest) return true;  // no encrypted file (or the encryption is at the header level and was checked while opening)

    struct NullSink final : ByteSink {
        void Write(const void*, size_t) override {}
    } sink;
    PasswordFn saved = impl_->password;
    impl_->password = [password](std::wstring& out) { out = password; return true; };
    bool ok = true;
    try {
        ReadEntry(smallest->index, sink);
    } catch (const Error&) {
        ok = false;
    }
    impl_->password = saved;
    return ok;
}

ArchiveSource::EntryTech ArchiveSource::EntryTechInfo(uint32_t index) const {
    EntryTech t;
    auto& arc = impl_->arc;
    if (!arc || index >= impl_->entries.size()) return t;
    auto get = [&](PROPID id, Prop& p) { return arc->GetProperty(index, id, &p.v) == S_OK && p.v.vt != VT_EMPTY; };
    Prop ct, at, method, comment, host, characts, posix, attr, offset, block, link;
    if (get(kpidCTime, ct)) t.ctimeFileTime = ct.FileTime();
    if (get(kpidATime, at)) t.atimeFileTime = at.FileTime();
    if (get(kpidMethod, method)) t.method = method.Str();
    if (get(kpidComment, comment)) t.comment = comment.Str();
    if (get(kpidHostOS, host)) t.hostOs = host.Str();
    if (get(kpidCharacts, characts)) t.characteristics = characts.Str();
    if (get(kpidSymLink, link)) t.linkTarget = link.Str();
    if (get(kpidPosixAttrib, posix)) {
        t.hasUnixMode = true;
        t.unixMode = static_cast<uint32_t>(posix.U64()) & 07777;
    } else if (get(kpidAttrib, attr) && (attr.U64() & 0x8000)) {   // FILE_ATTRIBUTE_UNIX_EXTENSION: the mode is in the high 16 bits (zip with Unix extra data)
        t.hasUnixMode = true;
        t.unixMode = static_cast<uint32_t>(attr.U64() >> 16) & 07777;
    }
    if (get(kpidOffset, offset)) { t.hasOffset = true; t.offset = offset.U64(); }
    if (get(kpidBlock, block)) { t.hasBlock = true; t.block = static_cast<uint32_t>(block.U64()); }
    return t;
}

std::wstring ArchiveSource::MethodName(uint16_t index) const { return index < impl_->methodNames.size() ? impl_->methodNames[index] : std::wstring(); }

std::vector<std::wstring> ArchiveSource::MethodNames() const { return impl_->methodNames; }

ArchiveDetails ArchiveSource::Details() const {
    ArchiveDetails d;
    auto& arc = impl_->arc;
    if (!arc) return d;
    auto str = [&](PROPID id) {
        Prop p;
        return arc->GetArchiveProperty(id, &p.v) == S_OK ? p.Str() : std::wstring();
    };
    auto num = [&](PROPID id) {
        Prop p;
        return arc->GetArchiveProperty(id, &p.v) == S_OK ? p.U64() : 0ull;
    };
    d.method = str(kpidMethod);
    d.comment = str(kpidComment);
    d.physicalSize = num(kpidPhySize);
    d.headersSize = num(kpidHeadersSize);
    d.blocks = static_cast<uint32_t>(num(kpidNumBlocks));
    d.volumes = static_cast<uint32_t>(num(kpidNumVolumes));
    if (d.method.empty()) {
        // zip tells the method of every item, not of the archive: the different ones, in the order they appear
        std::vector<std::wstring> seen;
        for (size_t i = 0; i < impl_->entries.size() && i < 20000 && seen.size() < 6; ++i) {
            if (impl_->entries[i].isDir) continue;
            Prop p;
            if (arc->GetProperty(static_cast<UInt32>(i), kpidMethod, &p.v) != S_OK) continue;
            const std::wstring m = p.Str();
            if (!m.empty() && std::find(seen.begin(), seen.end(), m) == seen.end()) seen.push_back(m);
        }
        for (auto const& m : seen) d.method += (d.method.empty() ? L"" : L" / ") + m;
    }
    return d;
}

NameEncodingInfo ArchiveSource::NameEncoding() const { return impl_->nameInfo; }

void ArchiveSource::SetNameCodepage(unsigned codepage) {
    if (!impl_->nameInfo.applicable || !codepage) return;
    impl_->nameInfo.codepage = codepage;
    impl_->nameInfo.autoDetected = false;
    impl_->nameInfo.ambiguous = false;
    for (size_t i = 0; i < impl_->entries.size(); ++i) {
        if (impl_->rawNames[i].empty()) continue;
        std::wstring w = DecodeName(impl_->rawNames[i], codepage);
        std::replace(w.begin(), w.end(), L'\\', L'/');
        impl_->entries[i].path = w;
    }
}

std::filesystem::path SevenZipDllPath() { return SevenZipLib::Get().DllPath(); }

std::wstring ArchiveBaseName(const std::filesystem::path& file) {
    return ArchiveBaseNameOf(file.filename().wstring());  // shared with the context menu (shellitems.h)
}

std::wstring ArchiveSource::FormatName() const { return impl_->format; }
Capabilities ArchiveSource::Caps() const { return impl_->caps; }
const std::vector<Entry>& ArchiveSource::Entries() const { return impl_->entries; }
const std::filesystem::path& ArchiveSource::File() const { return impl_->file; }

void ArchiveSource::ReadEntries(const std::vector<uint32_t>& indices, const std::function<ByteSink*(uint32_t)>& begin,
                                const std::function<void(uint32_t, bool)>& end, const ProgressFn& progress) {
    if (indices.empty()) return;
    for (uint32_t i : indices)
        if (i >= impl_->entries.size()) throw Error(E_INVALIDARG, Message::Make(L"core.item_index_out_of_range"));
    MultiSinkTarget target(begin, end);
    RunExtract(*impl_, indices, false, target, progress, {});
}

void ArchiveSource::ReadEntry(uint32_t index, ByteSink& sink, const ProgressFn& progress) {
    if (index >= impl_->entries.size()) throw Error(E_INVALIDARG, Message::Make(L"core.item_index_out_of_range"));
    SingleSinkTarget target(sink, index);
    RunExtract(*impl_, {index}, false, target, progress, {});
    if (!OpResultUsable(target.result))
        throw Error(E_FAIL, Message::Make(L"core.read_item_failed", OpResultMessage(target.result)));
}

}  // namespace lycan
