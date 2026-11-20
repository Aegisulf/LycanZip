#include "lycan/security.h"
#include "lycan/exectype.h"

#include <algorithm>
#include <array>
#include <cwctype>
#include <vector>

namespace lycan {
namespace {

// The device names Microsoft documents as reserved (COM and LPT also take the superscript digits 1 to 3). Windows ignores spaces
// at the end of the stem when it looks for them ("CON .txt" is CON), so they are ignored here too.
bool IsReservedName(std::wstring_view stem) {
    static constexpr std::array<std::wstring_view, 6> fixed{L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"};
    std::wstring up(stem);
    while (!up.empty() && (up.back() == L' ' || up.back() == L'.')) up.pop_back();
    std::transform(up.begin(), up.end(), up.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    for (auto f : fixed)
        if (up == f) return true;
    if (up.size() == 4 && (up.rfind(L"COM", 0) == 0 || up.rfind(L"LPT", 0) == 0)) {
        const wchar_t d = up[3];
        if ((d >= L'1' && d <= L'9') || d == 0x00B9 || d == 0x00B2 || d == 0x00B3) return true;
    }
    return false;
}

// Cleans one path component; returns whether it was changed.
bool SanitizeComponent(std::wstring& c) {
    bool changed = false;
    for (auto& ch : c) {
        if (ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"' || ch == L'|' || ch == L'?' ||
            ch == L'*') {
            ch = L'_';
            changed = true;
        }
    }
    while (!c.empty() && (c.back() == L' ' || c.back() == L'.')) {
        c.pop_back();
        changed = true;
    }
    if (c.empty()) return changed;
    auto dot = c.find(L'.');
    std::wstring_view stem = dot == std::wstring::npos ? std::wstring_view(c) : std::wstring_view(c).substr(0, dot);
    if (IsReservedName(stem)) {
        c.insert(c.begin(), L'_');
        changed = true;
    }
    return changed;
}

SafePath Reject(const wchar_t* why) {
    SafePath r;
    r.rejected = true;
    r.reason = Message::Make(why);
    return r;
}

}  // namespace

SafePath SanitizeEntryPath(std::wstring_view raw) {
    if (raw.empty()) return Reject(L"core.path_empty");
    if (raw.find(L'\0') != std::wstring_view::npos) return Reject(L"core.path_nul");

    // absolute path / UNC / drive letter / device path
    if (raw.front() == L'/' || raw.front() == L'\\') return Reject(L"core.path_absolute");
    if (raw.size() >= 2 && raw[1] == L':') return Reject(L"core.path_drive");

    std::wstring cur;
    std::vector<std::wstring> parts;
    auto flush = [&] {
        if (!cur.empty()) parts.push_back(cur);
        cur.clear();
    };
    for (wchar_t c : raw) {
        if (c == L'/' || c == L'\\') flush();
        else cur.push_back(c);
    }
    flush();

    bool modified = false;
    std::filesystem::path out;
    for (auto& p : parts) {
        if (p == L".") continue;
        if (p == L"..") return Reject(L"core.path_traversal");
        // NTFS alternate data stream: a colon inside the component (the drive letter was handled before)
        if (p.find(L':') != std::wstring::npos) return Reject(L"core.path_colon");
        if (SanitizeComponent(p)) modified = true;
        if (p.empty()) return Reject(L"core.path_component_empty");
        out /= p;
    }
    if (out.empty()) return Reject(L"core.path_no_component");

    SafePath r;
    r.modified = modified;
    r.relative = std::move(out);
    if (modified) r.reason = Message::Make(L"core.path_renamed");
    return r;
}

std::filesystem::path ToLongPath(const std::filesystem::path& p) {
    // absolute() throws for an empty or malformed path on newer runtimes (an empty path reached here from a caller's mistake): hand
    // such a path back as it is, so that the operation that uses it fails with an ordinary error
    std::error_code ec;
    const std::filesystem::path abs = std::filesystem::absolute(p, ec);
    if (ec) return p;
    std::wstring s = abs.lexically_normal().wstring();
    if (s.rfind(LR"(\\?\)", 0) == 0) return s;
    if (s.rfind(LR"(\\)", 0) == 0) return LR"(\\?\UNC\)" + s.substr(2);
    return LR"(\\?\)" + s;
}

namespace {
const wchar_t* BidiMark(wchar_t c) {
    switch (c) {
        case 0x061C: return L"[ALM]";
        case 0x200E: return L"[LRM]";
        case 0x200F: return L"[RLM]";
        case 0x202A: return L"[LRE]";
        case 0x202B: return L"[RLE]";
        case 0x202C: return L"[PDF]";
        case 0x202D: return L"[LRO]";
        case 0x202E: return L"[RLO]";
        case 0x2066: return L"[LRI]";
        case 0x2067: return L"[RLI]";
        case 0x2068: return L"[FSI]";
        case 0x2069: return L"[PDI]";
        default: return nullptr;
    }
}
}  // namespace

bool HasBidiControls(std::wstring_view name) {
    return std::any_of(name.begin(), name.end(), [](wchar_t c) { return BidiMark(c) != nullptr; });
}

std::wstring VisibleName(std::wstring_view name) {
    if (!HasBidiControls(name)) return std::wstring(name);
    std::wstring out;
    for (wchar_t c : name) {
        if (const wchar_t* m = BidiMark(c)) out += m;
        else out += c;
    }
    return out;
}

bool LooksDisguised(std::wstring_view name) {
    if (HasBidiControls(name)) return true;
    const size_t slash = name.find_last_of(L"/\\");
    std::wstring file(slash == std::wstring_view::npos ? name : name.substr(slash + 1));
    std::transform(file.begin(), file.end(), file.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (!IsExecutableName(file)) return false;
    std::wstring rest = file.substr(0, file.find_last_of(L'.'));
    while (!rest.empty() && (rest.back() == L' ' || rest.back() == L'.')) rest.pop_back();   // "photo.jpg      .scr"
    const size_t dot = rest.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    static const wchar_t* const kDecoys[] = {L"pdf", L"doc", L"docx", L"xls", L"xlsx", L"ppt", L"pptx", L"txt", L"rtf", L"csv", L"htm", L"html",
                                             L"jpg", L"jpeg", L"png", L"gif", L"bmp", L"webp", L"heic", L"mp3", L"wav", L"mp4", L"mkv", L"avi",
                                             L"mov", L"zip", L"rar", L"7z"};
    const std::wstring ext = rest.substr(dot + 1);
    return std::any_of(std::begin(kDecoys), std::end(kDecoys), [&](const wchar_t* d) { return ext == d; });
}

NameWarnings ScanEntryNames(const std::vector<std::wstring>& paths) {
    NameWarnings w;
    for (const auto& p : paths) {
        const bool disguised = LooksDisguised(p);
        const bool unsafe = SanitizeEntryPath(p).rejected;
        if (disguised) ++w.disguised;
        if (unsafe) ++w.unsafePaths;
        if ((disguised || unsafe) && w.example.empty()) w.example = VisibleName(p);
    }
    return w;
}

}  // namespace lycan
