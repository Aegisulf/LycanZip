#include "pch.h"
#include "DragOut.h"

#include <objidl.h>
#include <ole2.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <chrono>
#include <cwctype>
#include <set>
#include <map>
#include <mutex>
#include <thread>

#include "lycan/diag.h"
#include "lycan/error.h"
#include "lycan/i18n.h"
#include "lycan/junk.h"
#include "lycan/security.h"

namespace lycan_app {
namespace {


constexpr size_t kMaxQueued = 8u << 20;  // decompressed bytes waiting for Explorer, per file
constexpr size_t kChunk = 1u << 20;
constexpr ULONGLONG kIdleMs = 3000;  // a started stream that was not read for this long does not hold the pass back any more

// ---------------------------------------------------------------------------------------------------------------------
// One file's data on its way from the decompression thread to the stream Explorer reads

// A piece of the data: in memory, or (when nobody reads this file yet and the buffer is full) in the scratch file of the channel
struct Chunk {
    std::vector<uint8_t> mem;
    bool spilled = false;
    uint64_t offset = 0, length = 0;   // of a spilled piece, in the scratch file
};

struct Channel {
    std::mutex m;
    std::condition_variable space;           // the producer waits here while the buffer is full
    std::deque<Chunk> chunks;
    size_t queued = 0;                       // bytes in memory
    bool done = false, failed = false;
    std::atomic<bool> cancel{false};
    std::atomic<bool> started{false};        // a stream reads this file (until then a full buffer does not hold the pass back: it goes to disk)
    std::atomic<ULONGLONG> lastRead{0};      // GetTickCount64 of the last read of the stream
    HANDLE spill = INVALID_HANDLE_VALUE;     // scratch file (deleted when it is closed)
    uint64_t spillSize = 0;
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // something was queued or the file ended
    ~Channel() {
        if (ready) CloseHandle(ready);
        if (spill != INVALID_HANDLE_VALUE) CloseHandle(spill);
    }
};

// Appends to the scratch file of the channel (the lock of the channel is held)
bool SpillWrite(Channel& ch, const uint8_t* p, size_t n, Chunk& out) {
    if (ch.spill == INVALID_HANDLE_VALUE) {
        wchar_t dir[MAX_PATH + 2], name[MAX_PATH + 2];
        if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"lzd", 0, name)) return false;
        ch.spill = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (ch.spill == INVALID_HANDLE_VALUE) return false;
    }
    OVERLAPPED ov{};
    ov.Offset = static_cast<DWORD>(ch.spillSize & 0xFFFFFFFFu);
    ov.OffsetHigh = static_cast<DWORD>(ch.spillSize >> 32);
    DWORD w = 0;
    if (!WriteFile(ch.spill, p, static_cast<DWORD>(n), &w, &ov) || w != n) return false;
    out.spilled = true;
    out.offset = ch.spillSize;
    out.length = n;
    ch.spillSize += n;
    return true;
}

class ChannelSink final : public lycan::ByteSink {
public:
    explicit ChannelSink(std::shared_ptr<Channel> ch) : ch_(std::move(ch)) {}
    void Write(const void* data, size_t size) override {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (size > 0) {
            const size_t n = (std::min)(size, kChunk);
            std::unique_lock<std::mutex> lk(ch_->m);
            // A full buffer holds the pass back only while Explorer is reading this file. When nobody reads it (the stream of this file was
            // not asked for yet, or Explorer reads another file for a while), the data goes to disk: the pass must go on, or a reader of a
            // later file would wait for ever.
            bool toDisk = false;
            while (ch_->queued >= kMaxQueued && !ch_->cancel.load()) {
                if (!ch_->started.load() || GetTickCount64() - ch_->lastRead.load() > kIdleMs) { toDisk = true; break; }
                ch_->space.wait_for(lk, std::chrono::milliseconds(250));
            }
            if (ch_->cancel.load()) return;  // nobody reads any more: let the extraction run out quickly
            Chunk c;
            if (toDisk && SpillWrite(*ch_, p, n, c)) {
                ch_->chunks.push_back(std::move(c));
            } else {
                c.mem.assign(p, p + n);
                ch_->queued += n;
                ch_->chunks.push_back(std::move(c));
            }
            lk.unlock();
            SetEvent(ch_->ready);
            p += n;
            size -= n;
        }
    }

private:
    std::shared_ptr<Channel> ch_;
};

// A decompression thread with its own opened archive (an ArchiveSource is not thread safe, and opening a big archive takes
// time, so a thread is reused for the next file instead of opening the archive again)
class Worker : public std::enable_shared_from_this<Worker> {
public:
    Worker(std::filesystem::path archive, lycan::OpenOptions open) : archive_(std::move(archive)), open_(std::move(open)) {}

