#include "lycan/repack.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/security.h"
#include "lycan/tempdir.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

uint64_t SizeOf(const std::vector<fs::path>& files) {
    uint64_t n = 0;
    std::error_code ec;
    for (auto const& f : files) n += fs::file_size(ToLongPath(f), ec);
    return n;
}

// The scratch folder of one repack; removed with everything in it when the repack ends (also on failure and cancel)
struct ScratchFolder {
    fs::path dir;
    ~ScratchFolder() {
        if (dir.empty()) return;
        std::error_code ec;
        fs::remove_all(ToLongPath(dir), ec);
    }
};

// The mark of the web of the old archive goes to the new one (a downloaded archive stays "downloaded")
std::string ReadZoneIdentifier(const fs::path& file) {
    std::ifstream in(ToLongPath(file).wstring() + L":Zone.Identifier", std::ios::binary);
    if (!in) return {};
    return std::string((std::istreambuf_iterator<char>(in)), {});
}

void WriteZoneIdentifier(const fs::path& file, const std::string& zone) {
    if (zone.empty() || !ReadZoneIdentifier(file).empty()) return;
    std::ofstream out(ToLongPath(file).wstring() + L":Zone.Identifier", std::ios::binary | std::ios::trunc);
    out.write(zone.data(), static_cast<std::streamsize>(zone.size()));
}

// Maps the progress of one phase (its own done / total) into its share of the whole
ProgressFn Phase(const ProgressFn& all, int phase, int phases) {
    if (!all) return {};
    return [all, phase, phases](uint64_t done, uint64_t total) {
        constexpr uint64_t kScale = 1000000;
        const uint64_t part = total ? std::min<uint64_t>(kScale, done * kScale / total) : 0;
        return all(static_cast<uint64_t>(phase) * kScale + part, static_cast<uint64_t>(phases) * kScale);
    };
}

}  // namespace

bool WritableTypeOf(const ArchiveSource& src, ArchiveType& type) {
    const std::wstring f = Lower(src.FormatName());
    if (f == L"zip") type = ArchiveType::Zip;
    else if (f == L"7z") type = ArchiveType::SevenZip;
    else if (f == L"tar") type = ArchiveType::Tar;
    else if (f == L"tar (gzip)") type = ArchiveType::TarGz;
    else if (f == L"tar (xz)") type = ArchiveType::TarXz;
    else if (f == L"tar (bzip2)") type = ArchiveType::TarBz2;
    else return false;
    return true;
}

