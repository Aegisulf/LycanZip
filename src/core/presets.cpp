#include "lycan/presets.h"

#include <windows.h>

#include <algorithm>
#include <fstream>

#include "lycan/error.h"
#include "lycan/settings.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string Trim(std::string s) {
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(s.front())) s.erase(s.begin());
    while (!s.empty() && ws(s.back())) s.pop_back();
    return s;
}

struct FormatName { ArchiveType t; const char* s; };
constexpr FormatName kFormats[] = {{ArchiveType::Zip, "zip"}, {ArchiveType::SevenZip, "7z"}, {ArchiveType::TarGz, "tar.gz"},
                                   {ArchiveType::TarXz, "tar.xz"}, {ArchiveType::TarBz2, "tar.bz2"}, {ArchiveType::Tar, "tar"}, {ArchiveType::Iso, "iso"}};
struct ProfileName { Profile p; const char* s; };
constexpr ProfileName kProfiles[] = {{Profile::Balanced, "balanced"}, {Profile::Compatible, "compatible"},
                                     {Profile::Smallest, "smallest"}, {Profile::Fastest, "fastest"}};

bool ToBool(const std::string& s, bool d) {
    if (s == "true" || s == "1") return true;
    if (s == "false" || s == "0") return false;
    return d;
}

bool ToInt(const std::string& s, long long lo, long long hi, long long& out) {
    try {
        size_t used = 0;
        long long v = std::stoll(s, &used);
        if (used != s.size() || v < lo || v > hi) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

// Method names are accepted only from the list of the format (the content of the file must not become an injection point for any string)
std::wstring CleanMethod(const std::wstring& m, ArchiveType t) {
    for (auto& ok : MethodsFor(t)) if (_wcsicmp(ok.c_str(), m.c_str()) == 0) return ok;
    return L"";
}

}  // namespace

bool CompressPreset::SameSettings(const CompressPreset& o) const {
    return type == o.type && profile == o.profile && level == o.level && method == o.method && dictSize == o.dictSize &&
           wordSize == o.wordSize && solidSize == o.solidSize && zipEncryption == o.zipEncryption &&
           encryptFileNames == o.encryptFileNames && volumeSize == o.volumeSize && threads == o.threads &&
           pathMode == o.pathMode && customParams == o.customParams && storeCompressed == o.storeCompressed &&
           sumsArchive == o.sumsArchive && sumsFiles == o.sumsFiles;
}

std::vector<CompressPreset> BuiltinPresets() {
    std::vector<CompressPreset> v;
    auto add = [&](const wchar_t* name, ArchiveType t, Profile p, auto&& tweak) {
        CompressPreset c;
        c.name = name;
        c.builtin = true;
        c.type = t;
        c.profile = p;
        tweak(c);
        v.push_back(std::move(c));
    };
    auto none = [](CompressPreset&) {};
    add(L"General ZIP", ArchiveType::Zip, Profile::Balanced, none);
    add(L"General 7z", ArchiveType::SevenZip, Profile::Balanced, none);
    add(L"Smallest size", ArchiveType::SevenZip, Profile::Smallest, [](CompressPreset& c) { c.solidSize = kSolidAll; c.storeCompressed = false; });
    add(L"Fast", ArchiveType::Zip, Profile::Fastest, none);
    add(L"Email / share", ArchiveType::Zip, Profile::Balanced, [](CompressPreset& c) { c.volumeSize = 25ull << 20; c.sumsArchive = true; });   // the receiver can check every volume
    add(L"Backup", ArchiveType::SevenZip, Profile::Balanced, [](CompressPreset& c) { c.encryptFileNames = true; c.sumsArchive = true; c.sumsFiles = true; });
    add(L"Store only, no compression", ArchiveType::Zip, Profile::Balanced, [](CompressPreset& c) { c.level = 0; });
    return v;
}

fs::path DefaultPresetsPath() { return DefaultSettingsPath().parent_path() / L"presets.ini"; }

bool IsValidPresetName(const std::wstring& name) {
    if (name.empty() || name.size() > 40) return false;
    for (wchar_t c : name) if (c == L'[' || c == L']' || c < 0x20) return false;
    return name.front() != L' ' && name.back() != L' ';
}

std::vector<CompressPreset> LoadPresets(const fs::path& file) {
    std::vector<CompressPreset> out;
    std::ifstream in(file, std::ios::binary);
    if (!in) return out;
    auto builtins = BuiltinPresets();
    CompressPreset cur;
    bool have = false;
    auto flush = [&] {
        if (!have) return;
        have = false;
        if (!IsValidPresetName(cur.name)) return;
        for (auto& b : builtins) if (b.name == cur.name) return;
        for (auto& o : out) if (o.name == cur.name) return;
        // fields that do not fit the format go back to the default instead of making the compression fail
        cur.method = CleanMethod(cur.method, cur.type);
        if (cur.type != ArchiveType::SevenZip) { cur.solidSize = kSolidAuto; cur.encryptFileNames = false; }
        if (cur.type != ArchiveType::Zip) cur.zipEncryption.clear();
        if (cur.type != ArchiveType::Zip && cur.type != ArchiveType::SevenZip) cur.volumeSize = 0;
        out.push_back(cur);
    };
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        first = false;
        line = Trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            flush();
            cur = CompressPreset{};
            cur.name = FromUtf8(line.substr(1, line.size() - 2));
            have = true;
            continue;
        }
        if (!have) continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        long long n = 0;
        if (k == "format") { for (auto& f : kFormats) if (v == f.s) cur.type = f.t; }
        else if (k == "profile") { for (auto& p : kProfiles) if (v == p.s) cur.profile = p.p; }
        else if (k == "level") { if (ToInt(v, -1, 9, n)) cur.level = static_cast<int>(n); }
        else if (k == "method") cur.method = FromUtf8(v);
        else if (k == "dict") { if (ToInt(v, 0, 1ll << 40, n)) cur.dictSize = static_cast<uint64_t>(n); }
        else if (k == "word") { if (ToInt(v, 0, 1024, n)) cur.wordSize = static_cast<unsigned>(n); }
        else if (k == "solid") { if (v == "all") cur.solidSize = kSolidAll; else if (v == "off") cur.solidSize = kSolidOff; else if (ToInt(v, -1, 1ll << 44, n)) cur.solidSize = n; }
        else if (k == "zip_encryption") { if (v == "AES256" || v == "ZipCrypto") cur.zipEncryption = FromUtf8(v); }
        else if (k == "encrypt_names") cur.encryptFileNames = ToBool(v, false);
        else if (k == "volume") { if (ToInt(v, 0, 1ll << 44, n)) cur.volumeSize = static_cast<uint64_t>(n); }
        else if (k == "threads") { if (ToInt(v, 0, 256, n)) cur.threads = static_cast<unsigned>(n); }
        else if (k == "path_mode") cur.pathMode = v == "full" ? PathMode::Full : PathMode::Relative;
        else if (k == "params") cur.customParams = FromUtf8(v);
        else if (k == "store_compressed") cur.storeCompressed = ToBool(v, true);
        else if (k == "in_menu") cur.inMenu = ToBool(v, true);
        else if (k == "sums_archive") cur.sumsArchive = ToBool(v, false);
        else if (k == "sums_files") cur.sumsFiles = ToBool(v, false);
    }
    flush();
    return out;
}