    // Hands a file to this worker; false when it is busy (or told to quit)
    bool Assign(std::shared_ptr<Channel> ch, uint32_t index) {
        std::lock_guard<std::mutex> lk(m_);
        if (busy_ || quit_) return false;
        busy_ = true;
        ch_ = std::move(ch);
        index_ = index;
        if (!started_) {
            started_ = true;
            std::thread([self = shared_from_this()] { self->Loop(); }).detach();
        }
        cv_.notify_one();
        return true;
    }
    void Quit() {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
        cv_.notify_one();
    }

private:
    void Loop() {
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [&] { return ch_ || quit_; });
            if (!ch_) return;  // told to quit while idle
            auto ch = std::move(ch_);
            const uint32_t index = index_;
            lk.unlock();
            Run(ch, index);
            lk.lock();
            busy_ = false;
            if (quit_) return;
        }
    }
    void Run(const std::shared_ptr<Channel>& ch, uint32_t index) {
        try {
            if (!src_) src_ = lycan::ArchiveSource::Open(archive_, open_);
            ChannelSink sink(ch);
            src_->ReadEntry(index, sink, [ch](uint64_t, uint64_t) { return !ch->cancel.load(); });
        } catch (...) {
            std::lock_guard<std::mutex> lk(ch->m);
            ch->failed = true;
            src_.reset();  // open it afresh for the next file
        }
        {
            std::lock_guard<std::mutex> lk(ch->m);
            ch->done = true;
        }
        SetEvent(ch->ready);
    }

    std::filesystem::path archive_;
    lycan::OpenOptions open_;
    std::mutex m_;
    std::condition_variable cv_;
    std::shared_ptr<Channel> ch_;
    uint32_t index_ = 0;
    bool busy_ = false, quit_ = false, started_ = false;
    std::unique_ptr<lycan::ArchiveSource> src_;  // only touched by the worker thread
};

// The decompression threads of one drag. Explorer reads the files one after the other, so normally a single worker (and a
// single opened archive) serves them all; another one is added only when two files are read at the same time.
class Source {
public:
    Source(std::filesystem::path archive, lycan::OpenOptions open) : archive_(std::move(archive)), open_(std::move(open)) {}
    ~Source() {
        for (auto& w : workers_) w->Quit();
    }
    void Read(std::shared_ptr<Channel> ch, uint32_t index) {
        std::lock_guard<std::mutex> lk(m_);
        for (auto& w : workers_)
            if (w->Assign(ch, index)) return;
        workers_.push_back(std::make_shared<Worker>(archive_, open_));
        workers_.back()->Assign(std::move(ch), index);
    }

private:
    std::filesystem::path archive_;
    lycan::OpenOptions open_;
    std::mutex m_;
    std::vector<std::shared_ptr<Worker>> workers_;
};

// One pass over the archive for several files (a solid archive): one thread with its own opened archive reads all of them, in the order of
// the archive, each into the channel of its file. Explorer reads the files in the same order (the descriptor lists them so), so the data
// is used about when it is made.
class BatchSource : public std::enable_shared_from_this<BatchSource> {
public:
    BatchSource(std::filesystem::path archive, lycan::OpenOptions open, std::vector<uint32_t> indices)
        : archive_(std::move(archive)), open_(std::move(open)), indices_(std::move(indices)) {
        for (uint32_t i : indices_) channels_[i] = std::make_shared<Channel>();
    }
    // The channel of a file for its stream (once; null = it was taken already or is not part of the pass)
    std::shared_ptr<Channel> Take(uint32_t index) {
        std::lock_guard<std::mutex> lk(m_);
        auto it = channels_.find(index);
        if (it == channels_.end() || taken_.count(index)) return nullptr;
        taken_.insert(index);
        if (!started_) {
            started_ = true;
            std::thread([self = shared_from_this()] { self->Run(); }).detach();
        }
        return it->second;
    }
    void Cancel() {
        cancelAll_ = true;
        std::lock_guard<std::mutex> lk(m_);
        for (auto& c : channels_) {
            c.second->cancel = true;
            c.second->space.notify_all();
        }
    }

private:
    static void Finish(const std::shared_ptr<Channel>& ch, bool failed) {
        {
            std::lock_guard<std::mutex> lk(ch->m);
            if (failed) ch->failed = true;
            ch->done = true;
        }
        SetEvent(ch->ready);
    }
    void Run() {
        std::map<uint32_t, std::unique_ptr<ChannelSink>> sinks;
        try {
            auto src = lycan::ArchiveSource::Open(archive_, open_);
            src->ReadEntries(
                indices_,
                [&](uint32_t i) -> lycan::ByteSink* {
                    auto sink = std::make_unique<ChannelSink>(channels_[i]);
                    auto* raw = sink.get();
                    sinks[i] = std::move(sink);
                    return raw;
                },
                [&](uint32_t i, bool ok) { Finish(channels_[i], !ok); },
                [&](uint64_t, uint64_t) { return !cancelAll_.load(); });
        } catch (...) {
        }
        for (auto& c : channels_) {   // what the pass did not finish (failed, cancelled): the streams must not wait for ever
            bool done;
            { std::lock_guard<std::mutex> lk(c.second->m); done = c.second->done; }
            if (!done) Finish(c.second, true);
        }
    }