RepackReport RepackArchive(const fs::path& archive, const fs::path& output, const RepackOptions& o) {
    RepackReport rep;
    std::error_code ec;
    if (!fs::exists(ToLongPath(archive), ec))
        throw Error(HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND), Message::Make(L"core.archive_not_found", archive.wstring()));
    if (!o.inPlace && fs::equivalent(ToLongPath(archive), ToLongPath(output), ec))
        throw Error(E_INVALIDARG, "the new archive cannot be the old one (use inPlace)");
    fs::path target = o.inPlace ? fs::path(archive.wstring() + L".lycantmp") : output;
    const std::string zone = ReadZoneIdentifier(archive);
    const int phases = 3;   // unpack, pack, test

    // 1. unpack the whole archive into a scratch folder of its own
    OpenOptions oo = o.open;
    oo.tempDir = o.scratch;
    auto src = ArchiveSource::Open(archive, oo);
    {
        std::vector<fs::path> parts{archive};
        rep.archiveBefore = SizeOf(parts);
    }
    uint64_t unpacked = 0;
    for (auto const& e : src->Entries()) unpacked += e.size;
    ScratchFolder work;
    {
        const fs::path base = ScratchDirectory(o.scratch);
        uint64_t have = 0;
        const uint64_t need = unpacked + (1u << 20);
        if (ScratchFreeBytes(base, have) && need > have) throw Error(Error::kTempSpace, MakeTempSpaceMessage(need, have, base));
        work.dir = base / (L"lycan_repack_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
        fs::create_directories(ToLongPath(work.dir), ec);
        if (ec) throw Error(HRESULT_FROM_WIN32(ec.value()), Message::Make(L"core.cannot_create_temp_file", work.dir.wstring()));
    }
    ExtractOptions eo;
    eo.overwrite = OverwriteMode::Fail;
    eo.password = o.open.password;
    eo.progress = Phase(o.progress, 0, phases);
    eo.keepDamaged = false;      // a damaged item is left out (skipDamaged) or the whole repack fails
    eo.propagateMotw = false;    // the new archive gets the mark itself (CopyZoneIdentifier)
    const ExtractReport ex = ExtractTo(*src, work.dir, eo);
    if (ex.wrongPassword) throw Error(Error::kWrongPassword, Message::Make(L"core.wrong_password", archive.wstring()));
    if (!ex.ok() && !o.skipDamaged)
        throw Error(E_FAIL, Message::Make(L"core.repack_damaged", static_cast<uint64_t>(ex.errors.size())));
    for (auto const& m : ex.errors) rep.skipped.push_back(m);
    for (auto const& m : ex.rejected) rep.skipped.push_back(m);
    rep.files = ex.files;
    rep.dirs = ex.dirs;
    rep.bytes = ex.bytes;
    src.reset();   // closed: an archive replaced in place must not be open

    // 2. pack it again: every top-level item of the scratch folder goes to the top level of the new archive
    std::vector<fs::path> inputs;
    for (auto const& d : fs::directory_iterator(ToLongPath(work.dir), ec)) inputs.push_back(d.path());
    CompressOptions co = o.compress;
    co.overwrite = o.inPlace;    // the scratch file next to the archive may be left from an earlier attempt
    co.pathMode = PathMode::Relative;
    co.skipThumbsDb = false;     // the archive had them: they stay
    if (co.tempDir.empty()) co.tempDir = o.scratch;
    if (o.inPlace) co.volumeSize = 0;
    co.progress = Phase(o.progress, 1, phases);
    CompressReport cr;
    try {
        cr = CreateArchive(target, inputs, co);
    } catch (...) {
        if (o.inPlace) fs::remove(ToLongPath(target), ec);
        throw;
    }
    for (auto const& m : cr.warnings) rep.warnings.push_back(m);
    for (auto const& m : cr.errors) rep.errors.push_back(m);
    rep.outputs = cr.outputs.empty() ? std::vector<fs::path>{target} : cr.outputs;
    auto removeNew = [&] { for (auto const& f : rep.outputs) fs::remove(ToLongPath(f), ec); };
    if (!cr.ok() && o.inPlace) {
        removeNew();
        throw Error(E_FAIL, Message::Make(L"core.repack_test_failed"));
    }

    // 3. test the new archive; in place it replaces the old one only when it passes
    if (o.test || o.inPlace) {
        TestReport tr;
        try {
            OpenOptions to;
            to.tempDir = o.scratch;
            if (!co.password.empty()) {
                const std::wstring pw = co.password;
                to.password = [pw](std::wstring& out) { out = pw; return true; };
            }
            auto fresh = ArchiveSource::Open(rep.outputs.front(), to);
            ExtractOptions te;
            te.password = to.password;
            te.progress = Phase(o.progress, 2, phases);
            tr = TestArchive(*fresh, te);
        } catch (Error const& e) {
            if (e.cancelled()) { removeNew(); throw; }
            tr.errors.push_back(e.message());
        }
        rep.tested = true;
        if (!tr.ok()) {
            if (o.inPlace) {
                removeNew();
                throw Error(E_FAIL, Message::Make(L"core.repack_test_failed"));
            }
            for (auto const& m : tr.errors) rep.errors.push_back(m);
        } else if (tr.files != rep.files) {
            rep.warnings.push_back(Message::Make(L"core.repack_count_mismatch", tr.files, rep.files));
        }
    }

    if (o.inPlace) {
        const BOOL swapped = ReplaceFileW(ToLongPath(archive).c_str(), ToLongPath(target).c_str(), nullptr,
                                          REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr);
        if (!swapped) {
            const DWORD err = GetLastError();
            removeNew();
            throw Error(HRESULT_FROM_WIN32(err), Message::Make(L"core.cannot_replace_archive", archive.wstring()));
        }
        rep.outputs = {archive};
    }
    WriteZoneIdentifier(rep.outputs.front(), zone);
    rep.output = rep.outputs.front();
    rep.archiveAfter = SizeOf(rep.outputs);
    return rep;
}

}  // namespace lycan
