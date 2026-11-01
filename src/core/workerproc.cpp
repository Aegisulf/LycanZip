// Worker-process executor: each task runs in its own process (lycanzip-cli.exe --worker <read> <write>).
//
// Transport: two anonymous pipes whose client ends are inherited by the worker (nothing else is inherited, nothing can
// connect from outside, and a worker that dies closes its ends, which the host sees as a broken pipe).
// Protocol: framed messages [u32 length][u8 type][body]. After the host's Start message everything is request / response
// with the worker as the asker: Progress -> Ack (continue or cancel, plus the current priority), password / conflict
// request -> Reply, and at the end the worker sends Result. Because the worker waits for the Ack of a progress report
// (reports are sent at most every 50 ms), the host can pause a task simply by delaying the Ack, exactly like the
// in-process executor blocks inside the progress callback.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "lycan/archive_source.h"
#include "lycan/diag.h"
#include "lycan/error.h"
#include "lycan/taskexec.h"
#include "lycan/tempdir.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

// ------------------------------------------------------------------------------------------------ messages
enum MsgType : uint8_t {
    kStart = 1,        // host -> worker: the TaskSpec
    kAck = 2,          // host -> worker: u8 keepGoing, u8 priority
    kReply = 3,        // host -> worker: answer to a password / conflict request
    kProgress = 10,    // worker -> host: u64 done, u64 total
    kPasswordReq = 11, // worker -> host: u8 which (0 = opening, 1 = extracting)
    kConflictReq = 12, // worker -> host: ConflictInfo
    kResult = 13,      // worker -> host: outcome
    kLargeReq = 14,    // worker -> host: LargeExtractInfo (answer: u8 1 = go on)
};

class Writer {
public:
    void U8(uint8_t v) { b_.push_back(v); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void I32(int32_t v) { Raw(&v, 4); }
    void I64(int64_t v) { Raw(&v, 8); }
    void Bool(bool v) { U8(v ? 1 : 0); }
    void Str(const std::wstring& s) {
        U32(static_cast<uint32_t>(s.size()));
        Raw(s.data(), s.size() * sizeof(wchar_t));
    }
    void StrUtf8(const std::string& s) {
        U32(static_cast<uint32_t>(s.size()));
        Raw(s.data(), s.size());
    }
    void Path(const fs::path& p) { Str(p.wstring()); }
    void Strs(const std::vector<std::wstring>& v) {
        U32(static_cast<uint32_t>(v.size()));
        for (auto& s : v) Str(s);
    }
    // A message travels as id + arguments (+ the plain text when it has no id), so the UI process translates it into its language
    void Msg(const Message& m) {
        Str(m.id);
        Str(m.raw);
        Strs(m.args);
    }
    void Msgs(const std::vector<Message>& v) {
        U32(static_cast<uint32_t>(v.size()));
        for (auto& m : v) Msg(m);
    }
    void Paths(const std::vector<fs::path>& v) {
        U32(static_cast<uint32_t>(v.size()));
        for (auto& p : v) Path(p);
    }
    const std::vector<uint8_t>& Bytes() const { return b_; }

private:
    void Raw(const void* p, size_t n) {
        const auto* c = static_cast<const uint8_t*>(p);
        b_.insert(b_.end(), c, c + n);
    }
    std::vector<uint8_t> b_;
};

class Reader {
public:
    explicit Reader(const std::vector<uint8_t>& b) : b_(b) {}
    uint8_t U8() { uint8_t v; Raw(&v, 1); return v; }
    uint32_t U32() { uint32_t v; Raw(&v, 4); return v; }
    uint64_t U64() { uint64_t v; Raw(&v, 8); return v; }
    int32_t I32() { int32_t v; Raw(&v, 4); return v; }
    int64_t I64() { int64_t v; Raw(&v, 8); return v; }
    bool Bool() { return U8() != 0; }
    std::wstring Str() {
        const uint32_t n = U32();
        if (static_cast<uint64_t>(n) * sizeof(wchar_t) > b_.size() - pos_) throw std::runtime_error("bad message");
        std::wstring s(n, L'\0');
        Raw(s.data(), static_cast<size_t>(n) * sizeof(wchar_t));
        return s;
    }
    std::string StrUtf8() {
        const uint32_t n = U32();
        if (n > b_.size() - pos_) throw std::runtime_error("bad message");
        std::string s(n, '\0');
        Raw(s.data(), n);
        return s;
    }
    fs::path Path() { return fs::path(Str()); }
    std::vector<std::wstring> Strs() {
        const uint32_t n = U32();
        if (n > b_.size()) throw std::runtime_error("bad message");
        std::vector<std::wstring> v;
        for (uint32_t i = 0; i < n; ++i) v.push_back(Str());
        return v;
    }
    Message Msg() {
        Message m;
        m.id = Str();
        m.raw = Str();
        m.args = Strs();
        return m;
    }
    std::vector<Message> Msgs() {
        const uint32_t n = U32();
        if (n > b_.size()) throw std::runtime_error("bad message");
        std::vector<Message> v;
        for (uint32_t i = 0; i < n; ++i) v.push_back(Msg());
        return v;
    }
    std::vector<fs::path> Paths() {
        const uint32_t n = U32();
        if (n > b_.size()) throw std::runtime_error("bad message");
        std::vector<fs::path> v;
        for (uint32_t i = 0; i < n; ++i) v.push_back(Path());
        return v;
    }

private:
    void Raw(void* p, size_t n) {
        if (n > b_.size() - pos_) throw std::runtime_error("bad message");
        std::memcpy(p, b_.data() + pos_, n);
        pos_ += n;
    }
    const std::vector<uint8_t>& b_;
    size_t pos_ = 0;
};

// One direction pair of pipe handles with message framing. Not thread safe by itself (callers serialise).
class Channel {
public:
    Channel(HANDLE in, HANDLE out) : in_(in), out_(out) {}
    bool Send(uint8_t type, const Writer& w) {
        const uint32_t len = static_cast<uint32_t>(w.Bytes().size()) + 1;
        std::vector<uint8_t> frame(4 + len);
        std::memcpy(frame.data(), &len, 4);
        frame[4] = type;
        if (!w.Bytes().empty()) std::memcpy(frame.data() + 5, w.Bytes().data(), w.Bytes().size());
        return WriteAll(frame.data(), frame.size());
    }
    bool Send(uint8_t type) { return Send(type, Writer{}); }
    // false = the other side is gone
    bool Receive(uint8_t& type, std::vector<uint8_t>& body) {
        uint32_t len = 0;
        if (!ReadAll(&len, 4) || len == 0 || len > (256u << 20)) return false;
        std::vector<uint8_t> buf(len);
        if (!ReadAll(buf.data(), len)) return false;
        type = buf[0];
        body.assign(buf.begin() + 1, buf.end());
        return true;
    }

private:
    bool WriteAll(const void* p, size_t n) {
        const auto* c = static_cast<const uint8_t*>(p);
        while (n) {
            DWORD w = 0;
            if (!WriteFile(out_, c, static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), &w, nullptr) || !w) return false;
            c += w;
            n -= w;
        }
        return true;
    }
    bool ReadAll(void* p, size_t n) {
        auto* c = static_cast<uint8_t*>(p);
        while (n) {
            DWORD r = 0;
            if (!ReadFile(in_, c, static_cast<DWORD>(std::min<size_t>(n, 1u << 20)), &r, nullptr) || !r) return false;
            c += r;
            n -= r;
        }
        return true;
    }
    HANDLE in_, out_;
};