    std::filesystem::path archive_;
    lycan::OpenOptions open_;
    std::vector<uint32_t> indices_;
    std::map<uint32_t, std::shared_ptr<Channel>> channels_;   // built before the thread starts, only read afterwards
    std::mutex m_;
    std::set<uint32_t> taken_;
    bool started_ = false;
    std::atomic<bool> cancelAll_{false};
};

// Wait for `ev` while the UI thread keeps handling messages (this runs inside a COM call on the UI thread)
void WaitPumping(HANDLE ev) {
    const DWORD r = MsgWaitForMultipleObjectsEx(1, &ev, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (r == WAIT_OBJECT_0 + 1) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(msg.wParam));
                return;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The stream of one file (CFSTR_FILECONTENTS)

struct EntryStream : winrt::implements<EntryStream, ::IStream> {
    EntryStream(std::shared_ptr<Source> src, DragEntry entry, std::shared_ptr<Channel> preset = nullptr)
        : src_(std::move(src)), entry_(std::move(entry)), ch_(std::move(preset)) { fromPass_ = ch_ != nullptr; }
    ~EntryStream() {
        if (ch_) {
            ch_->cancel = true;  // Explorer is done (or gave up): stop decompressing
            ch_->space.notify_all();
        }
    }

    HRESULT __stdcall Read(void* pv, ULONG cb, ULONG* pcbRead) noexcept override {
        ULONG got = 0;
        auto* out = static_cast<uint8_t*>(pv);
        try {
            if (!ch_) {
                ch_ = std::make_shared<Channel>();
                ch_->started = true;
                src_->Read(ch_, entry_.index);
            }
            ch_->started = true;
            ch_->lastRead = GetTickCount64();
            while (got < cb) {
                if (pos_ == cur_.size() && !Fill()) break;
                const size_t n = (std::min)(static_cast<size_t>(cb - got), cur_.size() - pos_);
                memcpy(out + got, cur_.data() + pos_, n);
                pos_ += n;
                got += static_cast<ULONG>(n);
            }
        } catch (...) {
            failed_ = true;
        }
        consumed_ += got;
        if (pcbRead) *pcbRead = got;
        if (failed_ && got == 0) return STG_E_READFAULT;
        return got == cb ? S_OK : S_FALSE;
    }
    HRESULT __stdcall Write(const void*, ULONG, ULONG*) noexcept override { return STG_E_ACCESSDENIED; }

    // Some targets ask for the size with Seek(0, END) and go back with Seek(0, SET): allowed before anything was read
    HRESULT __stdcall Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) noexcept override {
        if (consumed_ != 0) {
            if (origin == STREAM_SEEK_CUR && move.QuadPart == 0) { if (newPos) newPos->QuadPart = consumed_; return S_OK; }
            return STG_E_INVALIDFUNCTION;
        }
        if (origin == STREAM_SEEK_END && move.QuadPart == 0) { if (newPos) newPos->QuadPart = entry_.size; return S_OK; }
        if (move.QuadPart == 0 && origin != STREAM_SEEK_END) { if (newPos) newPos->QuadPart = 0; return S_OK; }
        return STG_E_INVALIDFUNCTION;
    }
    HRESULT __stdcall SetSize(ULARGE_INTEGER) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall Commit(DWORD) noexcept override { return S_OK; }
    HRESULT __stdcall Revert() noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }
    HRESULT __stdcall UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) noexcept override { return STG_E_INVALIDFUNCTION; }
    HRESULT __stdcall Stat(STATSTG* st, DWORD) noexcept override {
        if (!st) return STG_E_INVALIDPOINTER;
        *st = {};
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = entry_.size;
        return S_OK;
    }
    HRESULT __stdcall Clone(IStream** ppstm) noexcept override {
        if (ppstm) *ppstm = nullptr;
        return E_NOTIMPL;
    }

