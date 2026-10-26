#include "lycan/extract.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <unordered_set>

#include "archive_impl.h"
#include "lycan/error.h"
#include "lycan/junk.h"
#include "lycan/security.h"
#include "lycan/testhooks.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

struct Budget {
    uint64_t limit = 0;  // 0 = no limit
    uint64_t written = 0;
    bool exceeded = false;
    // The sizes in the headers can lie (or be missing: a .gz only keeps 32 bits). When far more comes out than they said,
    // and that much is suspicious by the rule of ExtractOptions::onLarge, onLarge is asked once more with what came out so far.
    const ExtractOptions* opt = nullptr;
    uint64_t declared = 0, packed = 0;
    bool asked = false, declined = false;
    bool ShouldAsk(uint64_t actual) const {
        if (!opt || !opt->onLarge || asked) return false;
        if (actual <= 2 * declared || actual - declared < opt->largeBytes) return false;
        return (packed > 0 && actual / packed >= opt->largeRatio) || actual >= opt->hugeBytes;
    }
};

class FileOutStream final : public ISequentialOutStream, public CMyUnknownImp {
public:
    Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)
    Z7_IFACE_COM7_IMP(ISequentialOutStream)
public:
    FileOutStream(HANDLE h, Budget& b) : h_(h), budget_(b) {}
    ~FileOutStream() { Close(); }
    void SetTime(uint64_t ft) {
        if (h_ == INVALID_HANDLE_VALUE || ft == 0) return;
        FILETIME t{static_cast<DWORD>(ft & 0xFFFFFFFF), static_cast<DWORD>(ft >> 32)};
        SetFileTime(h_, nullptr, nullptr, &t);
    }
    void Close() {
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE h_;
    Budget& budget_;
};