// ------------------------------------------------------------------------------------------------ serialisation
void PutExtractReport(Writer& w, const ExtractReport& r) {
    w.U64(r.files); w.U64(r.dirs); w.U64(r.bytes);
    w.Path(r.outputDir);
    w.Msgs(r.rejected); w.Strs(r.renamed); w.Msgs(r.skipped); w.Msgs(r.errors); w.Msgs(r.warnings); w.Msgs(r.kept);
    w.Bool(r.wrongPassword); w.U32(r.macMetadataSkipped);
}
ExtractReport GetExtractReport(Reader& r) {
    ExtractReport e;
    e.files = r.U64(); e.dirs = r.U64(); e.bytes = r.U64();
    e.outputDir = r.Path();
    e.rejected = r.Msgs(); e.renamed = r.Strs(); e.skipped = r.Msgs(); e.errors = r.Msgs(); e.warnings = r.Msgs(); e.kept = r.Msgs();
    e.wrongPassword = r.Bool(); e.macMetadataSkipped = r.U32();
    return e;
}
void PutCompressReport(Writer& w, const CompressReport& c) {
    w.U64(c.files); w.U64(c.dirs); w.U64(c.bytesIn); w.U64(c.bytesOut);
    w.Paths(c.outputs);
    w.Msgs(c.skipped); w.Msgs(c.errors); w.Msgs(c.warnings); w.U32(c.thumbsDbSkipped); w.U32(c.storedFiles);
}
CompressReport GetCompressReport(Reader& r) {
    CompressReport c;
    c.files = r.U64(); c.dirs = r.U64(); c.bytesIn = r.U64(); c.bytesOut = r.U64();
    c.outputs = r.Paths();
    c.skipped = r.Msgs(); c.errors = r.Msgs(); c.warnings = r.Msgs(); c.thumbsDbSkipped = r.U32(); c.storedFiles = r.U32();
    return c;
}