private:
    // The next chunk, waiting for the producer when there is none yet. False at the end (or after a failure).
    bool Fill() {
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(ch_->m);
                if (!ch_->chunks.empty()) {
                    Chunk c = std::move(ch_->chunks.front());
                    ch_->chunks.pop_front();
                    ch_->lastRead = GetTickCount64();
                    if (!c.spilled) {
                        cur_ = std::move(c.mem);
                        ch_->queued -= cur_.size();
                        pos_ = 0;
                        ch_->space.notify_one();
                        return true;
                    }
                    // a piece that went to disk: read it back (the producer goes on meanwhile)
                    const HANDLE h = ch_->spill;
                    lk.unlock();
                    cur_.resize(static_cast<size_t>(c.length));
                    OVERLAPPED ov{};
                    ov.Offset = static_cast<DWORD>(c.offset & 0xFFFFFFFFu);
                    ov.OffsetHigh = static_cast<DWORD>(c.offset >> 32);
                    DWORD got = 0;
                    if (!ReadFile(h, cur_.data(), static_cast<DWORD>(c.length), &got, &ov) || got != c.length) { failed_ = true; cur_.clear(); return false; }
                    pos_ = 0;
                    return true;
                }
                if (ch_->done) {
                    if (ch_->failed) failed_ = true;
                    return false;
                }
            }
            WaitPumping(ch_->ready);
        }
    }

    std::shared_ptr<Source> src_;
    DragEntry entry_;
    std::shared_ptr<Channel> ch_;
    bool fromPass_ = false;
    std::vector<uint8_t> cur_;
    size_t pos_ = 0;
    uint64_t consumed_ = 0;
    bool failed_ = false;
};

// ---------------------------------------------------------------------------------------------------------------------
// The data object of the drag

constexpr wchar_t kZoneSuffix[] = L":Zone.Identifier";
constexpr size_t kMaxDragName = MAX_PATH - (sizeof(kZoneSuffix) / sizeof(wchar_t));

// The Zone.Identifier stream of a file (empty when there is none or it is unusually big)
std::string ReadZoneIdentifier(const std::filesystem::path& file) {
    std::string out;
    HANDLE h = CreateFileW((lycan::ToLongPath(file).wstring() + kZoneSuffix).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= 65536) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)) got = 0;
        out.resize(got);
    }
    CloseHandle(h);
    return out;
}

CLIPFORMAT CfDescriptor() { static const CLIPFORMAT f = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW)); return f; }
CLIPFORMAT CfContents() { static const CLIPFORMAT f = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS)); return f; }
CLIPFORMAT CfPreferred() { static const CLIPFORMAT f = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT)); return f; }

struct DragData : winrt::implements<DragData, ::IDataObject> {
    explicit DragData(VirtualDragRequest req) : req_(std::move(req)) {
        source_ = std::make_shared<Source>(req_.archive, req_.open);
        // The Mark of the Web: a dropped virtual file gets none from Explorer, so each file is followed by a second item
        // "name:Zone.Identifier" with the mark of the archive, which Explorer writes as that stream of the file it just made
        motw_ = ReadZoneIdentifier(req_.archive);
        for (size_t i = 0; i < req_.entries.size(); ++i) {
            items_.push_back({i, false});
            if (!motw_.empty() && !req_.entries[i].isDir) items_.push_back({i, true});
        }
        if (req_.onePass) {
            std::vector<uint32_t> files;
            for (const DragEntry& e : req_.entries)
                if (!e.isDir) files.push_back(e.index);
            batch_ = std::make_shared<BatchSource>(req_.archive, req_.open, std::move(files));
        }
    }

    ~DragData() {
        if (batch_) batch_->Cancel();
        for (auto& s : set_) ReleaseStgMedium(&s.second);
    }

