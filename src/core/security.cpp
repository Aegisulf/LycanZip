#include "lycan/security.h"

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

}  // namespace lycan