void PutUpdateReport(Writer& w, const UpdateReport& u) {
    w.U32(u.added); w.U32(u.replaced); w.U32(u.deleted); w.U32(u.renamed); w.U32(u.dirsCreated);
    w.U64(u.bytesIn); w.U64(u.archiveBefore); w.U64(u.archiveAfter); w.U32(u.thumbsDbSkipped); w.U32(u.storedFiles);
    w.U32(u.unchanged); w.U32(u.notAdded); w.Strs(u.deletedPaths);
    w.Msgs(u.skipped); w.Msgs(u.errors); w.Msgs(u.warnings); w.Bool(u.changed);
}
UpdateReport GetUpdateReport(Reader& r) {
    UpdateReport u;
    u.added = r.U32(); u.replaced = r.U32(); u.deleted = r.U32(); u.renamed = r.U32(); u.dirsCreated = r.U32();
    u.bytesIn = r.U64(); u.archiveBefore = r.U64(); u.archiveAfter = r.U64(); u.thumbsDbSkipped = r.U32(); u.storedFiles = r.U32();
    u.unchanged = r.U32(); u.notAdded = r.U32(); u.deletedPaths = r.Strs();
    u.skipped = r.Msgs(); u.errors = r.Msgs(); u.warnings = r.Msgs(); u.changed = r.Bool();
    return u;
}

void PutScanReport(Writer& w, const ScanReport& s) {
    w.U64(s.files); w.U64(s.bytes); w.U32(s.skippedLarge); w.U32(s.skippedEncrypted); w.U32(s.unreadable); w.U32(s.scanErrors);
    w.I32(static_cast<int32_t>(s.amsiHr));
    w.U32(static_cast<uint32_t>(s.notes.size()));
    for (auto& n : s.notes) { w.U8(static_cast<uint8_t>(n.kind)); w.Str(n.path); }
    w.U32(static_cast<uint32_t>(s.threats.size()));
    for (auto& t : s.threats) { w.Str(t.path); w.Bool(t.blocked); w.Str(t.name); w.Str(t.severity); w.Str(t.category); }
    w.U32(static_cast<uint32_t>(s.threatNames.size()));
    for (auto& n : s.threatNames) w.Str(n);
}
ScanReport GetScanReport(Reader& r) {
    ScanReport s;
    s.files = r.U64(); s.bytes = r.U64(); s.skippedLarge = r.U32(); s.skippedEncrypted = r.U32(); s.unreadable = r.U32(); s.scanErrors = r.U32();
    s.amsiHr = r.I32();
    uint32_t n = r.U32();
    if (n > (1u << 20)) throw std::runtime_error("bad message");
    for (uint32_t i = 0; i < n; ++i) { ScanNote note; note.kind = static_cast<ScanNote::Kind>(r.U8()); note.path = r.Str(); s.notes.push_back(std::move(note)); }
    n = r.U32();
    if (n > (1u << 24)) throw std::runtime_error("bad message");
    for (uint32_t i = 0; i < n; ++i) { ScanThreat t; t.path = r.Str(); t.blocked = r.Bool(); t.name = r.Str(); t.severity = r.Str(); t.category = r.Str(); s.threats.push_back(std::move(t)); }
    n = r.U32();
    if (n > (1u << 20)) throw std::runtime_error("bad message");
    for (uint32_t i = 0; i < n; ++i) s.threatNames.push_back(r.Str());
    return s;
}

void PutHashReport(Writer& w, const HashReport& h) {
    w.U32(h.algos); w.U64(h.bytes); w.U32(h.failed);
    w.U32(static_cast<uint32_t>(h.files.size()));
    for (auto& f : h.files) {
        w.Str(f.path); w.U64(f.size); w.Msg(f.error);
        w.U32(static_cast<uint32_t>(f.hex.size()));
        for (auto& x : f.hex) w.Str(std::wstring(x.begin(), x.end()));
    }
}
HashReport GetHashReport(Reader& r) {
    HashReport h;
    h.algos = r.U32(); h.bytes = r.U64(); h.failed = r.U32();
    uint32_t n = r.U32();
    if (n > (1u << 24)) throw std::runtime_error("bad message");
    for (uint32_t i = 0; i < n; ++i) {
        HashFileResult f;
        f.path = r.Str(); f.size = r.U64(); f.error = r.Msg();
        const uint32_t k = r.U32();
        if (k > 16) throw std::runtime_error("bad message");
        for (uint32_t j = 0; j < k; ++j) { auto w = r.Str(); f.hex.push_back(std::string(w.begin(), w.end())); };
        h.files.push_back(std::move(f));
    }
    return h;
}