    HRESULT __stdcall GetData(FORMATETC* f, STGMEDIUM* m) noexcept override {
        if (!f || !m) return E_INVALIDARG;
        *m = {};
        if (const HRESULT h = GetSet(f, m); h != DV_E_FORMATETC) return h;   // what the target (or the drag image helper) put into the object
        try {
            if (f->cfFormat == CfDescriptor() && (f->tymed & TYMED_HGLOBAL)) return Descriptor(m);
            if (f->cfFormat == CfPreferred() && (f->tymed & TYMED_HGLOBAL)) return Preferred(m);
            if (f->cfFormat == CfContents() && (f->tymed & TYMED_ISTREAM)) {
                if (f->lindex < 0 || static_cast<size_t>(f->lindex) >= items_.size()) return DV_E_LINDEX;
                const Item& it = items_[f->lindex];
                const DragEntry& entry = req_.entries[it.entry];
                if (entry.isDir) return DV_E_LINDEX;
                if (it.zone) {
                    IStream* z = SHCreateMemStream(reinterpret_cast<const BYTE*>(motw_.data()), static_cast<UINT>(motw_.size()));
                    if (!z) return E_OUTOFMEMORY;
                    m->tymed = TYMED_ISTREAM;
                    m->pstm = z;
                    return S_OK;
                }
                // in a pass the channel of the file is fed already (or soon); a second stream for the same file reads it alone
                auto stream = winrt::make_self<EntryStream>(source_, entry, batch_ ? batch_->Take(entry.index) : nullptr);
                m->tymed = TYMED_ISTREAM;
                m->pstm = stream.as<::IStream>().detach();
                return S_OK;
            }
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        return DV_E_FORMATETC;
    }
    HRESULT __stdcall GetDataHere(FORMATETC*, STGMEDIUM*) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall QueryGetData(FORMATETC* f) noexcept override {
        if (!f) return E_INVALIDARG;
        if ((f->cfFormat == CfDescriptor() || f->cfFormat == CfPreferred()) && (f->tymed & TYMED_HGLOBAL)) return S_OK;
        if (f->cfFormat == CfContents() && (f->tymed & TYMED_ISTREAM)) return S_OK;
        std::lock_guard lk(setM_);
        for (auto& s : set_) if (s.first.cfFormat == f->cfFormat && (f->tymed & s.first.tymed)) return S_OK;
        return DV_E_FORMATETC;
    }
    HRESULT __stdcall GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) noexcept override {
        if (out) out->ptd = nullptr;
        return DATA_S_SAMEFORMATETC;
    }
    // The target writes into the object (the text "Copy to ..." next to the cursor is the drop description it sets here; the drag image helper puts the image
    // here) and reads it back, and it reports what it did (PERFORMEDDROPEFFECT and friends). Global memory only, which is all of those formats.
    HRESULT __stdcall SetData(FORMATETC* f, STGMEDIUM* m, BOOL release) noexcept override {
        if (!f || !m) return E_INVALIDARG;
        if (!((m->tymed == TYMED_HGLOBAL && m->hGlobal) || (m->tymed == TYMED_ISTREAM && m->pstm))) { if (release) ReleaseStgMedium(m); return release ? S_OK : E_NOTIMPL; }
        STGMEDIUM own{};
        if (m->tymed == TYMED_ISTREAM) {   // the drag context of the image helper is a stream
            own.tymed = TYMED_ISTREAM;
            own.pstm = m->pstm;
            own.pstm->AddRef();
            if (release) ReleaseStgMedium(m);
        } else if (release) {
            own = *m;
        } else {
            const SIZE_T n = GlobalSize(m->hGlobal);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
            if (!h) return E_OUTOFMEMORY;
            if (void* src = GlobalLock(m->hGlobal)) { std::memcpy(GlobalLock(h), src, n); GlobalUnlock(h); GlobalUnlock(m->hGlobal); }
            own.tymed = TYMED_HGLOBAL;
            own.hGlobal = h;
        }
        std::lock_guard lk(setM_);
        for (auto& s : set_)
            if (s.first.cfFormat == f->cfFormat) { ReleaseStgMedium(&s.second); s.first = *f; s.second = own; return S_OK; }
        set_.emplace_back(*f, own);
        return S_OK;
    }
    HRESULT __stdcall EnumFormatEtc(DWORD dir, IEnumFORMATETC** e) noexcept override {
        if (!e) return E_INVALIDARG;
        *e = nullptr;
        if (dir != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> fmts = {
            {CfDescriptor(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
            {CfContents(), nullptr, DVASPECT_CONTENT, 0, TYMED_ISTREAM},
            {CfPreferred(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
        };
        {
            std::lock_guard lk(setM_);
            for (auto& s : set_) { FORMATETC f = s.first; f.ptd = nullptr; fmts.push_back(f); }
        }
        return SHCreateStdEnumFmtEtc(static_cast<UINT>(fmts.size()), fmts.data(), e);
    }
    HRESULT __stdcall DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) noexcept override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT __stdcall DUnadvise(DWORD) noexcept override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT __stdcall EnumDAdvise(IEnumSTATDATA**) noexcept override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    // A copy of a format that was set by the other side (DV_E_FORMATETC when there is none)
    HRESULT GetSet(FORMATETC* f, STGMEDIUM* m) {
        std::lock_guard lk(setM_);
        for (auto& s : set_) {
            if (s.first.cfFormat != f->cfFormat) continue;
            if ((f->tymed & TYMED_ISTREAM) && s.second.tymed == TYMED_ISTREAM) {
                IStream* clone = nullptr;
                if (FAILED(s.second.pstm->Clone(&clone)) || !clone) return E_FAIL;
                LARGE_INTEGER zero{};
                clone->Seek(zero, STREAM_SEEK_SET, nullptr);
                m->tymed = TYMED_ISTREAM;
                m->pstm = clone;
                return S_OK;
            }
            if (!(f->tymed & TYMED_HGLOBAL) || s.second.tymed != TYMED_HGLOBAL) continue;
            const SIZE_T n = GlobalSize(s.second.hGlobal);
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n);
            if (!h) return E_OUTOFMEMORY;
            void* src = GlobalLock(s.second.hGlobal);
            if (src) { std::memcpy(GlobalLock(h), src, n); GlobalUnlock(h); GlobalUnlock(s.second.hGlobal); }
            m->tymed = TYMED_HGLOBAL;
            m->hGlobal = h;
            return S_OK;
        }
        return DV_E_FORMATETC;
    }

    HRESULT Descriptor(STGMEDIUM* m) {
        const size_t n = items_.size();
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, sizeof(UINT) + n * sizeof(FILEDESCRIPTORW));
        if (!h) return E_OUTOFMEMORY;
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(h));
        group->cItems = static_cast<UINT>(n);
        for (size_t i = 0; i < n; ++i) {
            const DragEntry& e = req_.entries[items_[i].entry];
            FILEDESCRIPTORW& fd = group->fgd[i];
            if (items_[i].zone) {
                fd.dwFlags = FD_FILESIZE;
                fd.nFileSizeLow = static_cast<DWORD>(motw_.size());
                fd.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
                wcsncpy_s(fd.cFileName, (e.name + kZoneSuffix).c_str(), _TRUNCATE);
                continue;
            }
            fd.dwFlags = FD_ATTRIBUTES | FD_PROGRESSUI;
            if (e.mtimeFileTime) {
                fd.dwFlags |= FD_WRITESTIME;
                fd.ftLastWriteTime.dwLowDateTime = static_cast<DWORD>(e.mtimeFileTime & 0xFFFFFFFFu);
                fd.ftLastWriteTime.dwHighDateTime = static_cast<DWORD>(e.mtimeFileTime >> 32);
            }
            if (e.isDir) {
                fd.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            } else {
                fd.dwFlags |= FD_FILESIZE;
                fd.nFileSizeLow = static_cast<DWORD>(e.size & 0xFFFFFFFFu);
                fd.nFileSizeHigh = static_cast<DWORD>(e.size >> 32);
                const DWORD attrs = e.attributes & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE);
                fd.dwFileAttributes = attrs ? attrs : FILE_ATTRIBUTE_NORMAL;
            }
            wcsncpy_s(fd.cFileName, e.name.c_str(), _TRUNCATE);
        }
        GlobalUnlock(h);
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = h;
        return S_OK;
    }
    static HRESULT Preferred(STGMEDIUM* m) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (!h) return E_OUTOFMEMORY;
        *static_cast<DWORD*>(GlobalLock(h)) = DROPEFFECT_COPY;
        GlobalUnlock(h);
        m->tymed = TYMED_HGLOBAL;
        m->hGlobal = h;
        return S_OK;
    }

    struct Item {
        size_t entry;  // in req_.entries
        bool zone;     // the Zone.Identifier stream of that file
    };
    VirtualDragRequest req_;
    std::vector<Item> items_;   // what the descriptor lists, in its order
    std::string motw_;          // the Zone.Identifier of the archive (empty: it has none)
    std::shared_ptr<BatchSource> batch_;   // set when the drag is one pass over a solid archive
    std::shared_ptr<Source> source_;
    std::mutex setM_;
    std::vector<std::pair<FORMATETC, STGMEDIUM>> set_;
};

