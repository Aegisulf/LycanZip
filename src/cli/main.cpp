// lycanzip CLI: l lists, x extracts, t tests, cat writes one item to stdout (checks the VFS streaming)
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

#include <algorithm>

#include "lycan/archive_source.h"
#include "lycan/compress.h"
#include "lycan/diag.h"
#include "lycan/encoding.h"
#include "lycan/error.h"
#include "lycan/extract.h"
#include "lycan/checksum.h"
#include "lycan/hash.h"
#include "lycan/scan.h"
#include "lycan/settings.h"
#include "lycan/taskexec.h"
#include "lycan/repack.h"
#include "lycan/update.h"
#include "lycan/version.h"

using namespace lycan;

namespace {

void Out(const std::string& s) { fwrite(s.data(), 1, s.size(), stdout); }
void Err(const std::string& s) { fwrite(s.data(), 1, s.size(), stderr); }

void Usage() {
    Err("lycanzip-cli " LYCAN_VERSION_STRING "\n"
        "Usage:\n"
        "  lycanzip-cli info                             shows the version, 7z.dll, the place of the settings file and whether this is portable mode (add it to a bug report)\n"
        "  lycanzip-cli a <archive> <inputs...> [-t zip|7z|tar|tgz|txz|tbz2|iso] [-p pw] [-l 0-9]\n"
        "             [--profile compatible|balanced|smallest|fastest] [-v 50m] [--encrypt-names] [-y]\n"
        "                                            creates an archive (the format is taken from the extension unless -t is given)\n"
        "  lycanzip-cli u <archive> [-p pw] [-l 0-9] [--profile ...] [--backup] <operations...>\n"
        "                                            changes an existing zip / 7z / tar (the operations run in order; --into sets the folder that the following --add puts files in):\n"
        "                                              --add <path>  --into <folder>  --delete <path>  --rename <from> <to>\n"
        "                                              --mkdir <path>  --replace <path> <file>  --estimate (only estimates, changes nothing)\n"
        "                                            when --add meets a file of the same name in the archive (--mode): replace = always replace (default), update = replace when the file on disk is newer, and add new files,\n"
        "                                            freshen = only update what the archive has (when newer), sync = make the archive the same as the folder (replace what differs, delete what is extra);\n"
        "                                            --dry-run only lists what would be done (added, replaced, skipped, deleted), changes nothing\n"
        "  (a and u both take --no-store-compressed: files that are already compressed are not stored without compression)\n"
        "  lycanzip-cli convert <archive> <new archive> [-t zip|7z|tar|tgz|txz|tbz2] [-p pw] [--new-password pw | --no-password]\n"
        "             [-l 0-9] [--profile ...] [-v 50m] [--skip-damaged] [--no-test]\n"
        "                                            unpacks the archive and packs it again as a new one (the format from the extension unless -t is given);\n"
        "                                            the new archive keeps the password (-p) unless --new-password or --no-password; it is tested afterwards\n"
        "  lycanzip-cli unprotect <archive> -p pw     removes the password: the archive is packed again without it and replaced only when the new one passes the test\n"
        "  lycanzip-cli upgrade <archive> [-p pw]     an old zip / tar whose names have no UTF-8 mark: the names are written again in UTF-8 (the encoding\n"
        "                                            detected, or --encoding), the data is copied as it is; replaced only when the new one passes the test\n"
        "  a also takes --sums-archive (write <archive>.sha256) and --sums-files (write <name>.sha256 with the files that went in), next to the archive\n"
        "  lycanzip-cli l <archive>                      lists the content\n"
        "  lycanzip-cli x <archive> [-o dir] [-p pw] [-y|-s|-r] [--smart|--no-smart] [--max-size 10g] [--max-ratio 1000] [--discard-broken]\n"
        "                                            extracts (-y overwrite, -s skip, -r rename automatically; without -o it extracts smartly next to the archive;\n"
        "                                            --max-size / --max-ratio: stop when the expanded size or the ratio is over the limit (compression bombs);\n"
        "                                            files with a data error are kept and listed, --discard-broken deletes them instead)\n"
        "  lycanzip-cli t <archive> [-p pw]              tests the integrity\n"
        "  lycanzip-cli hash <files...> [-a crc32,md5,sha1,sha256,sha384,sha512|all] [-o <file>]\n"
        "                                            checksums (SHA-256 by default; the output is the same as sha256sum: 'value *name'; with several algorithms: 'SHA256 (name) = value')\n"
        "  lycanzip-cli hash --in <archive> [<item path>...] [-a ...] [-p pw] [-o <file>]   files inside an archive (no path = all of them; nothing is extracted to disk)\n"
        "  lycanzip-cli hash --check <checksum file>   same as verify <checksum file>\n"
        "  lycanzip-cli verify <checksum files...> [--in <archive>] [-p pw] [--strip <prefix>] [--add-prefix <prefix>]\n"
        "                                            Check .sha256 / .sha1 / .sha512 / .md5 / .sfv files: the files next to the checksum file, or with --in the\n"
        "                                            files inside the archive (names are matched by themselves; --strip / --add-prefix when they cannot be).\n"
        "                                            Only what is wrong is listed. Exit code 0 = all match, 1 = a mismatch, a missing file or an unusable checksum file\n"
        "  lycanzip-cli scan <archive> [-p pw]           hands every file to the installed antivirus (AMSI); exit code 0 = nothing found, 5 = suspicious files found, 1 = could not scan\n"
        "  common option: --encoding auto|utf8|big5|gbk|sjis|kr|oem|<code page>  sets the encoding of zip/tar names (detected automatically by default)\n"
        "  lycanzip-cli cat <archive> <index> [-p pw]    writes one item to stdout\n");
}

class StdoutSink final : public ByteSink {
public:
    void Write(const void* d, size_t n) override { fwrite(d, 1, n, stdout); }
};
class NullSink final : public ByteSink {
public:
    void Write(const void*, size_t) override {}
};

// "50k" "100m" "4.7g" "1048576"
bool ParseSize(const std::wstring& s, uint64_t& out) {
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    double v = wcstod(s.c_str(), &end);
    if (end == s.c_str() || v <= 0) return false;
    std::wstring unit = end;
    for (auto& c : unit) c = static_cast<wchar_t>(towlower(c));
    double mul = 1;
    if (unit == L"k" || unit == L"kb") mul = 1024.0;
    else if (unit == L"m" || unit == L"mb") mul = 1024.0 * 1024;
    else if (unit == L"g" || unit == L"gb") mul = 1024.0 * 1024 * 1024;
    else if (!unit.empty() && unit != L"b") return false;
    out = static_cast<uint64_t>(v * mul);
    return out > 0;
}

// ---- hash ------------------------------------------------------------------------------------------------------------------------

std::string BsdName(uint32_t algo) {
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

bool ParseAlgos(const std::wstring& list, uint32_t& mask) {
    mask = 0;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t end = list.find(L',', pos);
        if (end == std::wstring::npos) end = list.size();
        std::wstring w = list.substr(pos, end - pos);
        for (auto& c : w) c = static_cast<wchar_t>(towlower(c));
        w.erase(std::remove(w.begin(), w.end(), L'-'), w.end());
        if (w == L"all") mask |= kHashAll;
        else if (w == L"crc32" || w == L"crc") mask |= kHashCrc32;
        else if (w == L"md5") mask |= kHashMd5;
        else if (w == L"sha1") mask |= kHashSha1;
        else if (w == L"sha256") mask |= kHashSha256;
        else if (w == L"sha384") mask |= kHashSha384;
        else if (w == L"sha512") mask |= kHashSha512;
        else if (!w.empty()) return false;
        pos = end + 1;
    }
    return mask != 0;
}

std::string HashProgressText(uint64_t done, uint64_t total) {
    char b[16];
    snprintf(b, sizeof(b), "\r%3d%%", total ? static_cast<int>((std::min<uint64_t>)(done * 100 / total, 100)) : 100);
    return b;
}

// ---- verify ----------------------------------------------------------------------------------------------------------------------
std::string SumMessage(SumFileError e) {
    switch (e) {
        case SumFileError::Unreadable: return "cannot read the checksum file";
        case SumFileError::TooBig: return "the checksum file is too big";
        case SumFileError::NoLines: return "no checksum lines found";
        default: return {};
    }
}

// lycanzip-cli verify <sums...> [--in <archive>] ; returns the exit code
int VerifyFiles(const std::vector<std::filesystem::path>& sumFiles, const std::filesystem::path& archive, const PasswordFn& password, bool tty,
                const ArchiveSumOptions& match) {
    int code = 0;
    std::unique_ptr<ArchiveSource> src;
    try {
        if (!archive.empty()) {
            OpenOptions oo;
            oo.password = password;
            src = ArchiveSource::Open(archive, oo);
        }
        for (const auto& file : sumFiles) {
            if (sumFiles.size() > 1) Out("== " + ToUtf8(file.wstring()) + " ==\n");
            SumVerifyOptions vo;
            vo.password = password;
            if (tty) vo.progress = [](uint64_t d, uint64_t t) { Err(HashProgressText(d, t)); return true; };
            const SumFile sums = ReadSumFile(file);
            const SumVerifyReport rep = src ? VerifySumAgainstArchive(sums, *src, vo, match) : VerifySumFile(sums, vo);
            if (tty) Err("\r    \r");
            if (rep.error != SumFileError::None) { Out(SumMessage(rep.error) + "\n"); code = 1; continue; }
            for (const auto& p : rep.problems) {
                const std::string name = ToUtf8(p.name);
                if (p.status == SumStatus::Mismatch) Out(name + ": FAILED\n");
                else if (p.status == SumStatus::Missing) Out(name + ": MISSING\n");
                else Out(name + ": ERROR (" + ToUtf8(p.error) + ")\n");
            }
            for (const auto& e : rep.extra) Out(ToUtf8(e) + ": not in the checksum file\n");
            if (src && !rep.confident) {
                Out("Most names of the checksum file are not in the archive; try --strip <prefix> and --add-prefix <prefix>\n");
                code = 1;
            }
            if (sums.badLines) Out(std::to_string(sums.badLines) + " lines of the checksum file were not understood\n");
            if (rep.allMatch() && rep.extra.empty()) Out("All " + std::to_string(rep.total) + " files match\n");
            else Out(std::to_string(rep.matched) + " of " + std::to_string(rep.total) + " match" +
                     (rep.mismatched ? ", " + std::to_string(rep.mismatched) + " differ" : "") + (rep.missing ? ", " + std::to_string(rep.missing) + " missing" : "") +
                     (rep.errors ? ", " + std::to_string(rep.errors) + " could not be read" : "") +
                     (rep.extra.empty() ? "" : ", " + std::to_string(rep.extra.size()) + " in the archive but not listed") + "\n");
            if (!rep.allMatch()) code = 1;
        }
    } catch (const Error& e) {
        if (tty) Err("\r    \r");
        Err(std::string(e.cancelled() ? "cancelled" : std::string("error: ") + e.what()) + "\n");
        return 1;
    }
    return code;
}

int VerifyCommand(int argc, wchar_t** argv) {
    std::vector<std::filesystem::path> sums;
    std::filesystem::path archive;
    std::wstring password;
    bool hasPassword = false;
    ArchiveSumOptions match;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--in" && i + 1 < argc) archive = argv[++i];
        else if (a == L"-p" && i + 1 < argc) { password = argv[++i]; hasPassword = true; }
        else if (a == L"--strip" && i + 1 < argc) { match.stripPrefix = argv[++i]; match.manual = true; }
        else if (a == L"--add-prefix" && i + 1 < argc) { match.addPrefix = argv[++i]; match.manual = true; }
        else if (!a.empty() && a[0] != L'-') sums.emplace_back(a);
        else { Usage(); return 2; }
    }
    if (sums.empty()) { Usage(); return 2; }
    PasswordFn pw;
    if (hasPassword) pw = [password](std::wstring& out) { out = password; return true; };
    return VerifyFiles(sums, archive, pw, _isatty(_fileno(stderr)) != 0, match);
}

