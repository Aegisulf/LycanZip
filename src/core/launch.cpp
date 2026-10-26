#include "lycan/launch.h"

#include "lycan/checksum.h"
#include "lycan/shellitems.h"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace lycan {
namespace {

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The context menu extension writes %TEMP%\LycanZip-<guid>.lst when there are many paths; it is deleted after reading (only a file with this naming
// that is inside %TEMP% is deleted, so a --list file given by the user is never removed).
bool IsShellExtTempList(const std::filesystem::path& file) {
    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) return false;
    std::error_code ec;
    auto parent = std::filesystem::weakly_canonical(file.parent_path(), ec);
    auto temp = std::filesystem::weakly_canonical(std::filesystem::path(tmp), ec);
    std::wstring name = file.filename().wstring();
    return !ec && parent == temp && name.rfind(L"LycanZip-", 0) == 0 && file.extension() == L".lst";
}

bool ReadList(const std::filesystem::path& file, std::vector<std::filesystem::path>& out) {
    {
        std::ifstream in(file, std::ios::binary);
        if (!in) return false;
        std::string line;
        bool first = true;
        while (std::getline(in, line)) {
            if (first && line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);  // BOM
            first = false;
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) out.emplace_back(FromUtf8(line));
        }
    }
    if (IsShellExtTempList(file)) {
        std::error_code ec;
        std::filesystem::remove(file, ec);
    }
    return true;
}

bool IEquals(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

}  // namespace

bool ParseFormatName(const std::wstring& name, ArchiveType& out) {
    struct F { const wchar_t* n; ArchiveType t; };
    static const F kF[] = {{L"zip", ArchiveType::Zip},       {L"7z", ArchiveType::SevenZip},    {L"tar.gz", ArchiveType::TarGz},
                           {L"tar.xz", ArchiveType::TarXz}, {L"tar.bz2", ArchiveType::TarBz2}, {L"iso", ArchiveType::Iso}};
    for (auto& f : kF) if (IEquals(name, f.n)) { out = f.t; return true; }
    return false;
}

const wchar_t* FormatName(ArchiveType t) {
    switch (t) {
        case ArchiveType::SevenZip: return L"7z";
        case ArchiveType::TarGz: return L"tar.gz";
        case ArchiveType::TarXz: return L"tar.xz";
        case ArchiveType::TarBz2: return L"tar.bz2";
        case ArchiveType::Iso: return L"iso";
        default: return L"zip";
    }
}