// ---------------------------------------------------------------------------------------------------------------------
// The picture that follows the cursor: the icon of the first item and its name, and below it how many items / how big (the way Explorer shows a drag)

HBITMAP MakeDragImage(const VirtualDragRequest& req, SIZE& size, POINT& offset) {
    // the top-level names of the selection
    std::vector<std::wstring> tops;
    bool firstIsDir = false;
    for (const DragEntry& e : req.entries) {
        const size_t slash = e.name.find(L'\\');
        const std::wstring top = slash == std::wstring::npos ? e.name : e.name.substr(0, slash);
        if (std::find(tops.begin(), tops.end(), top) != tops.end()) continue;
        if (tops.empty()) firstIsDir = e.isDir || slash != std::wstring::npos;
        tops.push_back(top);
    }
    if (tops.empty()) return nullptr;
    const double s = std::max(1.0, GetDpiForSystem() / 96.0);
    auto px = [&](int dip) { return static_cast<int>(dip * s + 0.5); };
    const int W = px(272), H = px(60), pad = px(12), icon = px(32);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp || !bits) { if (bmp) DeleteObject(bmp); return nullptr; }
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bmp);

    // dark rounded card
    {
        HBRUSH bg = CreateSolidBrush(RGB(36, 38, 44));
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(70, 74, 86));
        HGDIOBJ oldBrush = SelectObject(dc, bg), oldPen = SelectObject(dc, pen);
        RoundRect(dc, 0, 0, W, H, px(10), px(10));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(bg);
        DeleteObject(pen);
    }
    // the icon of the first item (by its name only: nothing is read from the disk)
    {
        SHFILEINFOW fi{};
        const std::wstring probe = firstIsDir ? tops[0] : tops[0];
        if (SHGetFileInfoW(probe.c_str(), firstIsDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, &fi, sizeof(fi),
                           SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES) && fi.hIcon) {
            DrawIconEx(dc, pad, (H - icon) / 2, fi.hIcon, icon, icon, 0, nullptr, DI_NORMAL);
            DestroyIcon(fi.hIcon);
        }
    }
    // the texts
    {
        LOGFONTW lf{};
        lf.lfHeight = -px(14);
        lf.lfWeight = FW_SEMIBOLD;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        HFONT f1 = CreateFontIndirectW(&lf);
        lf.lfHeight = -px(12);
        lf.lfWeight = FW_NORMAL;
        HFONT f2 = CreateFontIndirectW(&lf);
        SetBkMode(dc, TRANSPARENT);
        const int x = pad * 2 + icon;
        RECT r1{x, px(10), W - pad, px(32)};
        SetTextColor(dc, RGB(245, 246, 250));
        HGDIOBJ oldFont = SelectObject(dc, f1);
        DrawTextW(dc, tops[0].c_str(), -1, &r1, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        std::wstring second;
        if (tops.size() > 1) second = lycan::Tf(L"drag.items", tops.size());
        else if (firstIsDir) second = lycan::Tr(L"drag.folder");
        else { wchar_t buf[64] = {}; StrFormatByteSizeW(static_cast<LONGLONG>(req.totalBytes), buf, static_cast<UINT>(std::size(buf))); second = buf; }
        RECT r2{x, px(33), W - pad, px(52)};
        SetTextColor(dc, RGB(170, 176, 192));
        SelectObject(dc, f2);
        DrawTextW(dc, second.c_str(), -1, &r2, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        DeleteObject(f1);
        DeleteObject(f2);
    }
    GdiFlush();
    SelectObject(dc, old);
    DeleteDC(dc);

    // GDI does not write the alpha channel: inside the rounded card every pixel becomes (almost) opaque, the corners stay transparent (premultiplied)
    {
        auto* p = static_cast<uint32_t*>(bits);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                uint32_t& v = p[y * W + x];
                const bool corner = v == 0;   // RoundRect left the corners at 0 (the DIB starts black); card pixels are never exactly 0 (the fill is not black)
                if (corner) continue;
                const uint32_t a = 242;
                const uint32_t b = (v & 0xFF) * a / 255, g = ((v >> 8) & 0xFF) * a / 255, rr = ((v >> 16) & 0xFF) * a / 255;
                v = (a << 24) | (rr << 16) | (g << 8) | b;
            }
    }
    size = {W, H};
    offset = {px(18), px(18)};
    return bmp;
}