void PutRepackReport(Writer& w, const RepackReport& p) {
    w.U64(p.files); w.U64(p.dirs); w.U64(p.bytes); w.Path(p.output); w.Paths(p.outputs);
    w.U64(p.archiveBefore); w.U64(p.archiveAfter); w.Bool(p.tested);
    w.Msgs(p.skipped); w.Msgs(p.warnings); w.Msgs(p.errors);
}
RepackReport GetRepackReport(Reader& r) {
    RepackReport p;
    p.files = r.U64(); p.dirs = r.U64(); p.bytes = r.U64(); p.output = r.Path(); p.outputs = r.Paths();
    p.archiveBefore = r.U64(); p.archiveAfter = r.U64(); p.tested = r.Bool();
    p.skipped = r.Msgs(); p.warnings = r.Msgs(); p.errors = r.Msgs();
    return p;
}

void PutCompressOptions(Writer& w, const CompressOptions& c) {
    w.U8(static_cast<uint8_t>(c.type)); w.U8(static_cast<uint8_t>(c.profile)); w.I32(c.level);
    w.Str(c.password); w.Bool(c.encryptFileNames); w.U64(c.volumeSize); w.Bool(c.overwrite); w.U32(c.threads);
    w.Str(c.method); w.U64(c.dictSize); w.U32(c.wordSize); w.I64(c.solidSize); w.Str(c.zipEncryption);
    w.U8(static_cast<uint8_t>(c.pathMode)); w.Str(c.customParams); w.Path(c.tempDir); w.Bool(c.skipThumbsDb); w.Bool(c.storeCompressed); w.Str(c.isoLabel); w.Bool(c.isoKeepTimes);
}
void GetCompressOptions(Reader& r, CompressOptions& c) {
    c.type = static_cast<ArchiveType>(r.U8()); c.profile = static_cast<Profile>(r.U8()); c.level = r.I32();
    c.password = r.Str(); c.encryptFileNames = r.Bool(); c.volumeSize = r.U64(); c.overwrite = r.Bool(); c.threads = r.U32();
    c.method = r.Str(); c.dictSize = r.U64(); c.wordSize = r.U32(); c.solidSize = r.I64(); c.zipEncryption = r.Str();
    c.pathMode = static_cast<PathMode>(r.U8()); c.customParams = r.Str(); c.tempDir = r.Path(); c.skipThumbsDb = r.Bool(); c.storeCompressed = r.Bool(); c.isoLabel = r.Str(); c.isoKeepTimes = r.Bool();
}

void PutSpec(Writer& w, const TaskSpec& s, TaskPriority priority) {
    w.U8(static_cast<uint8_t>(s.kind));
    w.Path(s.archive); w.Path(s.dest); w.Path(s.output);
    w.Paths(s.inputs);
    w.U32(static_cast<uint32_t>(s.indices.size()));
    for (uint32_t i : s.indices) w.U32(i);
    w.U8(static_cast<uint8_t>(priority));
    const ExtractOptions& e = s.extractOptions;
    w.U8(static_cast<uint8_t>(e.overwrite));
    w.Bool(e.propagateMotw); w.U64(e.maxTotalBytes); w.U32(e.maxRatio); w.Bool(e.smart);
    w.Str(e.wrapName); w.Str(e.stripPrefix); w.Bool(e.skipMacMetadata); w.Bool(e.keepDamaged);
    w.Bool(static_cast<bool>(e.onConflict));
    w.Bool(static_cast<bool>(e.onLarge)); w.U64(e.largeBytes); w.U32(e.largeRatio); w.U64(e.hugeBytes);
    w.Bool(static_cast<bool>(e.password));
    w.U32(s.openOptions.nameCodepage);
    w.Bool(static_cast<bool>(s.openOptions.password));
    w.Path(s.openOptions.tempDir);
    PutCompressOptions(w, s.compressOptions);
    // update: the operations and the options (the compress options of the update ride along)
    w.U32(static_cast<uint32_t>(s.updateOps.size()));
    for (const UpdateOp& o : s.updateOps) {
        w.U8(static_cast<uint8_t>(o.kind)); w.Str(o.path); w.Str(o.newPath); w.Paths(o.inputs); w.Path(o.file);
    }
    const UpdateOptions& u = s.updateOptions;
    PutCompressOptions(w, u.compress);
    w.Bool(u.keepBackup); w.Bool(u.replaceSameName); w.U32(u.nameCodepage); w.U8(static_cast<uint8_t>(u.mode));
    w.Bool(u.rewriteNames); w.Bool(u.verify);
    w.U32(s.hashAlgos);
    w.Bool(s.repackInPlace); w.Bool(s.repackSkipDamaged); w.Bool(s.repackTest);
}