LaunchRequest ParseLaunchArgs(const std::vector<std::wstring>& args) {
    LaunchRequest r;
    LaunchAction explicitAction = LaunchAction::None;
    bool haveAction = false;
    std::vector<std::filesystem::path> paths;

    struct Simple { const wchar_t* flag; LaunchAction action; };
    static const Simple kSimple[] = {
        {L"--add", LaunchAction::Add},
        {L"--add-encrypted", LaunchAction::AddEncrypted},
        {L"--extract-here", LaunchAction::ExtractHere},
        {L"--extract-folder", LaunchAction::ExtractFolder},
        {L"--extract-to", LaunchAction::ExtractTo},
        {L"--extract-merge", LaunchAction::ExtractMerge},
        {L"--test", LaunchAction::Test},
        {L"--preview-images", LaunchAction::PreviewImages},
        {L"--preview-audio", LaunchAction::PreviewAudio},
        {L"--open-content", LaunchAction::OpenContent},
        {L"--hash", LaunchAction::Hash},
        {L"--verify", LaunchAction::Verify},
        {L"--hash-save", LaunchAction::HashSave},
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a.size() >= 2 && a[0] == L'-' && a[1] == L'-') {
            bool matched = false;
            for (auto& s : kSimple)
                if (IEquals(a, s.flag)) { explicitAction = s.action; haveAction = true; matched = true; break; }
            if (matched) continue;
            if (IEquals(a, L"--compress") || IEquals(a, L"--compress-each")) {
                haveAction = true;
                explicitAction = IEquals(a, L"--compress") ? LaunchAction::Compress : LaunchAction::CompressEach;
                if (i + 1 >= args.size()) { r.valid = false; r.error = L"--compress needs a format (zip, 7z, tar.gz, tar.xz or tar.bz2)"; return r; }
                const std::wstring& f = args[++i];
                if (!ParseFormatName(f, r.format)) { r.valid = false; r.error = L"Unsupported archive format: " + f; return r; }
            } else if (IEquals(a, L"--dest")) {
                if (i + 1 >= args.size()) { r.valid = false; r.error = L"--dest needs a folder"; return r; }
                r.dest = args[++i];
            } else if (IEquals(a, L"--template")) {
                if (i + 1 >= args.size()) { r.valid = false; r.error = L"--template needs a preset name"; return r; }
                r.preset = CanonicalBuiltinPresetName(args[++i]);
            } else if (IEquals(a, L"--list")) {
                if (i + 1 >= args.size()) { r.valid = false; r.error = Message::Make(L"core.launch_list_needs_path"); return r; }
                if (!ReadList(args[++i], paths)) { r.valid = false; r.error = Message::Make(L"core.launch_cannot_read_list", args[i]); return r; }
            } else {
                r.valid = false;
                r.error = Message::Make(L"core.launch_unknown_option", a);
                return r;
            }
        } else if (!a.empty()) {
            paths.emplace_back(a);
        }
    }

    r.paths = std::move(paths);
    if (haveAction) {
        r.action = explicitAction;
        if (r.paths.empty()) { r.valid = false; r.error = Message::Make(L"core.launch_no_files"); }
        else if ((r.action == LaunchAction::PreviewImages || r.action == LaunchAction::PreviewAudio || r.action == LaunchAction::OpenContent) &&
                 r.paths.size() != 1) {
            r.valid = false;
            r.error = L"This option takes exactly one file";
        }
    } else if (!r.paths.empty() && std::all_of(r.paths.begin(), r.paths.end(), [](const std::filesystem::path& p) { return IsSumFile(p); })) {
        r.action = LaunchAction::Verify;   // a checksum file given alone (or several): check it, do not compress it
    } else if (r.paths.size() == 1) {
        r.action = LaunchAction::Open;  // a single path: open (the UI decides whether it is an archive, otherwise it becomes "create an archive")
    } else if (r.paths.size() > 1) {
        r.action = LaunchAction::Add;
    }
    return r;
}

std::wstring SerializeLaunchRequest(const LaunchRequest& r) {
    std::wstring s = std::to_wstring(static_cast<int>(r.action)) + L';' + FormatName(r.format) + L';' + r.preset;
    s += L'\n';
    s += r.dest.wstring();
    for (auto& p : r.paths) {
        s += L'\n';
        s += p.wstring();
    }
    return s;
}

LaunchRequest DeserializeLaunchRequest(const std::wstring& s) {
    LaunchRequest r;
    std::wstringstream ss(s);
    std::wstring line;
    if (!std::getline(ss, line)) { r.valid = false; return r; }
    while (!line.empty() && line.back() == L'\r') line.pop_back();
    try {
        // "action;format;preset" (the format and preset are optional)
        const size_t p1 = line.find(L';');
        int a = std::stoi(line.substr(0, p1));
        if (a < 0 || a > static_cast<int>(LaunchAction::Last)) { r.valid = false; return r; }
        r.action = static_cast<LaunchAction>(a);
        if (p1 != std::wstring::npos) {
            const size_t p2 = line.find(L';', p1 + 1);
            ParseFormatName(line.substr(p1 + 1, p2 == std::wstring::npos ? p2 : p2 - p1 - 1), r.format);
            if (p2 != std::wstring::npos) r.preset = line.substr(p2 + 1);
        }
    } catch (...) { r.valid = false; return r; }
    if (!std::getline(ss, line)) return r;  // no destination line: nothing more
    while (!line.empty() && line.back() == L'\r') line.pop_back();
    r.dest = line;
    while (std::getline(ss, line)) {
        while (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) r.paths.emplace_back(line);
    }
    return r;
}

}  // namespace lycan