// lycanzip-cli hash ...
int HashCommand(int argc, wchar_t** argv) {
    uint32_t algos = kHashSha256;
    std::filesystem::path sumOut, check, archive, sumBase;
    std::wstring password;
    bool hasPassword = false;
    std::vector<std::wstring> items;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"-a" && i + 1 < argc) { if (!ParseAlgos(argv[++i], algos)) { Err("Unknown algorithm (available: crc32 md5 sha1 sha256 sha384 sha512 all)\n"); return 2; } }
        else if (a == L"-o" && i + 1 < argc) sumOut = argv[++i];
        else if (a == L"-p" && i + 1 < argc) { password = argv[++i]; hasPassword = true; }
        else if (a == L"--in" && i + 1 < argc) archive = argv[++i];
        else if (a == L"--check" && i + 1 < argc) check = argv[++i];
        else items.push_back(a);
    }
    const bool tty = _isatty(_fileno(stderr)) != 0;
    HashOptions ho;
    ho.algos = algos;
    if (hasPassword) ho.password = [password](std::wstring& out) { out = password; return true; };
    if (tty) ho.progress = [](uint64_t d, uint64_t t) { Err(HashProgressText(d, t)); return true; };

    try {
        if (!check.empty()) return VerifyFiles({check}, {}, ho.password, tty, {});

        HashReport rep;
        if (!archive.empty()) {
            OpenOptions oo;
            if (hasPassword) oo.password = ho.password;
            auto src = ArchiveSource::Open(archive, oo);
            std::vector<uint32_t> idx;
            for (auto const& e : src->Entries()) {
                if (e.isDir) continue;
                bool want = items.empty();
                for (auto const& it : items) {
                    std::wstring w = it;
                    std::replace(w.begin(), w.end(), L'\\', L'/');
                    if (e.path == w || (!w.empty() && e.path.rfind(w + L"/", 0) == 0)) want = true;
                }
                if (want) idx.push_back(e.index);
            }
            if (idx.empty()) { Err("No file in the archive matches\n"); return 1; }
            rep = HashEntries(*src, idx, ho);
        } else {
            if (items.empty()) { Usage(); return 2; }
            std::vector<std::filesystem::path> files(items.begin(), items.end());
            bool anyFolder = false;
            for (auto const& f : files) { std::error_code fec; if (std::filesystem::is_directory(f, fec)) anyFolder = true; }
            if (anyFolder) { sumBase = files[0].lexically_normal().parent_path(); files = ExpandForHash(files); }   // folders: every file in them, names relative to the folder's parent
            rep = HashFiles(files, ho);
        }
        if (tty) Err("\r    \r");
        const auto list = HashAlgoList(rep.algos);
        for (auto const& f : rep.files)
            if (!f.ok()) Err(ToUtf8(f.path) + ": " + ToUtf8(f.error) + "\n");
        const std::string text = list.size() == 1 ? HashReportText(rep, list[0], sumBase) : HashReportTextAll(rep, sumBase);
        if (sumOut.empty()) Out(text);
        else {
            std::ofstream o(sumOut, std::ios::binary | std::ios::trunc);
            o << text;
            if (!o.good()) { Err("Could not write " + ToUtf8(sumOut.wstring()) + "\n"); return 1; }
            Err("Written: " + ToUtf8(sumOut.wstring()) + "\n");
        }
        return rep.failed ? 1 : 0;
    } catch (const Error& e) {
        if (tty) Err("\r    \r");
        Err(std::string(e.cancelled() ? "Cancelled" : "Error: ") + (e.cancelled() ? "" : e.what()) + "\n");
        return 1;
    }
}

}  // namespace

