#include "lycan/scan.h"

#include <windows.h>

#include <algorithm>
#include <memory>

#include "lycan/error.h"
#include "archive_impl.h"

namespace lycan {
namespace {

// ---- AMSI, loaded when it is needed (amsi.dll is part of Windows 10 and later) -----------------------------------------

class Amsi {
public:
    static std::unique_ptr<Amsi> Open(long& hr) {
        std::unique_ptr<Amsi> a(new Amsi());
        a->dll_ = LoadLibraryExW(L"amsi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!a->dll_) { hr = static_cast<long>(HRESULT_FROM_WIN32(GetLastError())); return nullptr; }
        a->init_ = reinterpret_cast<InitFn>(GetProcAddress(a->dll_, "AmsiInitialize"));
        a->open_ = reinterpret_cast<OpenFn>(GetProcAddress(a->dll_, "AmsiOpenSession"));
        a->scan_ = reinterpret_cast<ScanBufferFn>(GetProcAddress(a->dll_, "AmsiScanBuffer"));
        a->closeSession_ = reinterpret_cast<CloseFn>(GetProcAddress(a->dll_, "AmsiCloseSession"));
        a->uninit_ = reinterpret_cast<UninitFn>(GetProcAddress(a->dll_, "AmsiUninitialize"));
        if (!a->init_ || !a->open_ || !a->scan_) { hr = static_cast<long>(E_NOTIMPL); return nullptr; }
        HRESULT r = a->init_(L"LycanZip", &a->ctx_);
        if (FAILED(r)) { hr = static_cast<long>(r); a->ctx_ = nullptr; return nullptr; }
        r = a->open_(a->ctx_, &a->session_);
        if (FAILED(r)) { hr = static_cast<long>(r); a->session_ = nullptr; return nullptr; }
        return a;
    }
    ~Amsi() {
        if (session_ && closeSession_) closeSession_(ctx_, session_);
        if (ctx_ && uninit_) uninit_(ctx_);
        if (dll_) FreeLibrary(dll_);
    }
    Amsi(const Amsi&) = delete;

    // `hr` gets the call's HRESULT when it failed
    ScanVerdict Scan(const std::wstring& name, const void* data, size_t size, long& hr) {
        int result = 0;
        const HRESULT r = scan_(ctx_, const_cast<void*>(data), static_cast<ULONG>(size), name.c_str(), session_, &result);
        if (FAILED(r)) { hr = static_cast<long>(r); return ScanVerdict::Failed; }
        if (result >= 32768) return ScanVerdict::Detected;                  // AMSI_RESULT_DETECTED and above
        if (result >= 0x4000 && result <= 0x4FFF) return ScanVerdict::Blocked;
        return ScanVerdict::Clean;
    }

private:
    Amsi() = default;
    using InitFn = HRESULT(WINAPI*)(LPCWSTR, void**);
    using OpenFn = HRESULT(WINAPI*)(void*, void**);
    using ScanBufferFn = HRESULT(WINAPI*)(void*, PVOID, ULONG, LPCWSTR, void*, int*);
    using CloseFn = void(WINAPI*)(void*, void*);
    using UninitFn = void(WINAPI*)(void*);
    HMODULE dll_ = nullptr;
    void* ctx_ = nullptr;
    void* session_ = nullptr;
    InitFn init_ = nullptr;
    OpenFn open_ = nullptr;
    ScanBufferFn scan_ = nullptr;
    CloseFn closeSession_ = nullptr;
    UninitFn uninit_ = nullptr;
};

ScanVerdict FakeScan(const std::wstring&, const void* data, size_t size) {
    static const char marker[] = "LYCAN-TEST-THREAT";
    const auto* p = static_cast<const char*>(data);
    return std::search(p, p + size, marker, marker + sizeof(marker) - 1) != p + size ? ScanVerdict::Detected : ScanVerdict::Clean;
}

// ---- one file in memory ---------------------------------------------------------------------------------------------------

class MemOutStream final : public ISequentialOutStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)
    Z7_IFACE_COM7_IMP(ISequentialOutStream)
public:
    MemOutStream(std::vector<uint8_t>& buf, uint64_t cap) : buf_(buf), cap_(cap) {}
private:
    std::vector<uint8_t>& buf_;
    uint64_t cap_;
};

Z7_COM7F_IMF(MemOutStream::Write(const void* data, UInt32 size, UInt32* processed)) {
    if (processed) *processed = 0;
    if (buf_.size() + size > cap_) return E_OUTOFMEMORY;  // an archive that says it is smaller than it is
    try {
        const auto* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + size);
    } catch (...) {
        return E_OUTOFMEMORY;
    }
    if (processed) *processed = size;
    return S_OK;
}

class ScanTarget final : public ExtractTarget {
public:
    ScanTarget(ArchiveSource::Impl& impl, const ScanOptions& o, const ScanFn& fn, ScanReport& rep, std::wstring archiveName)
        : impl_(impl), o_(o), fn_(fn), rep_(rep), name_(std::move(archiveName)), hasPassword_(static_cast<bool>(o.password)) {}

    CMyComPtr<ISequentialOutStream> Begin(uint32_t index) override {
        cur_ = kNone;
        if (index >= impl_.entries.size()) return nullptr;
        const Entry& e = impl_.entries[index];
        if (e.isDir || e.isSymlink || e.isHardLink || e.size == 0) return nullptr;
        if (e.isEncrypted && !hasPassword_) {
            ++rep_.skippedEncrypted;
            Note(ScanNote::Kind::Encrypted, e.path);
            return nullptr;
        }
        if (e.size > o_.maxFileBytes) {
            ++rep_.skippedLarge;
            Note(ScanNote::Kind::TooLarge, e.path);
            return nullptr;
        }
        buf_.clear();
        buf_.reserve(static_cast<size_t>(e.size));
        cur_ = index;
        return new MemOutStream(buf_, o_.maxFileBytes);
    }