struct WireSpec {
    TaskSpec spec;
    bool hasConflict = false, hasLarge = false, hasExtractPassword = false, hasOpenPassword = false;
};
WireSpec GetSpec(Reader& r) {
    WireSpec ws;
    TaskSpec& s = ws.spec;
    s.kind = static_cast<TaskKind>(r.U8());
    s.archive = r.Path(); s.dest = r.Path(); s.output = r.Path();
    s.inputs = r.Paths();
    const uint32_t n = r.U32();
    for (uint32_t i = 0; i < n; ++i) s.indices.push_back(r.U32());
    s.priority = static_cast<TaskPriority>(r.U8());
    ExtractOptions& e = s.extractOptions;
    e.overwrite = static_cast<OverwriteMode>(r.U8());
    e.propagateMotw = r.Bool(); e.maxTotalBytes = r.U64(); e.maxRatio = r.U32(); e.smart = r.Bool();
    e.wrapName = r.Str(); e.stripPrefix = r.Str(); e.skipMacMetadata = r.Bool(); e.keepDamaged = r.Bool();
    ws.hasConflict = r.Bool();
    ws.hasLarge = r.Bool(); e.largeBytes = r.U64(); e.largeRatio = r.U32(); e.hugeBytes = r.U64();
    ws.hasExtractPassword = r.Bool();
    s.openOptions.nameCodepage = r.U32();
    ws.hasOpenPassword = r.Bool();
    s.openOptions.tempDir = r.Path();
    GetCompressOptions(r, s.compressOptions);
    const uint32_t nops = r.U32();
    if (nops > (1u << 20)) throw std::runtime_error("bad message");
    for (uint32_t i = 0; i < nops; ++i) {
        UpdateOp o;
        o.kind = static_cast<UpdateOp::Kind>(r.U8());
        o.path = r.Str(); o.newPath = r.Str(); o.inputs = r.Paths(); o.file = r.Path();
        s.updateOps.push_back(std::move(o));
    }
    GetCompressOptions(r, s.updateOptions.compress);
    s.updateOptions.keepBackup = r.Bool(); s.updateOptions.replaceSameName = r.Bool(); s.updateOptions.nameCodepage = r.U32(); s.updateOptions.mode = static_cast<UpdateMode>(r.U8());
    s.updateOptions.rewriteNames = r.Bool(); s.updateOptions.verify = r.Bool();
    s.hashAlgos = r.U32();
    s.repackInPlace = r.Bool(); s.repackSkipDamaged = r.Bool(); s.repackTest = r.Bool();
    return ws;
}

// ------------------------------------------------------------------------------------------------ priority
void SetEfficiencyMode(bool on) {
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    st.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    st.StateMask = on ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);  // Windows 10 1709+; ignored when unsupported
}

// Applied inside the worker (background mode can only be entered by the process itself).
void ApplyPriority(TaskPriority want, TaskPriority& current) {
    if (want == current) return;
    if (current == TaskPriority::Background) {
        SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_END);
        SetEfficiencyMode(false);
    }
    switch (want) {
        case TaskPriority::Normal: SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS); break;
        case TaskPriority::BelowNormal: SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS); break;
        case TaskPriority::Background:
            SetPriorityClass(GetCurrentProcess(), PROCESS_MODE_BACKGROUND_BEGIN);
            SetEfficiencyMode(true);
            break;
    }
    current = want;
}