static int CliMain(int argc, wchar_t** argv) {
    // The CLI must be usable in scripts / CI: no error may open a dialog and wait for a person
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout), _O_BINARY);

    // worker process of the task executor (see lycan/taskexec.h): lycanzip-cli --worker <read handle> <write handle>
    if (argc == 4 && std::wstring(argv[1]) == L"--worker")
        return lycan::RunWorker(static_cast<uintptr_t>(_wcstoui64(argv[2], nullptr, 10)), static_cast<uintptr_t>(_wcstoui64(argv[3], nullptr, 10)));

    if (argc == 2 && (std::wstring(argv[1]) == L"info" || std::wstring(argv[1]) == L"--version")) {
        Out("lycanzip-cli " LYCAN_VERSION_STRING "\n");
        if (std::wstring(argv[1]) == L"--version") return 0;
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        std::filesystem::path dir = std::filesystem::path(std::wstring(exe, n)).parent_path();
        try {
            auto dll = SevenZipDllPath();
            std::string ver;
            DWORD h = 0, size = GetFileVersionInfoSizeW(dll.c_str(), &h);
            if (size) {
                std::vector<char> buf(size);
                VS_FIXEDFILEINFO* fi = nullptr;
                UINT len = 0;
                if (GetFileVersionInfoW(dll.c_str(), 0, size, buf.data()) &&
                    VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) && fi)
                {
                    char v[32];
                    snprintf(v, sizeof(v), "%u.%02u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS));  // the way 7-Zip writes it, like 26.03
                    ver = v;
                }
            }
            Out("7z.dll: " + ToUtf8(dll.wstring()) + (ver.empty() ? "" : "  (7-Zip " + ver + ")") + "\n");
        } catch (const Error& e) {
            Out(std::string("7z.dll: not found (") + e.what() + ")\n");
        }
        std::error_code ec;
        bool portable = std::filesystem::exists(dir / L"LycanZip.portable", ec);
        Out("Portable mode: " + std::string(portable ? "yes" : "no") + "\n");
        Out("Settings file: " + ToUtf8(DefaultSettingsPath().wstring()) + "\n");
        Out("Log folder: " + ToUtf8(diag::Dir().wstring()) + (diag::Detailed() ? " (detailed log: on)" : " (detailed log: off; set the environment variable LYCAN_LOG=debug to switch it on)") + "\n");
        return 0;
    }
    if (argc < 3) { Usage(); return 2; }

    std::wstring cmd = argv[1];
    if (cmd == L"hash") return HashCommand(argc, argv);
    if (cmd == L"verify") return VerifyCommand(argc, argv);
    std::filesystem::path archive = argv[2];
    std::filesystem::path outDir;
    std::wstring password;
    bool hasPassword = false;
    bool smart = false, noSmart = false, keepBroken = true;
    unsigned nameCp = 0;  // 0 = detect the file name encoding automatically
    OverwriteMode ow = OverwriteMode::Fail;
    int positional = 3;
    std::wstring catIndex;
    CompressOptions addOpt;
    std::vector<std::wstring> addInputs;
    bool typeGiven = false;
    bool sumsArchive = false, sumsFiles = false;
    uint64_t maxSize = 0;      // extraction limit (bytes), 0 = none
    uint32_t maxRatio = 0;     // expansion / compression ratio limit, 0 = none
    UpdateOptions updOpt;
    std::vector<UpdateOp> updOps;
    std::wstring updInto;
    bool estimateOnly = false, dryRun = false;
    std::filesystem::path convertOut;   // convert: the new archive
    std::wstring newPassword;
    bool newPasswordGiven = false, noPassword = false, skipDamaged = false, noTest = false;
    const bool packs = cmd == L"a" || cmd == L"convert";   // takes the options of a new archive (addOpt)

    for (int i = 3; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"-o" && i + 1 < argc) outDir = argv[++i];
        else if (a == L"-p" && i + 1 < argc) { password = argv[++i]; hasPassword = true; }
        else if (a == L"--encoding" && i + 1 < argc) {
            std::wstring e = argv[++i];
            for (auto& c : e) c = static_cast<wchar_t>(towlower(c));
            if (e == L"utf8" || e == L"utf-8") nameCp = 65001;
            else if (e == L"big5") nameCp = 950;
            else if (e == L"gbk" || e == L"gb2312") nameCp = 936;
            else if (e == L"sjis" || e == L"shift-jis") nameCp = 932;
            else if (e == L"euckr" || e == L"kr") nameCp = 949;
            else if (e == L"oem") nameCp = GetOEMCP();
            else if (e != L"auto") nameCp = static_cast<unsigned>(_wtoi(e.c_str()));
            if (e != L"auto" && !nameCp) { Usage(); return 2; }
        }
        else if (cmd == L"x" && a == L"--max-size" && i + 1 < argc) {
            if (!ParseSize(argv[++i], maxSize)) { Err("Cannot read --max-size (for example 500m, 10g)\n"); return 2; }
        }
        else if (cmd == L"x" && a == L"--max-ratio" && i + 1 < argc) {
            const int r = _wtoi(argv[++i]);
            if (r <= 0) { Err("--max-ratio must be a positive whole number (for example 1000)\n"); return 2; }
            maxRatio = static_cast<uint32_t>(r);
        }
        else if (cmd == L"x" && a == L"--discard-broken") keepBroken = false;
        else if (cmd == L"x" && a == L"--keep-broken") keepBroken = true;
        else if (a == L"--smart") smart = true;
        else if (a == L"--no-smart") noSmart = true;
        else if (a == L"-y") ow = OverwriteMode::Overwrite;
        else if (a == L"-s") ow = OverwriteMode::Skip;
        else if (a == L"-r") ow = OverwriteMode::Rename;
        else if ((packs || cmd == L"u") && a == L"-l" && i + 1 < argc) (packs ? addOpt : updOpt.compress).level = _wtoi(argv[++i]);
        else if ((packs || cmd == L"u") && a == L"--profile" && i + 1 < argc) {
            std::wstring v = argv[++i];
            CompressOptions& co = packs ? addOpt : updOpt.compress;
            if (v == L"compatible") co.profile = Profile::Compatible;
            else if (v == L"balanced") co.profile = Profile::Balanced;
            else if (v == L"smallest") { co.profile = Profile::Smallest; co.storeCompressed = false; }
            else if (v == L"fastest") co.profile = Profile::Fastest;
            else { Usage(); return 2; }
        }
        else if ((packs || cmd == L"u") && a == L"--no-store-compressed") (packs ? addOpt : updOpt.compress).storeCompressed = false;
        else if (cmd == L"convert" && a == L"--new-password" && i + 1 < argc) { newPassword = argv[++i]; newPasswordGiven = true; }
        else if (cmd == L"convert" && a == L"--no-password") noPassword = true;
        else if (cmd == L"convert" && a == L"--skip-damaged") skipDamaged = true;
        else if (cmd == L"convert" && a == L"--no-test") noTest = true;
        else if (cmd == L"u" && a == L"--backup") updOpt.keepBackup = true;
        else if (cmd == L"u" && a == L"--estimate") estimateOnly = true;
        else if (cmd == L"u" && a == L"--dry-run") dryRun = true;
        else if (cmd == L"u" && a == L"--mode" && i + 1 < argc) {
            const std::wstring v = argv[++i];
            if (v == L"replace") updOpt.mode = UpdateMode::AddReplace;
            else if (v == L"update") updOpt.mode = UpdateMode::UpdateAdd;
            else if (v == L"freshen") updOpt.mode = UpdateMode::Freshen;
            else if (v == L"sync") updOpt.mode = UpdateMode::Synchronize;
            else { Usage(); return 2; }
        }
        else if (cmd == L"u" && a == L"--into" && i + 1 < argc) updInto = argv[++i];
        else if (cmd == L"u" && a == L"--add" && i + 1 < argc) {
            const std::filesystem::path in = argv[++i];
            if (!updOps.empty() && updOps.back().kind == UpdateOp::Kind::Add && updOps.back().path == updInto) updOps.back().inputs.push_back(in);
            else updOps.push_back(UpdateOp::Add(updInto, {in}));
        }
        else if (cmd == L"u" && a == L"--delete" && i + 1 < argc) updOps.push_back(UpdateOp::Delete(argv[++i]));
        else if (cmd == L"u" && a == L"--mkdir" && i + 1 < argc) updOps.push_back(UpdateOp::MakeDir(argv[++i]));
        else if (cmd == L"u" && a == L"--rename" && i + 2 < argc) { std::wstring from = argv[++i]; updOps.push_back(UpdateOp::Rename(from, argv[++i])); }
        else if (cmd == L"u" && a == L"--replace" && i + 2 < argc) { std::wstring path = argv[++i]; updOps.push_back(UpdateOp::Replace(path, std::filesystem::path(argv[++i]))); }
        else if (packs && a.rfind(L"-v", 0) == 0 && (a.size() > 2 || i + 1 < argc)) {  // -v50m or -v 50m
            std::wstring v = a.size() > 2 ? a.substr(2) : std::wstring(argv[++i]);
            if (!ParseSize(v, addOpt.volumeSize)) { Err("Cannot read the volume size (for example 50k, 100m, 4.7g)\n"); return 2; }
        }
        else if (cmd == L"a" && a == L"--encrypt-names") addOpt.encryptFileNames = true;
        else if (cmd == L"a" && a == L"--iso-label" && i + 1 < argc) addOpt.isoLabel = argv[++i];
        else if (cmd == L"a" && a == L"--iso-now") addOpt.isoKeepTimes = false;
        else if (cmd == L"a" && a == L"--sums-archive") sumsArchive = true;
        else if (cmd == L"a" && a == L"--sums-files") sumsFiles = true;
        else if (packs && a == L"-t" && i + 1 < argc) {
            std::wstring v = argv[++i];
            if (v == L"zip") addOpt.type = ArchiveType::Zip;
            else if (v == L"7z") addOpt.type = ArchiveType::SevenZip;
            else if (v == L"tar") addOpt.type = ArchiveType::Tar;
            else if (v == L"iso") addOpt.type = ArchiveType::Iso;
            else if (v == L"tgz" || v == L"tar.gz") addOpt.type = ArchiveType::TarGz;
            else if (v == L"txz" || v == L"tar.xz") addOpt.type = ArchiveType::TarXz;
            else if (v == L"tbz2" || v == L"tar.bz2") addOpt.type = ArchiveType::TarBz2;
            else { Usage(); return 2; }
            typeGiven = true;
        }
        else if (cmd == L"a" && !a.empty() && a[0] != L'-') addInputs.push_back(a);
        else if (cmd == L"convert" && convertOut.empty() && !a.empty() && a[0] != L'-') convertOut = a;
        else if (positional == 3 && cmd == L"cat") { catIndex = a; positional = 4; }
        else { Usage(); return 2; }
    }
    PasswordFn pw;
    if (hasPassword) pw = [&](std::wstring& out) { out = password; return true; };

    if (cmd == L"a") {
        try {
            if (addInputs.empty()) { Usage(); return 2; }
            if (!typeGiven && !GuessArchiveType(archive, addOpt.type)) {
                Err("The format cannot be told from the extension; give it with -t (zip / 7z / tar / tgz / txz / tbz2)\n");
                return 2;
            }
            addOpt.password = password;
            addOpt.overwrite = ow == OverwriteMode::Overwrite;
            addOpt.progress = [](uint64_t done, uint64_t total) {
                if (total) fprintf(stderr, "\r%3d%%", static_cast<int>(std::min<uint64_t>(done * 100 / total, 100)));
                return true;
            };
            std::vector<std::filesystem::path> inputs(addInputs.begin(), addInputs.end());
            auto rep = CreateArchive(archive, inputs, addOpt);
            Err("\n");
            Out("Files " + std::to_string(rep.files) + ", folders " + std::to_string(rep.dirs) + ", original " +
                std::to_string(rep.bytesIn) + " bytes -> " + std::to_string(rep.bytesOut) + " bytes\n");
            if (rep.storedFiles) Out("Stored without compression (already compressed files): " + std::to_string(rep.storedFiles) + "\n");
            for (auto& o : rep.outputs) Out("Output: " + ToUtf8(o.wstring()) + "\n");
            for (auto& s : rep.skipped) Out("Skipped: " + ToUtf8(s) + "\n");
            for (auto& s : rep.warnings) Out("Warning: " + ToUtf8(s) + "\n");
            for (auto& s : rep.errors) Out("Error: " + ToUtf8(s) + "\n");
            if (rep.ok() && (sumsArchive || sumsFiles) && !rep.outputs.empty()) {
                // volumes: the checksum file of the archive has one line per volume
                auto sw = WriteArchiveSums(rep.outputs[0], inputs, sumsArchive, sumsFiles, {}, rep.outputs);
                for (auto& o : sw.written) Out("Checksum file: " + ToUtf8(o.wstring()) + "\n");
                if (sw.skipped) Out("Not in the checksum file (could not be read): " + std::to_string(sw.skipped) + "\n");
            }
            return rep.ok() ? 0 : 1;
        } catch (const Error& e) {
            Err(std::string("\nError: ") + e.what() + "\n");
            return e.cancelled() ? 3 : 1;
        }
    }

    if (cmd == L"convert" || cmd == L"unprotect") {
        try {
            RepackOptions ro;
            ro.open.password = pw;
            ro.open.nameCodepage = nameCp;
            ro.compress = addOpt;
            ro.skipDamaged = skipDamaged;
            ro.test = !noTest;
            ro.progress = [](uint64_t done, uint64_t total) {
                if (total) fprintf(stderr, "\r%3d%%", static_cast<int>(std::min<uint64_t>(done * 100 / total, 100)));
                return true;
            };
            if (cmd == L"unprotect") {
                if (!hasPassword) { Usage(); return 2; }
                auto src = ArchiveSource::Open(archive, [&] { OpenOptions o; o.password = pw; return o; }());
                if (!WritableTypeOf(*src, ro.compress.type)) { Err("This format cannot be written: convert it to another one instead\n"); return 1; }
                src.reset();
                ro.inPlace = true;
                ro.compress.password.clear();
            } else {
                if (convertOut.empty()) { Usage(); return 2; }
                if (!typeGiven && !GuessArchiveType(convertOut, ro.compress.type)) {
                    Err("The format cannot be told from the extension; give it with -t (zip / 7z / tar / tgz / txz / tbz2)\n");
                    return 2;
                }
                ro.compress.password = noPassword ? L"" : newPasswordGiven ? newPassword : password;
                ro.compress.overwrite = ow == OverwriteMode::Overwrite;
            }
            auto rep = RepackArchive(archive, convertOut, ro);
            Err("\n");
            Out("Files " + std::to_string(rep.files) + ", folders " + std::to_string(rep.dirs) + "; " + std::to_string(rep.archiveBefore) + " -> " +
                std::to_string(rep.archiveAfter) + " bytes" + (rep.tested ? ", tested" : "") + "\n");
            for (auto& o : rep.outputs) Out("Output: " + ToUtf8(o.wstring()) + "\n");
            for (auto& s : rep.skipped) Out("Left out: " + ToUtf8(s) + "\n");
            for (auto& s : rep.warnings) Out("Warning: " + ToUtf8(s) + "\n");
            for (auto& s : rep.errors) Out("Error: " + ToUtf8(s) + "\n");
            return rep.ok() ? 0 : 1;
        } catch (const Error& e) {
            Err(std::string("\nError: ") + e.what() + "\n");
            return e.cancelled() ? 3 : 1;
        }
    }

    if (cmd == L"upgrade") {
        try {
            UpdateOptions uo;
            uo.compress.password = password;
            uo.nameCodepage = nameCp;
            uo.rewriteNames = true;
            uo.verify = true;
            auto rep = UpdateArchive(archive, {}, uo);
            if (!rep.changed) { Out("Nothing to upgrade: the names are marked as UTF-8 already (or are plain ASCII)\n"); return 0; }
            Out("Upgraded: the names are in UTF-8 now; " + std::to_string(rep.archiveBefore) + " -> " + std::to_string(rep.archiveAfter) + " bytes\n");
            return rep.ok() ? 0 : 1;
        } catch (const Error& e) {
            Err(std::string("\nError: ") + e.what() + "\n");
            return e.cancelled() ? 3 : 1;
        }
    }

    if (cmd == L"u") {
        try {
            if (updOps.empty()) { Usage(); return 2; }
            updOpt.compress.password = password;
            updOpt.progress = [](uint64_t done, uint64_t total) {
                if (total) fprintf(stderr, "\r%3d%%", static_cast<int>(std::min<uint64_t>(done * 100 / total, 100)));
                return true;
            };
            if (estimateOnly) {
                auto c = EstimateUpdate(archive, updOps, updOpt);
                Out("Archive " + std::to_string(c.archiveBytes) + " bytes (it is written once more); new data " + std::to_string(c.newBytes) +
                    " bytes; old data to compress again " + std::to_string(c.recompressBytes) + " bytes\n");
                return 0;
            }
            auto summary = [](const UpdateReport& r) {
                std::string s = "added " + std::to_string(r.added) + ", replaced " + std::to_string(r.replaced) + ", deleted " + std::to_string(r.deleted) +
                                ", renamed " + std::to_string(r.renamed) + ", new folders " + std::to_string(r.dirsCreated);
                if (r.unchanged) s += ", unchanged " + std::to_string(r.unchanged);
                if (r.notAdded) s += ", new files not added " + std::to_string(r.notAdded);
                return s;
            };
            if (dryRun) {
                auto rep = PreviewUpdate(archive, updOps, updOpt);
                Out("Preview (the archive is not changed): " + summary(rep) + "\n");
                for (auto& s : rep.deletedPaths) Out("Would delete: " + ToUtf8(s) + "\n");
                for (auto& s : rep.skipped) Out("Skipped: " + ToUtf8(s) + "\n");
                for (auto& s : rep.warnings) Out("Warning: " + ToUtf8(s) + "\n");
                for (auto& s : rep.errors) Out("Error: " + ToUtf8(s) + "\n");
                return rep.ok() ? 0 : 1;
            }
            auto rep = UpdateArchive(archive, updOps, updOpt);
            Err("\n");
            if (!rep.changed) { Out("No change" + std::string(rep.unchanged ? " (" + std::to_string(rep.unchanged) + " files are unchanged)" : "") + "\n"); return rep.ok() ? 0 : 1; }
            Out(summary(rep) + "; " + std::to_string(rep.archiveBefore) + " → " + std::to_string(rep.archiveAfter) + " bytes\n");
            for (auto& s : rep.deletedPaths) Out("Deleted: " + ToUtf8(s) + "\n");
            if (rep.storedFiles) Out("Stored without compression (already compressed files): " + std::to_string(rep.storedFiles) + "\n");
            for (auto& s : rep.skipped) Out("Skipped: " + ToUtf8(s) + "\n");
            for (auto& s : rep.warnings) Out("Warning: " + ToUtf8(s) + "\n");
            for (auto& s : rep.errors) Out("Error: " + ToUtf8(s) + "\n");
            return rep.ok() ? 0 : 1;
        } catch (const Error& e) {
            Err(std::string("\nError: ") + e.what() + "\n");
            return e.cancelled() ? 3 : 1;
        }
    }

    try {
        OpenOptions oo;
        oo.password = pw;
        oo.nameCodepage = nameCp;
        auto src = ArchiveSource::Open(archive, oo);

        if (cmd == L"l") {
            auto caps = src->Caps();
            Out("Format: " + ToUtf8(src->FormatName()) + (caps.solid ? "  [solid]" : "") +
                (caps.encrypted ? "  [encrypted]" : "") + "\n");
            if (auto ne = src->NameEncoding(); ne.applicable)
                Out("Name encoding: " + ToUtf8(CodepageName(ne.codepage)) +
                    (ne.autoDetected ? (ne.ambiguous ? "  [detected automatically, not sure: set it with --encoding]" : "  [detected automatically]") : "  [given]") + "\n");
            Out("  #\tsize\tpacked\tpath\n");
            for (auto& e : src->Entries()) {
                Out(std::to_string(e.index) + "\t" + (e.isDir ? "<DIR>" : std::to_string(e.size)) + "\t" +
                    std::to_string(e.packedSize) + "\t" + ToUtf8(e.path) + (e.isEncrypted ? " *" : "") +
                    (e.isSymlink ? " ->link" : "") + "\n");
            }
            Out(std::to_string(src->Entries().size()) + " items\n");
            return 0;
        }

        if (cmd == L"cat") {
            if (catIndex.empty()) { Usage(); return 2; }
            StdoutSink sink;
            src->ReadEntry(static_cast<uint32_t>(std::stoul(catIndex)), sink);
            return 0;
        }

        if (cmd == L"t") {
            // 7-Zip native test mode (one pass, also for solid archives)
            ExtractOptions to;
            to.password = pw;
            to.progress = [](uint64_t done, uint64_t total) {
                if (total) fprintf(stderr, "\r%3d%%", static_cast<int>(done * 100 / total));
                return true;
            };
            auto rep = TestArchive(*src, to);
            Err("\n");
            for (auto& s : rep.warnings) Err("Warning " + ToUtf8(s) + "\n");
            for (auto& s : rep.errors) Err("FAIL " + ToUtf8(s) + "\n");
            Out(rep.ok() ? "OK\n" : "Test failed\n");
            return rep.ok() ? 0 : 1;
        }

        if (cmd == L"scan") {
            // every file is unpacked into memory and handed to the installed antimalware product (AMSI)
            ScanOptions so;
            so.password = pw;
            so.progress = [](uint64_t done, uint64_t total) {
                if (total) fprintf(stderr, "\r%3d%%", static_cast<int>(std::min<uint64_t>(done * 100 / total, 100)));
                return true;
            };
            auto rep = ScanArchive(*src, so);
            Err("\n");
            if (rep.amsiHr) {
                const AmsiProblem p = ClassifyAmsiError(rep.amsiHr);
                Err(p == AmsiProblem::RealTimeOff ? "Cannot scan: real-time protection is off (switch it on in Windows Security)\n"
                    : p == AmsiProblem::NoProvider ? "Cannot scan: the installed antivirus does not support AMSI (Windows Defender does)\n"
                                                   : "Cannot scan: AMSI cannot be used on this computer\n");
                return 1;
            }
            Out("Scanned " + std::to_string(rep.files) + " files (" + std::to_string(rep.bytes) + " bytes)\n");
            if (rep.skippedLarge) Out("Skipped (over 128 MB): " + std::to_string(rep.skippedLarge) + "\n");
            if (rep.skippedEncrypted) Out("Skipped (encrypted, no password): " + std::to_string(rep.skippedEncrypted) + "\n");
            if (rep.unreadable) Out("Cannot be read: " + std::to_string(rep.unreadable) + "\n");
            if (rep.scanErrors) Out("Scan failed: " + std::to_string(rep.scanErrors) + "\n");
            for (auto& t : rep.threats) {
                std::string line = std::string(t.blocked ? "Blocked: " : "Found: ") + ToUtf8(t.path);
                if (!t.name.empty()) {
                    line += "  ->  " + ToUtf8(t.name);
                    if (!t.severity.empty() || !t.category.empty()) line += " (" + ToUtf8(t.severity) + (t.severity.empty() || t.category.empty() ? "" : ", ") + ToUtf8(t.category) + ")";
                }
                Out(line + "\n");
            }
            {
                bool unmatched = false;
                for (auto& t : rep.threats) if (!t.blocked && t.name.empty()) unmatched = true;
                if (unmatched && !rep.threatNames.empty()) {
                    std::string all;
                    for (auto& n : rep.threatNames) all += (all.empty() ? "" : ", ") + ToUtf8(n);
                    Out("Threat names recorded by Windows Defender: " + all + "\n");
                }
            }
            Out(rep.Found() ? "Suspicious files were found\n" : "No threat found (this does not mean it is safe: the installed antivirus found nothing)\n");
            return rep.Found() ? 5 : 0;
        }

        if (cmd == L"x") {
            // Without -o: extract to the folder of the archive with smart extraction; with -o: put it exactly into that folder (unless --smart is added)
            ExtractOptions eo;
            if (outDir.empty()) {
                outDir = archive.has_parent_path() ? archive.parent_path() : std::filesystem::path(L".");
                eo.smart = !noSmart;
            } else {
                eo.smart = smart;
            }
            eo.overwrite = ow;
            eo.keepDamaged = keepBroken;
            eo.password = pw;
            eo.maxRatio = maxRatio;
            eo.maxTotalBytes = maxSize;
            eo.progress = [](uint64_t done, uint64_t total) {
                if (total) fprintf(stderr, "\r%3d%%", static_cast<int>(done * 100 / total));
                return true;
            };
            auto rep = ExtractTo(*src, outDir, eo);
            Err("\n");
            Out("Output: " + ToUtf8(rep.outputDir.wstring()) + "\n");
            Out("Files " + std::to_string(rep.files) + ", folders " + std::to_string(rep.dirs) + ", " +
                std::to_string(rep.bytes) + " bytes\n");
            for (auto& s : rep.rejected) Out("Rejected: " + ToUtf8(s) + "\n");
            for (auto& s : rep.renamed) Out("Renamed: " + ToUtf8(s) + "\n");
            for (auto& s : rep.skipped) Out("Skipped: " + ToUtf8(s) + "\n");
            for (auto& s : rep.warnings) Out("Warning: " + ToUtf8(s) + "\n");
            for (auto& s : rep.kept) Out("Kept (with an error): " + ToUtf8(s) + "\n");
            for (auto& s : rep.errors) Out("Error: " + ToUtf8(s) + "\n");
            return rep.ok() ? 0 : 1;
        }

        Usage();
        return 2;
    } catch (const Error& e) {
        Err(std::string("Error: ") + e.what() + "\n");
        return e.cancelled() ? 3 : 1;
    } catch (const std::exception& e) {
        Err(std::string("Exception: ") + e.what() + "\n");
        return 1;
    }
}

int wmain(int argc, wchar_t** argv) {
    const bool worker = argc == 4 && std::wstring(argv[1]) == L"--worker";
    diag::Init(worker ? L"worker" : L"cli");  // (a worker process also does this itself)
    if (!worker && argc >= 2) diag::Info(L"cli: {}", std::wstring(argv[1]));
    const int rc = CliMain(argc, argv);
    if (!worker && rc != 0 && argc >= 2) diag::Warn(L"cli: {} ended with exit code {}", std::wstring(argv[1]), rc);
    return rc;
}