Z7_COM7F_IMF(FileOutStream::Write(const void* data, UInt32 size, UInt32* processed)) {
    if (processed) *processed = 0;
    if (budget_.declined) return E_ABORT;
    if (budget_.limit && budget_.written + size > budget_.limit) {
        budget_.exceeded = true;
        return E_ABORT;
    }
    if (budget_.ShouldAsk(budget_.written + size)) {
        budget_.asked = true;
        LargeExtractInfo info;
        info.declaredBytes = budget_.written + size;
        info.packedBytes = budget_.packed;
        if (!budget_.opt->onLarge(info)) {
            budget_.declined = true;
            return E_ABORT;
        }
    }
    DWORD n = 0;
    if (TestWriteShouldFail(size)) return HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    if (!WriteFile(h_, data, size, &n, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    budget_.written += n;
    if (processed) *processed = n;
    return S_OK;
}

std::string ReadAds(const fs::path& streamPath) {
    HANDLE h = CreateFileW(streamPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char buf[4096];
    DWORD n = 0;
    std::string out;
    if (ReadFile(h, buf, sizeof(buf), &n, nullptr)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

void WriteAds(const fs::path& streamPath, const std::string& data) {
    HANDLE h = CreateFileW(streamPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &n, nullptr);
    CloseHandle(h);
}

// foo.txt -> foo (2).txt, foo (3).txt ... (until a name that does not exist)
fs::path UniqueSibling(const fs::path& p) {
    std::error_code ec;
    std::wstring stem = p.stem().wstring(), ext = p.extension().wstring();
    for (int i = 2; i < 10000; ++i) {
        fs::path c = p.parent_path() / (stem + L" (" + std::to_wstring(i) + L")" + ext);
        if (!fs::exists(ToLongPath(c), ec)) return c;
    }
    return p;
}

// foo.txt -> foo (damaged).txt, foo (damaged 2).txt ... : where a damaged item goes when the real name belongs to a good file
fs::path DamagedSibling(const fs::path& p) {
    std::error_code ec;
    std::wstring stem = p.stem().wstring(), ext = p.extension().wstring();
    for (int i = 1; i < 10000; ++i) {
        fs::path c = p.parent_path() / (stem + (i == 1 ? L" (damaged)" : L" (damaged " + std::to_wstring(i) + L")") + ext);
        if (!fs::exists(ToLongPath(c), ec)) return c;
    }
    return p;
}

// std::filesystem::create_directories reports an error for a folder that already exists when it is a drive root
// ("D:\\"), which made extracting next to an archive on the root of a drive fail. Judge by the result, not the code.
bool EnsureDir(const fs::path& p) {
    std::error_code ec;
    const fs::path lp = ToLongPath(p);
    if (fs::is_directory(lp, ec)) return true;
    fs::create_directories(lp, ec);
    return fs::is_directory(lp, ec);
}

bool IsInside(const fs::path& root, const fs::path& p) {
    // Compare absolute paths: a relative destination such as "." normalises to something that is not a
    // prefix of the (also normalised) file path, which wrongly rejected every file.
    std::error_code ec;
    auto r = fs::absolute(root, ec).lexically_normal();
    auto q = fs::absolute(p, ec).lexically_normal();
    auto [ri, qi] = std::mismatch(r.begin(), r.end(), q.begin(), q.end());
    // allow the empty name that is left of root ("dir\\")
    while (ri != r.end() && ri->empty()) ++ri;
    return ri == r.end() && qi != q.end();
}

// Applies ExtractOptions::stripPrefix; an empty string returned means this item is the stripped folder itself (nothing to output)
std::wstring StripPrefix(const ExtractOptions& opt, const std::wstring& path) {
    const std::wstring& pre = opt.stripPrefix;
    if (pre.empty()) return path;
    if (path.size() >= pre.size() && path.compare(0, pre.size(), pre) == 0) return path.substr(pre.size());
    // the folder item may have no trailing '/': compare with the prefix without '/' (it is the stripped folder itself)
    if (path.size() + 1 == pre.size() && pre.compare(0, path.size(), path) == 0) return {};
    return path;
}

// Puts the finished file in the place of the old one. The old one can be held for a moment by someone else (a virus scanner,
// the indexer, another extraction into the same folder that is replacing it too): that is "access denied" or "sharing
// violation", so it is tried again for about a second before it counts as an error.
bool ReplaceWithRetry(const fs::path& from, const fs::path& to) {
    for (int attempt = 0;; ++attempt) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        const DWORD err = GetLastError();
        if (attempt >= 20 || (err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION)) {
            SetLastError(err);
            return false;
        }
        Sleep(50);
    }
}

class DiskTarget final : public ExtractTarget {
public:
    DiskTarget(ArchiveSource::Impl& impl, const fs::path& dest, const ExtractOptions& opt, ExtractReport& rep,
               Budget& budget, std::string motw)
        : impl_(impl), dest_(dest), opt_(opt), rep_(rep), budget_(budget), motw_(std::move(motw)) {}

    CMyComPtr<ISequentialOutStream> Begin(uint32_t index) override {
        cur_.Release();
        curRaw_ = nullptr;
        curPath_.clear();
        curFinal_.clear();
        curFull_.clear();
        curReplacing_ = false;
        const Entry& e = impl_.entries[index];

        if (e.isSymlink) {
            rep_.skipped.push_back(Message::Make(L"core.skipped_symlink_policy", e.path));
            return nullptr;
        }
        if (e.isHardLink) {
            rep_.skipped.push_back(Message::Make(L"core.skipped_hardlink_policy", e.path));
            return nullptr;
        }
        std::wstring shown = StripPrefix(opt_, e.path);
        if (shown.empty()) return nullptr;  // it is the folder that is stripped
        SafePath sp = SanitizeEntryPath(shown);
        if (sp.rejected) {
            rep_.rejected.push_back(Message::Make(L"core.path_reason", e.path, sp.reason));
            return nullptr;
        }
        if (sp.modified) rep_.renamed.push_back(e.path + L" -> " + sp.relative.generic_wstring());

        fs::path full = dest_ / sp.relative;
        if (!IsInside(dest_, full)) {  // defence in depth: in theory SanitizeEntryPath has ruled this out
            rep_.rejected.push_back(Message::Make(L"core.rejected_outside", e.path));
            return nullptr;
        }

        std::error_code ec;
        if (e.isDir) {
            if (!EnsureDir(full)) rep_.errors.push_back(Message::Make(L"core.cannot_create_folder", e.path));
            else ++rep_.dirs;
            return nullptr;
        }

        if (!EnsureDir(full.parent_path())) {
            rep_.errors.push_back(Message::Make(L"core.cannot_create_parent", e.path));
            return nullptr;
        }

        fs::path lp = ToLongPath(full);
        bool exists = fs::exists(lp, ec);
        bool replacing = false;
        if (exists && fs::is_directory(lp, ec)) {
            rep_.errors.push_back(Message::Make(L"core.folder_in_the_way", e.path));
            return nullptr;
        }
        if (exists) {
            OverwriteMode mode = opt_.overwrite;
            if (mode == OverwriteMode::Ask) {
                if (sticky_) {
                    mode = *sticky_;
                } else if (opt_.onConflict) {
                    ConflictInfo ci;
                    ci.existing = full;
                    ci.entryPath = e.path;
                    ci.newSize = e.size;
                    ci.newMTime = e.mtimeFileTime;
                    WIN32_FILE_ATTRIBUTE_DATA d;
                    if (GetFileAttributesExW(lp.c_str(), GetFileExInfoStandard, &d)) {
                        ci.existingSize = (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
                        ci.existingMTime = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
                    }
                    ConflictAnswer a = opt_.onConflict(ci);
                    switch (a.action) {
                        case ConflictAction::Cancel: throw Error(E_ABORT, Message::Make(L"core.cancelled"));
                        case ConflictAction::Overwrite: mode = OverwriteMode::Overwrite; break;
                        case ConflictAction::Rename: mode = OverwriteMode::Rename; break;
                        default: mode = OverwriteMode::Skip; break;
                    }
                    if (a.applyToAll) sticky_ = mode;
                } else {
                    mode = OverwriteMode::Fail;
                }
            }
            if (mode == OverwriteMode::Skip) {
                rep_.skipped.push_back(Message::Make(L"core.skipped_exists", e.path));
                return nullptr;
            }
            if (mode == OverwriteMode::Fail) {
                rep_.errors.push_back(Message::Make(L"core.file_exists", e.path));
                return nullptr;
            }
            if (mode == OverwriteMode::Rename) {
                full = UniqueSibling(full);
                lp = ToLongPath(full);
                rep_.renamed.push_back(e.path + L" -> " + full.filename().wstring());
            }
            // Overwrite: the new content is written beside the old file and takes its place when it is complete (End)
            replacing = mode == OverwriteMode::Overwrite;
        }

        fs::path writeTo = lp;
        if (replacing) {
            static std::atomic<unsigned> seq{0};  // several extractions can run at once in one process
            writeTo = ToLongPath(full.parent_path() / (L".lycanzip-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++seq) + L".tmp"));
        }
        // a file that is being written by someone else at this moment (another extraction into this folder, a scanner) is waited for, about a second
        HANDLE h = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt <= 20; ++attempt) {
            h = CreateFileW(writeTo.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION || attempt == 20) break;
            Sleep(50);
        }
        if (h == INVALID_HANDLE_VALUE) {
            rep_.errors.push_back(Message::Make(L"core.cannot_create_file", e.path, GetLastError()));
            return nullptr;
        }
        curRaw_ = new FileOutStream(h, budget_);
        cur_ = curRaw_;
        curPath_ = writeTo;
        curFinal_ = lp;
        curFull_ = full;
        curReplacing_ = replacing;
        return cur_;
    }

    void End(uint32_t index, int opRes) override {
        if (!cur_) {
            // No file was opened: skipped / refused item (the result is OK), or 7-Zip cannot decode it and falls back to "test mode"
            // (for example a 7z with a wrong password: GetStream gets kTest, the result is kDataError). The latter must be an error, otherwise it would look like success.
            if (!OpResultUsable(opRes) && index < impl_.entries.size()) {
                const Entry& e = impl_.entries[index];
                using namespace NArchive::NExtract::NOperationResult;
                if (!e.isDir) {
                    if (opRes == kWrongPassword || (e.isEncrypted && (opRes == kDataError || opRes == kCRCError)))
                        rep_.wrongPassword = true;
                    rep_.errors.push_back(Message::Make(L"core.path_reason", e.path, OpResultMessage(opRes)));
                }
            }
            return;
        }
        const Entry& e = impl_.entries[index];
        curRaw_->SetTime(e.mtimeFileTime);
        curRaw_->Close();
        fs::path where = curFinal_;  // where the content ends up
        if (!OpResultUsable(opRes)) {
            using namespace NArchive::NExtract::NOperationResult;
            const bool guessedPassword = opRes == kWrongPassword || (e.isEncrypted && (opRes == kDataError || opRes == kCRCError));
            if (guessedPassword) rep_.wrongPassword = true;
            rep_.errors.push_back(Message::Make(L"core.path_reason", e.path, OpResultMessage(opRes)));
            // What is worth keeping: the content came out (CRC mismatch) or its first part did (cut off, damaged). Not what a wrong password produced.
            const bool keepable = opt_.keepDamaged && !guessedPassword && (opRes == kCRCError || opRes == kDataError || opRes == kUnexpectedEnd);
            bool kept = false;
            if (keepable) {
                if (curReplacing_) {  // the real name belongs to a good file: the damaged one goes beside it
                    where = ToLongPath(DamagedSibling(curFull_));
                    kept = MoveFileExW(curPath_.c_str(), where.c_str(), 0) != 0;
                } else {
                    kept = true;
                }
            }
            if (kept) {
                rep_.kept.push_back(Message::Make(L"core.path_reason", curReplacing_ ? e.path + L" -> " + where.filename().wstring() : e.path, OpResultMessage(opRes)));
                if (!motw_.empty()) WriteAds(fs::path(where.wstring() + L":Zone.Identifier"), motw_);
            } else {
                DeleteFileW(curPath_.c_str());  // no leftovers (the file that was there before is untouched when the new one went to a temporary name)
            }
        } else {
            bool placed = true;
            if (curReplacing_) placed = ReplaceWithRetry(curPath_, curFinal_);
            if (!placed) {
                rep_.errors.push_back(Message::Make(L"core.cannot_replace_file", e.path, GetLastError()));
                DeleteFileW(curPath_.c_str());
            } else {
                if (opRes != NArchive::NExtract::NOperationResult::kOK) rep_.warnings.push_back(Message::Make(L"core.path_reason", e.path, OpResultMessage(opRes)));  // complete, the CRC matched: keep it
                ++rep_.files;
                rep_.bytes += e.size;
                if (!motw_.empty()) WriteAds(fs::path(where.wstring() + L":Zone.Identifier"), motw_);
            }
        }
        cur_.Release();
        curRaw_ = nullptr;
    }

    // The job was aborted (cancel, limit exceeded, fatal error) while a file was being written:
    // close it and delete the half-written file.
    void DiscardCurrent() {
        if (!cur_) return;
        curRaw_->Close();
        DeleteFileW(curPath_.c_str());
        cur_.Release();
        curRaw_ = nullptr;
    }

private:
    ArchiveSource::Impl& impl_;
    fs::path dest_;
    const ExtractOptions& opt_;
    ExtractReport& rep_;
    Budget& budget_;
    std::string motw_;
    std::optional<OverwriteMode> sticky_;  // "apply to all" for conflicts
    CMyComPtr<ISequentialOutStream> cur_;
    FileOutStream* curRaw_ = nullptr;  // lifetime of cur_ is in the same scope
    fs::path curPath_;       // where the content is being written (a temporary name when it is to replace a file)
    fs::path curFinal_;      // the name it is meant for (long path)
    fs::path curFull_;       // the same, as a normal path
    bool curReplacing_ = false;
};

// Test mode: nothing is written, only the per-item result is recorded.
class TestTarget final : public ExtractTarget {
public:
    TestTarget(ArchiveSource::Impl& impl, TestReport& rep) : impl_(impl), rep_(rep) {}
    CMyComPtr<ISequentialOutStream> Begin(uint32_t) override { return nullptr; }
    void End(uint32_t index, int opRes) override {
        if (index >= impl_.entries.size()) return;
        const Entry& e = impl_.entries[index];
        if (e.isDir) return;
        using namespace NArchive::NExtract::NOperationResult;
        if (OpResultUsable(opRes)) {
            if (opRes != kOK) rep_.warnings.push_back(Message::Make(L"core.path_reason", e.path, OpResultMessage(opRes)));
            ++rep_.files;
            return;
        }
        if (opRes == kWrongPassword || (e.isEncrypted && (opRes == kDataError || opRes == kCRCError))) rep_.wrongPassword = true;
        rep_.errors.push_back(Message::Make(L"core.path_reason", e.path, OpResultMessage(opRes)));
    }
private:
    ArchiveSource::Impl& impl_;
    TestReport& rep_;
};

}  // namespace

TestReport TestArchive(ArchiveSource& src, const ExtractOptions& options) {
    TestReport rep;
    TestTarget target(src.impl(), rep);
    RunExtract(src.impl(), {}, true, target, options.progress, options.password);
    return rep;
}

ExtractReport ExtractTo(ArchiveSource& src, const fs::path& destDir, const ExtractOptions& options,
                        const std::vector<uint32_t>& requested) {
    auto& impl = src.impl();
    ExtractReport rep;

    // Leave out the clutter macOS adds; `indices` below is the list that is really extracted (empty = everything)
    std::vector<uint32_t> kept;
    const std::vector<uint32_t>* want = &requested;
    if (options.skipMacMetadata) {
        auto consider = [&](uint32_t i) {
            if (i >= impl.entries.size()) return;
            if (IsMacMetadataPath(impl.entries[i].path)) ++rep.macMetadataSkipped;
            else kept.push_back(i);
        };
        if (requested.empty()) for (uint32_t i = 0; i < impl.entries.size(); ++i) consider(i);
        else for (auto i : requested) consider(i);
        if (kept.empty() && rep.macMetadataSkipped > 0) return rep;  // nothing but metadata was asked for
        want = &kept;
    }
    const std::vector<uint32_t>& indices = *want;

    // Pre-check (the sizes declared in the archive; the real written amount is counted by the Budget below)
    uint64_t total = 0;
    auto sum = [&](const Entry& e) { total += e.size; };
    if (indices.empty()) for (auto& e : impl.entries) sum(e);
    else for (auto i : indices) if (i < impl.entries.size()) sum(impl.entries[i]);
    if (options.maxTotalBytes && total > options.maxTotalBytes)
        throw Error(E_FAIL, Message::Make(L"core.bomb_total"));
    if (options.maxRatio) {
        std::error_code ec;
        auto packed = fs::file_size(impl.file, ec);
        if (!ec && packed > 0 && total / packed > options.maxRatio)
            throw Error(E_FAIL, Message::Make(L"core.bomb_ratio"));
    }

    // A suspiciously big archive: ask (the sizes are the declared ones; the archive file is its first volume when it is split)
    if (options.onLarge) {
        std::error_code ec;
        const uint64_t packed = fs::file_size(impl.file, ec);
        const bool ratioBig = !ec && packed > 0 && total >= options.largeBytes && total / packed >= options.largeRatio;
        if (ratioBig || total >= options.hugeBytes) {
            LargeExtractInfo info;
            info.declaredBytes = total;
            info.packedBytes = ec ? 0 : packed;
            if (!options.onLarge(info)) throw Error(E_ABORT, Message::Make(L"core.cancelled"));
        }
    }

    // Free-space precheck: refuse early (before anything is written) when the declared size cannot fit.
    {
        uint64_t freeBytes = 0;
        if (total > 0 && QueryFreeSpace(ToLongPath(destDir).c_str(), freeBytes) && total > freeBytes) {
            const uint64_t needMB = (total + (1u << 20) - 1) >> 20, haveMB = freeBytes >> 20;
            throw Error(static_cast<long>(0x80070070L), Message::Make(L"core.disk_full_target", needMB, haveMB));
        }
    }

    // Wrap: decide the real top-level folder
    fs::path root = destDir;
    rep.outputDir = destDir;
    if (options.smart) {
        std::unordered_set<std::wstring> tops;  // names of the top level (lower case, folders removed); collected as a set: with tens of thousands of files a list would be O(n^2)
        std::wstring singleTop;
        bool topIsFolder = false;
        auto scan = [&](const Entry& e) {
            std::wstring shown = StripPrefix(options, e.path);
            if (shown.empty()) return;
            SafePath sp = SanitizeEntryPath(shown);
            if (sp.rejected) return;
            std::wstring first = sp.relative.begin()->wstring();
            std::wstring key = first;
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (tops.insert(key).second) singleTop = first;
            if (std::distance(sp.relative.begin(), sp.relative.end()) > 1 || e.isDir) topIsFolder = true;
        };
        if (indices.empty()) for (auto& e : impl.entries) scan(e);
        else for (auto i : indices) if (i < impl.entries.size()) scan(impl.entries[i]);

        if (tops.size() > 1) {
            std::wstring name = options.wrapName.empty() ? ArchiveBaseName(impl.file) : options.wrapName;
            SafePath w = SanitizeEntryPath(name);
            root = destDir / (w.rejected ? fs::path(L"extracted") : w.relative);
            rep.outputDir = root;
        } else if (tops.size() == 1 && topIsFolder) {
            rep.outputDir = destDir / singleTop;
        }
    }

    if (!EnsureDir(root)) throw Error(E_FAIL, Message::Make(L"core.cannot_create_target", root.wstring()));

    std::string motw;
    if (options.propagateMotw) motw = ReadAds(fs::path(ToLongPath(impl.file).wstring() + L":Zone.Identifier"));

    Budget budget;
    budget.limit = options.maxTotalBytes;
    budget.opt = &options;
    budget.declared = total;
    {
        std::error_code ec;
        budget.packed = fs::file_size(impl.file, ec);
        if (ec) budget.packed = 0;
    }
    DiskTarget target(impl, root, options, rep, budget, motw);
    try {
        RunExtract(impl, indices, false, target, options.progress, options.password);
        if (budget.declined) throw Error(E_ABORT, Message::Make(L"core.cancelled"));
    } catch (const Error&) {
        target.DiscardCurrent();
        if (budget.exceeded) throw Error(E_FAIL, Message::Make(L"core.bomb_actual"));
        if (budget.declined) throw Error(E_ABORT, Message::Make(L"core.cancelled"));
        throw;
    } catch (...) {
        target.DiscardCurrent();
        throw;
    }
    return rep;
}

}  // namespace lycan