// ------------------------------------------------------------------------------------------------ host side
class ProcessExecutor final : public TaskExecutor {
public:
    explicit ProcessExecutor(ProcessExecutorOptions o) : opt_(std::move(o)) {
        if (opt_.workerExe.empty()) {
            // the executable of this process (the app, the CLI or a test) sits next to lycanzip-cli.exe
            wchar_t self[MAX_PATH * 2];
            GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
            opt_.workerExe = fs::path(self).parent_path() / L"lycanzip-cli.exe";
        }
        job_ = CreateJobObjectW(nullptr, nullptr);
        if (job_) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
            info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &info, sizeof info);
        }
    }
    ~ProcessExecutor() override { if (job_) CloseHandle(job_); }  // kills the workers that are still running

    TaskResult Execute(const TaskSpec& spec, const ProgressFn& progress, TaskControl& control) override {
        struct Handles {
            HANDLE toChildR = nullptr, toChildW = nullptr, fromChildR = nullptr, fromChildW = nullptr;
            PROCESS_INFORMATION pi{};
            ~Handles() {
                if (pi.hProcess) {
                    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
                        TerminateProcess(pi.hProcess, 1);
                        WaitForSingleObject(pi.hProcess, 2000);
                    }
                    CloseHandle(pi.hProcess);
                }
                if (pi.hThread) CloseHandle(pi.hThread);
                for (HANDLE h : {toChildR, toChildW, fromChildR, fromChildW}) if (h) CloseHandle(h);
            }
        } hs;

        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        if (!CreatePipe(&hs.toChildR, &hs.toChildW, &sa, 0) || !CreatePipe(&hs.fromChildR, &hs.fromChildW, &sa, 0))
            throw Error(HRESULT_FROM_WIN32(GetLastError()), "cannot create the worker pipes");
        SetHandleInformation(hs.toChildW, HANDLE_FLAG_INHERIT, 0);    // the host's ends stay with the host
        SetHandleInformation(hs.fromChildR, HANDLE_FLAG_INHERIT, 0);

        // only the two child ends are inherited
        HANDLE inherit[2] = {hs.toChildR, hs.fromChildW};
        SIZE_T attrSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
        std::vector<char> attrBuf(attrSize);
        auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
        InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize);
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, nullptr, nullptr);

        const TaskPriority prio = control.priority.load();
        DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
        if (prio == TaskPriority::BelowNormal) flags |= BELOW_NORMAL_PRIORITY_CLASS;
        else if (prio == TaskPriority::Background) flags |= IDLE_PRIORITY_CLASS;

        std::wstring cmd = L"\"" + opt_.workerExe.wstring() + L"\" --worker " + std::to_wstring(reinterpret_cast<uintptr_t>(hs.toChildR)) + L" " +
                           std::to_wstring(reinterpret_cast<uintptr_t>(hs.fromChildW));
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof si;
        si.lpAttributeList = attrs;
        const BOOL ok = CreateProcessW(opt_.workerExe.c_str(), cmd.data(), nullptr, nullptr, TRUE, flags, nullptr, nullptr, &si.StartupInfo, &hs.pi);
        const DWORD spawnErr = GetLastError();
        DeleteProcThreadAttributeList(attrs);
        if (!ok) throw Error(HRESULT_FROM_WIN32(spawnErr), "cannot start the worker process: " + opt_.workerExe.string());
        if (job_) AssignProcessToJobObject(job_, hs.pi.hProcess);
        ResumeThread(hs.pi.hThread);
        if (opt_.onSpawn) opt_.onSpawn(hs.pi.dwProcessId);
        diag::Debug(L"worker process {} started", hs.pi.dwProcessId);
        // the worker owns its ends now; closing ours is what lets a dying worker show up as a broken pipe
        CloseHandle(hs.toChildR); hs.toChildR = nullptr;
        CloseHandle(hs.fromChildW); hs.fromChildW = nullptr;

        Channel ch(hs.fromChildR, hs.toChildW);
        {
            Writer w;
            PutSpec(w, spec, prio);
            if (!ch.Send(kStart, w)) throw Error(E_FAIL, "the worker process did not accept the task");
        }

        std::exception_ptr callbackError;
        bool cancelSent = false;
        for (;;) {
            uint8_t type = 0;
            std::vector<uint8_t> body;
            if (!ch.Receive(type, body)) break;  // broken pipe: the worker is gone (crash) - handled below
            try {
                Reader r(body);
                if (type == kProgress) {
                    const uint64_t done = r.U64(), total = r.U64();
                    bool keep = !cancelSent;
                    if (keep) {
                        try { keep = progress ? progress(done, total) : true; }
                        catch (...) { callbackError = std::current_exception(); keep = false; }
                    }
                    if (!keep) cancelSent = true;
                    Writer w;
                    w.U8(keep ? 1 : 0);
                    w.U8(static_cast<uint8_t>(control.priority.load()));
                    if (!ch.Send(kAck, w)) break;
                } else if (type == kPasswordReq) {
                    const uint8_t which = r.U8();
                    std::wstring pw;
                    bool have = false;
                    const PasswordFn& fn = which == 0 ? spec.openOptions.password : spec.extractOptions.password;
                    if (fn) {
                        try { have = fn(pw); } catch (...) { callbackError = std::current_exception(); cancelSent = true; }
                    }
                    Writer w;
                    w.Bool(have);
                    w.Str(pw);
                    if (!ch.Send(kReply, w)) break;
                } else if (type == kConflictReq) {
                    ConflictInfo ci;
                    ci.existing = r.Path(); ci.entryPath = r.Str();
                    ci.newSize = r.U64(); ci.existingSize = r.U64(); ci.newMTime = r.U64(); ci.existingMTime = r.U64();
                    ConflictAnswer a;
                    a.action = ConflictAction::Cancel;
                    if (spec.extractOptions.onConflict) {
                        try { a = spec.extractOptions.onConflict(ci); }
                        catch (...) { callbackError = std::current_exception(); cancelSent = true; a.action = ConflictAction::Cancel; }
                    }
                    Writer w;
                    w.U8(static_cast<uint8_t>(a.action));
                    w.Bool(a.applyToAll);
                    if (!ch.Send(kReply, w)) break;
                } else if (type == kLargeReq) {
                    LargeExtractInfo li;
                    li.declaredBytes = r.U64(); li.packedBytes = r.U64();
                    bool go = false;
                    if (spec.extractOptions.onLarge) {
                        try { go = spec.extractOptions.onLarge(li); }
                        catch (...) { callbackError = std::current_exception(); cancelSent = true; go = false; }
                    }
                    Writer w;
                    w.Bool(go);
                    if (!ch.Send(kReply, w)) break;
                } else if (type == kResult) {
                    const uint8_t outcome = r.U8();
                    if (callbackError) std::rethrow_exception(callbackError);
                    if (outcome == 1) {
                        const int32_t hr = r.I32();
                        throw Error(hr, r.Msg());
                    }
                    if (outcome == 2) throw std::runtime_error(r.StrUtf8());
                    TaskResult res;
                    res.state = static_cast<TaskState>(r.U8());
                    res.error = r.Msg();
                    res.extract = GetExtractReport(r);
                    res.compress = GetCompressReport(r);
                    res.update = GetUpdateReport(r);
                    res.scan = GetScanReport(r);
                    res.hash = GetHashReport(r);
                    res.repack = GetRepackReport(r);
                    return res;
                }
            } catch (const std::runtime_error& e) {
                if (std::string(e.what()) == "bad message") break;  // treated like a lost worker
                throw;
            }
        }

        // no Result arrived: the worker died
        if (callbackError) std::rethrow_exception(callbackError);
        if (cancelSent) throw Error(static_cast<long>(0x80004004L), "cancelled");
        // a crashed compression leaves its temporary output behind (the extraction side has no such marker)
        if (spec.kind == TaskKind::Compress || spec.kind == TaskKind::Update) {
            std::error_code ec;
            WaitForSingleObject(hs.pi.hProcess, 3000);
            fs::remove(fs::path((spec.kind == TaskKind::Update ? spec.archive : spec.output).wstring() + L".lycantmp"), ec);
        }
        if (spec.kind == TaskKind::Repack) {
            // the half-made archive, and the scratch folder of that worker (named after its process id)
            std::error_code ec;
            WaitForSingleObject(hs.pi.hProcess, 3000);
            fs::remove(fs::path((spec.repackInPlace ? spec.archive : spec.output).wstring() + L".lycantmp"), ec);
            const std::wstring prefix = L"lycan_repack_" + std::to_wstring(hs.pi.dwProcessId) + L"_";
            for (auto const& d : fs::directory_iterator(ScratchDirectory(spec.openOptions.tempDir), ec))
                if (d.path().filename().wstring().rfind(prefix, 0) == 0) fs::remove_all(d.path(), ec);
        }
        DWORD code = 0;
        WaitForSingleObject(hs.pi.hProcess, 3000);
        GetExitCodeProcess(hs.pi.hProcess, &code);
        char msg[160];
        std::snprintf(msg, sizeof msg, "the worker process ended unexpectedly (exit code 0x%08lX)", static_cast<unsigned long>(code));
        diag::Err(L"worker process {} ended unexpectedly (exit code 0x{:08X})", hs.pi.dwProcessId, static_cast<unsigned long>(code));
        throw Error(E_FAIL, msg);
    }