struct DropSource : winrt::implements<DropSource, ::IDropSource> {
    HRESULT __stdcall QueryContinueDrag(BOOL escape, DWORD keys) noexcept override {
        if (escape) return DRAGDROP_S_CANCEL;
        if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
        return S_OK;
    }
    HRESULT __stdcall GiveFeedback(DWORD) noexcept override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

std::wstring StripPrefix(const std::wstring& pre, const std::wstring& path) {
    if (pre.empty()) return path;
    if (path.size() >= pre.size() && path.compare(0, pre.size(), pre) == 0) return path.substr(pre.size());
    if (path.size() + 1 == pre.size() && pre.compare(0, path.size(), path) == 0) return {};  // the prefix folder itself
    return path;
}

}  // namespace

bool PlanVirtualDrag(const std::vector<lycan::Entry>& entries, const std::vector<uint32_t>& indices, const std::wstring& stripPrefix,
                     const std::wstring& wrapName, bool skipMacMetadata, bool randomAccess, VirtualDragRequest& out) {
    out.entries.clear();
    out.totalBytes = 0;
    out.onePass = false;
    size_t files = 0;
    std::wstring wrap;
    if (!wrapName.empty()) {
        const lycan::SafePath w = lycan::SanitizeEntryPath(wrapName);
        if (w.rejected) return false;
        wrap = w.relative.wstring() + L'\\';
    }
    for (uint32_t i : indices) {
        if (i >= entries.size()) continue;
        const lycan::Entry& e = entries[i];
        if (e.isSymlink || e.isHardLink) continue;  // the extraction leaves them out as well
        if (skipMacMetadata && lycan::IsMacMetadataPath(e.path)) continue;
        const std::wstring shown = StripPrefix(stripPrefix, e.path);
        if (shown.empty()) continue;
        const lycan::SafePath sp = lycan::SanitizeEntryPath(shown);
        if (sp.rejected) continue;
        DragEntry d;
        d.index = e.index;
        d.name = sp.relative.wstring();
        std::replace(d.name.begin(), d.name.end(), L'/', L'\\');
        d.name = wrap + d.name;
        if (d.name.size() >= kMaxDragName) return false;  // FILEDESCRIPTORW::cFileName is MAX_PATH long (and the mark of the web needs a name too)
        d.size = e.size;
        d.mtimeFileTime = e.mtimeFileTime;
        d.attributes = e.attributes;
        d.isDir = e.isDir;
        if (!d.isDir) {
            ++files;
            out.totalBytes += d.size;
        }
        out.entries.push_back(std::move(d));
    }
    if (files == 0) return false;
    out.onePass = files > 1 && !randomAccess;
    if (out.onePass)   // Explorer takes the files in the order of the descriptor: the order of the archive, which is the order of the pass
        std::stable_sort(out.entries.begin(), out.entries.end(), [](const DragEntry& x, const DragEntry& y) { return x.index < y.index; });
    // The folders go first, each after the folder that holds it and each only once. Explorer makes the folders of a file (or of a deeper
    // folder) when it gets there, so a folder item that came later would find itself there already: "the destination already has a
    // folder of that name", for every folder. Archives often list a folder after what is in it. The files keep their order.
    std::vector<DragEntry> dirs, rest;
    std::set<std::wstring> seen;
    for (DragEntry& d : out.entries) {
        if (!d.isDir) { rest.push_back(std::move(d)); continue; }
        std::wstring key = d.name;
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (seen.insert(key).second) dirs.push_back(std::move(d));
    }
    auto depth = [](const DragEntry& d) { return std::count(d.name.begin(), d.name.end(), L'\\'); };
    std::stable_sort(dirs.begin(), dirs.end(), [&](const DragEntry& x, const DragEntry& y) { return depth(x) < depth(y); });
    out.entries = std::move(dirs);
    for (DragEntry& d : rest) out.entries.push_back(std::move(d));
    return true;
}