bool SavePresets(const fs::path& file, const std::vector<CompressPreset>& list) {
    std::error_code ec;
    if (file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    fs::path tmp = file;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (auto& p : list) {
            if (p.builtin || !IsValidPresetName(p.name)) continue;
            const char* fmt = "zip";
            for (auto& f : kFormats) if (f.t == p.type) fmt = f.s;
            const char* prof = "balanced";
            for (auto& x : kProfiles) if (x.p == p.profile) prof = x.s;
            out << "\n[" << ToUtf8(p.name) << "]\n"
                << "format=" << fmt << "\n"
                << "profile=" << prof << "\n"
                << "level=" << p.level << "\n"
                << "method=" << ToUtf8(p.method) << "\n"
                << "dict=" << p.dictSize << "\n"
                << "word=" << p.wordSize << "\n"
                << "solid=" << (p.solidSize == kSolidAll ? std::string("all") : std::to_string(p.solidSize)) << "\n"
                << "zip_encryption=" << ToUtf8(p.zipEncryption) << "\n"
                << "encrypt_names=" << (p.encryptFileNames ? "true" : "false") << "\n"
                << "volume=" << p.volumeSize << "\n"
                << "threads=" << p.threads << "\n"
                << "path_mode=" << (p.pathMode == PathMode::Full ? "full" : "relative") << "\n"
                << "store_compressed=" << (p.storeCompressed ? "true" : "false") << "\n"
                << "sums_archive=" << (p.sumsArchive ? "true" : "false") << "\n"
                << "sums_files=" << (p.sumsFiles ? "true" : "false") << "\n"
                << "in_menu=" << (p.inMenu ? "true" : "false") << "\n"
                << "params=" << ToUtf8(p.customParams) << "\n";
        }
        if (!out.good()) return false;
    }
    fs::remove(file, ec);
    fs::rename(tmp, file, ec);
    return !ec;
}

void ApplyPreset(const CompressPreset& p, CompressOptions& o) {
    o.type = p.type;
    o.profile = p.profile;
    o.level = p.level;
    o.method = p.method;
    o.dictSize = p.dictSize;
    o.wordSize = p.wordSize;
    o.solidSize = p.solidSize;
    o.zipEncryption = p.zipEncryption;
    o.encryptFileNames = p.encryptFileNames;
    o.volumeSize = p.volumeSize;
    o.threads = p.threads;
    o.pathMode = p.pathMode;
    o.customParams = p.customParams;
    o.storeCompressed = p.storeCompressed;
}

}  // namespace lycan