private:
    ProcessExecutorOptions opt_;
    HANDLE job_ = nullptr;
};

}  // namespace

std::shared_ptr<TaskExecutor> MakeProcessExecutor(ProcessExecutorOptions options) {
    return std::make_shared<ProcessExecutor>(std::move(options));
}

// ------------------------------------------------------------------------------------------------ worker side
namespace {

void CrashLater(DWORD ms) {  // test hook: LYCAN_TEST_WORKER_CRASH=<ms>
    std::thread([ms] {
        Sleep(ms);
        volatile int* p = nullptr;
        *p = 1;
    }).detach();
}

}  // namespace

int RunWorker(uintptr_t readHandle, uintptr_t writeHandle) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    diag::Init(L"worker");
    Channel ch(reinterpret_cast<HANDLE>(readHandle), reinterpret_cast<HANDLE>(writeHandle));
    uint8_t type = 0;
    std::vector<uint8_t> body;
    if (!ch.Receive(type, body) || type != kStart) return 2;
    WireSpec ws;
    try {
        Reader r(body);
        ws = GetSpec(r);
    } catch (...) {
        return 2;
    }
    TaskSpec spec = std::move(ws.spec);
    wchar_t crash[16];
    if (GetEnvironmentVariableW(L"LYCAN_TEST_WORKER_CRASH", crash, 16) > 0) CrashLater(static_cast<DWORD>(_wtoi(crash)));

    TaskPriority currentPrio = TaskPriority::Normal;
    ApplyPriority(spec.priority, currentPrio);

    std::mutex m;  // one request / response at a time
    std::atomic<bool> lost{false};
    bool cancelled = false;
    auto lastSent = std::chrono::steady_clock::time_point{};

    ProgressFn progress = [&](uint64_t done, uint64_t total) -> bool {
        std::lock_guard lk(m);
        if (cancelled || lost) return false;
        const auto now = std::chrono::steady_clock::now();
        if (lastSent.time_since_epoch().count() != 0 && now - lastSent < std::chrono::milliseconds(50)) return true;
        lastSent = now;
        Writer w;
        w.U64(done);
        w.U64(total);
        uint8_t t = 0;
        std::vector<uint8_t> b;
        if (!ch.Send(kProgress, w) || !ch.Receive(t, b) || t != kAck) { lost = true; return false; }
        try {
            Reader r(b);
            const bool keep = r.Bool();
            ApplyPriority(static_cast<TaskPriority>(r.U8()), currentPrio);
            if (!keep) cancelled = true;
            return keep;
        } catch (...) {
            lost = true;
            return false;
        }
    };
    auto askPassword = [&](uint8_t which, std::wstring& out) -> bool {
        std::lock_guard lk(m);
        Writer w;
        w.U8(which);
        uint8_t t = 0;
        std::vector<uint8_t> b;
        if (!ch.Send(kPasswordReq, w) || !ch.Receive(t, b) || t != kReply) { lost = true; return false; }
        try {
            Reader r(b);
            const bool have = r.Bool();
            std::wstring pw = r.Str();
            if (have) out = std::move(pw);
            return have;
        } catch (...) {
            lost = true;
            return false;
        }
    };
    if (ws.hasOpenPassword) spec.openOptions.password = [&](std::wstring& o) { return askPassword(0, o); };
    if (ws.hasExtractPassword) spec.extractOptions.password = [&](std::wstring& o) { return askPassword(1, o); };
    if (ws.hasLarge)
        spec.extractOptions.onLarge = [&](const LargeExtractInfo& li) -> bool {
            std::lock_guard lk(m);
            Writer w;
            w.U64(li.declaredBytes); w.U64(li.packedBytes);
            uint8_t t = 0;
            std::vector<uint8_t> b;
            if (!ch.Send(kLargeReq, w) || !ch.Receive(t, b) || t != kReply) { lost = true; return false; }
            try { Reader r(b); return r.Bool(); } catch (...) { lost = true; return false; }
        };
    if (ws.hasConflict)
        spec.extractOptions.onConflict = [&](const ConflictInfo& ci) -> ConflictAnswer {
            std::lock_guard lk(m);
            Writer w;
            w.Path(ci.existing); w.Str(ci.entryPath);
            w.U64(ci.newSize); w.U64(ci.existingSize); w.U64(ci.newMTime); w.U64(ci.existingMTime);
            ConflictAnswer a;
            a.action = ConflictAction::Cancel;
            uint8_t t = 0;
            std::vector<uint8_t> b;
            if (!ch.Send(kConflictReq, w) || !ch.Receive(t, b) || t != kReply) { lost = true; return a; }
            try {
                Reader r(b);
                a.action = static_cast<ConflictAction>(r.U8());
                a.applyToAll = r.Bool();
            } catch (...) {
                lost = true;
                a.action = ConflictAction::Cancel;
            }
            return a;
        };

    // errors travel as Message (id + arguments), so the process of the UI shows them in its language
    Writer res;
    TaskControl control;
    try {
        TaskResult r = MakeInProcessExecutor()->Execute(spec, progress, control);
        res.U8(0);
        res.U8(static_cast<uint8_t>(r.state));
        res.Msg(r.error);
        PutExtractReport(res, r.extract);
        PutCompressReport(res, r.compress);
        PutUpdateReport(res, r.update);
        PutScanReport(res, r.scan);
        PutHashReport(res, r.hash);
        PutRepackReport(res, r.repack);
    } catch (const Error& e) {
        if (!e.cancelled()) diag::Warn(L"worker: the task failed: {}", diag::FromUtf8(e.what()));
        res = Writer{};
        res.U8(1);
        res.I32(static_cast<int32_t>(e.hr()));
        res.Msg(e.message());
    } catch (const std::exception& e) {
        res = Writer{};
        res.U8(2);
        res.StrUtf8(e.what());
    }
    std::lock_guard lk(m);
    return ch.Send(kResult, res) ? 0 : 3;
}

}  // namespace lycan