bool PlanSingleFileDrag(uint32_t index, const std::wstring& name, uint64_t size, uint64_t mtimeFileTime, VirtualDragRequest& out) {
    out.entries.clear();
    const lycan::SafePath sp = lycan::SanitizeEntryPath(name);
    if (sp.rejected) return false;
    DragEntry d;
    d.index = index;
    d.name = sp.relative.filename().wstring();  // only the name: the preview shows pictures of every folder of the archive
    if (d.name.empty() || d.name.size() >= kMaxDragName) return false;
    d.size = size;
    d.mtimeFileTime = mtimeFileTime;
    d.attributes = FILE_ATTRIBUTE_NORMAL;
    out.totalBytes = size;
    out.entries.push_back(std::move(d));
    return true;
}

DWORD RunVirtualDrag(VirtualDragRequest request) {
    OleInitialize(nullptr);  // S_FALSE: the UI thread already did it
    // WinUI delivers the mouse as pointer messages (and refuses to turn that off), but the OLE drag loop only reads the
    // legacy mouse messages: it would never see the cursor move or the button go up. A helper thread reads the real state
    // and posts those messages to the UI thread for as long as the drag lasts.
    const DWORD uiThread = GetCurrentThreadId();
    auto stop = std::make_shared<std::atomic<bool>>(false);
    std::thread([uiThread, stop] {
        while (!stop->load()) {
            POINT pt{};
            GetCursorPos(&pt);
            const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            const LPARAM lp = MAKELPARAM(static_cast<WORD>(pt.x), static_cast<WORD>(pt.y));
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) PostThreadMessageW(uiThread, WM_KEYDOWN, VK_ESCAPE, 0);
            PostThreadMessageW(uiThread, down ? WM_MOUSEMOVE : WM_LBUTTONUP, down ? MK_LBUTTON : 0, lp);
            if (!down) break;
            Sleep(15);
        }
    }).detach();
    SIZE imageSize{};
    POINT imageOffset{};
    HBITMAP image = MakeDragImage(request, imageSize, imageOffset);
    auto data = winrt::make_self<DragData>(std::move(request));
    auto source = winrt::make_self<DropSource>();
    // the picture and the text next to the cursor: without them the cursor goes about alone
    winrt::com_ptr<::IDragSourceHelper> helper;
    const HRESULT hrHelper = CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(helper.put()));
    HRESULT hrFlags = E_FAIL, hrInit = E_FAIL;
    if (SUCCEEDED(hrHelper)) {
        if (auto helper2 = helper.try_as<::IDragSourceHelper2>()) hrFlags = helper2->SetFlags(DSH_ALLOWDROPDESCRIPTIONTEXT);
        if (image) {
            SHDRAGIMAGE di{};
            di.sizeDragImage = imageSize;
            di.ptOffset = imageOffset;
            di.hbmpDragImage = image;
            di.crColorKey = CLR_NONE;
            hrInit = helper->InitializeFromBitmap(&di, data.as<::IDataObject>().get());
            if (SUCCEEDED(hrInit)) image = nullptr;  // the helper owns it now
        }
    }
    lycan::diag::Info(L"drag image: helper 0x{:08X}, flags 0x{:08X}, bitmap {}, initialize 0x{:08X}", static_cast<unsigned>(hrHelper), static_cast<unsigned>(hrFlags), image ? L"made" : L"none/owned", static_cast<unsigned>(hrInit));
    if (image) DeleteObject(image);
    DWORD effect = DROPEFFECT_NONE;
    DoDragDrop(data.as<::IDataObject>().get(), source.as<::IDropSource>().get(), DROPEFFECT_COPY, &effect);
    stop->store(true);
    return effect;
}

}  // namespace lycan_app