    void End(uint32_t index, int opResult) override {
        if (cur_ != index) return;  // it was skipped in Begin
        cur_ = kNone;
        const Entry& e = impl_.entries[index];
        if (!OpResultUsable(opResult)) {
            ++rep_.unreadable;
            Note(ScanNote::Kind::Unreadable, e.path);
        } else {
            std::wstring name = name_ + L"\\" + e.path;
            std::replace(name.begin(), name.end(), L'/', L'\\');
            const ScanVerdict v = fn_(name, buf_.data(), buf_.size());
            if (v == ScanVerdict::Failed) {
                ++rep_.scanErrors;
                Note(ScanNote::Kind::ScanFailed, e.path);
            } else {
                ++rep_.files;
                rep_.bytes += buf_.size();
                if (v != ScanVerdict::Clean) rep_.threats.push_back({e.path, v == ScanVerdict::Blocked});
            }
        }
        std::vector<uint8_t>().swap(buf_);  // give the memory back before the next file
    }

private:
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    void Note(ScanNote::Kind k, const std::wstring& path) {
        if (rep_.notes.size() < kMaxScanNotes) rep_.notes.push_back({k, path});
    }
    ArchiveSource::Impl& impl_;
    const ScanOptions& o_;
    const ScanFn& fn_;
    ScanReport& rep_;
    std::wstring name_;
    bool hasPassword_;
    uint32_t cur_ = kNone;
    std::vector<uint8_t> buf_;
};

}  // namespace

AmsiProblem ClassifyAmsiError(long hr) {
    if (hr == 0) return AmsiProblem::None;
    if (hr == static_cast<long>(0x80070015L)) return AmsiProblem::RealTimeOff;  // ERROR_NOT_READY
    if (hr == static_cast<long>(0x80070103L)) return AmsiProblem::NoProvider;   // ERROR_NO_MORE_ITEMS
    return AmsiProblem::Unavailable;
}

// Defender logs its detections a moment after AMSI answered; wait (a little) until there is one entry per detected file.
static void NameThreats(ScanReport& rep, const ScanOptions& options, uint64_t since) {
    std::vector<size_t> found;  // the detected files (not the blocked ones: policy blocks are not detections of Defender)
    for (size_t i = 0; i < rep.threats.size(); ++i)
        if (!rep.threats[i].blocked) found.push_back(i);
    if (found.empty()) return;
    std::vector<DefenderDetection> log;
    for (unsigned waited = 0;; waited += 250) {
        try { log = options.detections ? options.detections(since) : DefenderDetectionsSince(since); } catch (...) { log.clear(); }
        if (log.size() >= found.size() || waited >= options.lookupWaitMs) break;
        Sleep(250);
    }
    for (const auto& d : log)
        if (std::find(rep.threatNames.begin(), rep.threatNames.end(), d.name) == rep.threatNames.end()) rep.threatNames.push_back(d.name);
    if (log.size() == found.size()) {  // one entry per file, in the order the files were scanned
        for (size_t i = 0; i < found.size(); ++i) {
            ScanThreat& t = rep.threats[found[i]];
            t.name = log[i].name;
            t.severity = log[i].severity;
            t.category = log[i].category;
        }
    } else if (rep.threatNames.size() == 1 && !log.empty()) {  // many entries, one name: it is the name of every detected file
        for (size_t i : found) {
            rep.threats[i].name = log[0].name;
            rep.threats[i].severity = log[0].severity;
            rep.threats[i].category = log[0].category;
        }
    }
}

ScanReport ScanArchive(ArchiveSource& src, const ScanOptions& options) {
    ScanReport rep;
    ScanFn fn = options.scanner;
    std::unique_ptr<Amsi> amsi;
    long amsiFailure = 0;
    if (!fn) {
        wchar_t fake[8];
        if (GetEnvironmentVariableW(L"LYCAN_TEST_SCAN_FAKE", fake, 8) > 0) {
            fn = FakeScan;
        } else {
            long hr = 0;
            amsi = Amsi::Open(hr);
            if (!amsi) { rep.amsiHr = hr ? hr : static_cast<long>(E_FAIL); return rep; }
            Amsi* raw = amsi.get();
            fn = [raw, &amsiFailure](const std::wstring& name, const void* data, size_t size) {
                long hr = 0;
                const ScanVerdict v = raw->Scan(name, data, size, hr);
                if (v == ScanVerdict::Failed && !amsiFailure) amsiFailure = hr;
                return v;
            };
        }
    }
    const bool realScanner = amsi != nullptr;
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    const uint64_t startedAt = ((static_cast<uint64_t>(nowFt.dwHighDateTime) << 32) | nowFt.dwLowDateTime) - 20000000ull;  // a second of margin
    ScanTarget target(src.impl(), options, fn, rep, src.File().filename().wstring());
    RunExtract(src.impl(), {}, false, target, options.progress, options.password);
    // AMSI refused the very first scans (real-time protection off, no provider): nothing was looked at, say so
    if (amsiFailure && rep.files == 0 && rep.threats.empty()) {
        const AmsiProblem p = ClassifyAmsiError(amsiFailure);
        if (p == AmsiProblem::RealTimeOff || p == AmsiProblem::NoProvider) rep.amsiHr = amsiFailure;
    }
    if (!rep.threats.empty() && (realScanner || options.detections)) NameThreats(rep, options, startedAt);
    return rep;
}

}  // namespace lycan
