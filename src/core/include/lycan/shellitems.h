#pragma once
// The items of the Explorer context menu: ids, groups and defaults.
// Header-only and free of dependencies on purpose: the shell extension DLL (which must stay small and safe inside
// explorer.exe), the registration code and the settings page all share this one table.
//
// The settings file stores the enabled items as `shell_items=<id>,<id>,...` (table order). A missing or empty value
// means "the defaults"; "-" means "no items at all".
#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>

namespace lycan {

enum class ShellGroup { Files, Archives, Other };

struct ShellItemInfo {
    const char* id;
    ShellGroup group;
    bool defaultOn;
};

inline constexpr ShellItemInfo kShellItems[] = {
    // Files and folders (not archives)
    {"add", ShellGroup::Files, true},          // Add to archive...
    {"fmt1", ShellGroup::Files, true},         // Compress to "name.<format 1>" (zip by default)
    {"fmt2", ShellGroup::Files, true},         // Compress to "name.<format 2>" (7z by default)
    {"each", ShellGroup::Files, true},         // Compress each item separately (only shown for several items)
    {"template", ShellGroup::Files, true},     // Compress with a preset >
    {"more", ShellGroup::Files, false},        // Other formats > (tar.gz / tar.xz / tar.bz2)
    {"encrypt", ShellGroup::Files, false},     // Encrypted archive...
    // Archives
    {"open", ShellGroup::Archives, true},
    {"here", ShellGroup::Archives, true},      // Extract here
    {"folder", ShellGroup::Archives, true},    // Extract to "name\"
    {"to", ShellGroup::Archives, true},        // Extract to...
    {"test", ShellGroup::Archives, true},
    {"images", ShellGroup::Archives, true},    // Image preview (all images of the archive)
    {"audio", ShellGroup::Archives, false},    // Audio preview
    {"recent", ShellGroup::Archives, false},   // Extract to a recently used folder >
    {"merge", ShellGroup::Archives, false},    // Extract several archives into one folder
    // Other
    {"opencontent", ShellGroup::Other, true},  // Open with LycanZip: .exe / .iso / .msi / .docx ... as an archive
    {"hash", ShellGroup::Other, true},         // Hash values (CRC-32, MD5, SHA-1, SHA-256 ...) of the selected files and folders; in both menus
    {"hashsave", ShellGroup::Other, true},     // Calculate the SHA-256 and save it as name.sha256 next to the item (nothing is shown unless it takes long); in both menus
};

inline constexpr size_t kShellItemCount = sizeof(kShellItems) / sizeof(kShellItems[0]);

inline int ShellItemIndex(std::string_view id) {
    for (size_t i = 0; i < kShellItemCount; ++i) if (id == kShellItems[i].id) return static_cast<int>(i);
    return -1;
}

// Is the item enabled in `list` (the value of shell_items)? Unknown ids are never enabled.
inline bool ShellItemEnabled(std::string_view list, std::string_view id) {
    const int idx = ShellItemIndex(id);
    if (idx < 0) return false;
    if (list.empty()) return kShellItems[idx].defaultOn;
    if (list == "-") return false;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string_view::npos) end = list.size();
        if (list.substr(pos, end - pos) == id) return true;
        pos = end + 1;
    }
    return false;
}

// The canonical value of shell_items for the given set of enabled items (table order). Returns "" when it equals the
// defaults, so that a user who never touched the list follows later changes of the defaults.
inline std::string ShellItemsValue(const bool (&enabled)[kShellItemCount]) {
    std::string out;
    bool isDefault = true;
    for (size_t i = 0; i < kShellItemCount; ++i) {
        if (enabled[i] != kShellItems[i].defaultOn) isDefault = false;
        if (!enabled[i]) continue;
        if (!out.empty()) out += ',';
        out += kShellItems[i].id;
    }
    if (isDefault) return {};
    return out.empty() ? std::string("-") : out;
}

// Files that "Open contents with LycanZip" is offered for (they are archives in disguise); every other extension is
// left alone. Lower case, with the dot.
inline constexpr const wchar_t* kContentExts[] = {L".exe", L".msi", L".dll", L".apk", L".jar", L".docx",
                                                  L".xlsx", L".pptx", L".epub", L".appx", L".msix"};

// Names of the built-in compression presets (src/core/presets.cpp; test_presets keeps the two in step). The context menu
// lists them without loading Core.
inline constexpr const wchar_t* kBuiltinPresetNames[] = {L"General ZIP", L"General 7z", L"Smallest size", L"Fast",
                                                         L"Email / share", L"Backup", L"Store only, no compression"};
// The names that the first versions used (Chinese). A setting or a command line that still has one gets the current name.
inline constexpr const wchar_t* kLegacyBuiltinPresetNames[] = {L"一般 ZIP", L"一般 7z", L"最小體積", L"快速",
                                                              L"寄信 / 分享", L"備份", L"僅儲存不壓縮"};
inline std::wstring CanonicalBuiltinPresetName(const std::wstring& name) {
    for (size_t i = 0; i < sizeof(kLegacyBuiltinPresetNames) / sizeof(kLegacyBuiltinPresetNames[0]); ++i)
        if (name == kLegacyBuiltinPresetNames[i]) return kBuiltinPresetNames[i];
    return name;
}
// The ids of their display names (the names themselves are the keys: the settings and the command line refer to them; the user sees the translated text)
inline constexpr const wchar_t* kBuiltinPresetNameIds[] = {L"preset.general_zip", L"preset.general_7z", L"main.smallest_size", L"preset.fast",
                                                           L"preset.e_mail_share", L"preset.backup", L"preset.store_only_no_compression"};

// Which of the presets are in the "Compress with a preset" sub menu. The built-in ones are switched in the settings
// (`shell_presets=<id>,<id>,...`, empty = the defaults below, "-" = none); a preset of the user has `in_menu=` in
// presets.ini. Every preset is always available on the compress page; this only decides what the menu lists.
// "zip" and "7z" are not in the menu by default: "Compress to ZIP / 7z" are items of their own.
inline constexpr size_t kBuiltinPresetCount = sizeof(kBuiltinPresetNames) / sizeof(kBuiltinPresetNames[0]);
inline constexpr const char* kBuiltinPresetIds[kBuiltinPresetCount] = {"zip", "7z", "smallest", "fast", "share", "backup", "store"};
inline constexpr bool kBuiltinPresetDefaultOn[kBuiltinPresetCount] = {false, false, true, false, false, true, true};

// The id to translate the name of a preset with; a name that is not a built-in one (a preset of the user) is its own text.
inline const wchar_t* BuiltinPresetNameId(const wchar_t* name) {
    for (size_t i = 0; i < kBuiltinPresetCount; ++i)
        if (std::wstring_view(name) == kBuiltinPresetNames[i]) return kBuiltinPresetNameIds[i];
    return name;
}
inline const wchar_t* BuiltinPresetNameId(const std::wstring& name) {
    for (size_t i = 0; i < kBuiltinPresetCount; ++i)
        if (name == kBuiltinPresetNames[i]) return kBuiltinPresetNameIds[i];
    return name.c_str();
}

inline bool BuiltinPresetEnabled(std::string_view list, size_t index) {
    if (index >= kBuiltinPresetCount) return false;
    if (list.empty()) return kBuiltinPresetDefaultOn[index];
    if (list == "-") return false;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t end = list.find(',', pos);
        if (end == std::string_view::npos) end = list.size();
        if (list.substr(pos, end - pos) == kBuiltinPresetIds[index]) return true;
        pos = end + 1;
    }
    return false;
}

// The canonical value of shell_presets for a set of enabled built-in presets ("" when it equals the defaults)
inline std::string BuiltinPresetsValue(const bool (&enabled)[kBuiltinPresetCount]) {
    std::string out;
    bool isDefault = true;
    for (size_t i = 0; i < kBuiltinPresetCount; ++i) {
        if (enabled[i] != kBuiltinPresetDefaultOn[i]) isDefault = false;
        if (!enabled[i]) continue;
        if (!out.empty()) out += ',';
        out += kBuiltinPresetIds[i];
    }
    if (isDefault) return {};
    return out.empty() ? std::string("-") : out;
}

inline std::string NormalizeBuiltinPresets(std::string_view list) {
    if (list.empty()) return {};
    bool enabled[kBuiltinPresetCount];
    for (size_t i = 0; i < kBuiltinPresetCount; ++i) enabled[i] = BuiltinPresetEnabled(list, i);
    return BuiltinPresetsValue(enabled);
}

inline std::string BuiltinPresetsWith(std::string_view list, size_t index, bool on) {
    bool enabled[kBuiltinPresetCount];
    for (size_t i = 0; i < kBuiltinPresetCount; ++i) enabled[i] = BuiltinPresetEnabled(list, i);
    if (index < kBuiltinPresetCount) enabled[index] = on;
    return BuiltinPresetsValue(enabled);
}

// The name an archive gives its folder: "foo.tar.gz" -> "foo", "foo.7z.001" -> "foo", "foo.part1.rar" -> "foo".
// `filename` is the file name only (no directory). Shared by Core (extraction) and the context menu (menu titles).
inline std::wstring ArchiveBaseNameOf(const std::wstring& filename) {
    auto lower = [](std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c)); return s; };
    // the same split as std::filesystem::path: the extension starts at the last dot, but a leading dot is not one
    auto dot = [](const std::wstring& s) { const size_t d = s.rfind(L'.'); return d == 0 ? std::wstring::npos : d; };
    auto extOf = [&](const std::wstring& s) { const size_t d = dot(s); return d == std::wstring::npos ? std::wstring() : s.substr(d); };
    auto stemOf = [&](const std::wstring& s) { const size_t d = dot(s); return d == std::wstring::npos ? s : s.substr(0, d); };
    std::wstring p = filename;
    const std::wstring e0 = extOf(p);
    if (e0.size() == 4 && std::iswdigit(e0[1]) && std::iswdigit(e0[2]) && std::iswdigit(e0[3])) p = stemOf(p);  // .001 .002 ...
    const std::wstring ext = lower(extOf(p));
    const bool combined = ext == L".tgz" || ext == L".tbz" || ext == L".tbz2" || ext == L".txz" || ext == L".tlz" ||
                          ext == L".taz" || ext == L".tzst";
    p = stemOf(p);
    if (!combined && lower(extOf(p)) == L".tar") p = stemOf(p);
    // foo.part1 / foo.part01 (rar volumes)
    const std::wstring lp = lower(p);
    const size_t pos = lp.rfind(L".part");
    if (pos != std::wstring::npos && pos + 5 < lp.size()) {
        bool digits = true;
        for (size_t i = pos + 5; i < lp.size(); ++i) if (!std::iswdigit(lp[i])) digits = false;
        if (digits) p.erase(pos);
    }
    return p.empty() ? filename : p;
}

// Clean up a value read from a file: unknown ids dropped, table order, canonical form.
inline std::string NormalizeShellItems(std::string_view list) {
    if (list.empty()) return {};
    bool enabled[kShellItemCount];
    for (size_t i = 0; i < kShellItemCount; ++i) enabled[i] = ShellItemEnabled(list, kShellItems[i].id);
    return ShellItemsValue(enabled);
}

// `list` with one item switched on or off.
inline std::string ShellItemsWith(std::string_view list, std::string_view id, bool on) {
    const int idx = ShellItemIndex(id);
    bool enabled[kShellItemCount];
    for (size_t i = 0; i < kShellItemCount; ++i) enabled[i] = ShellItemEnabled(list, kShellItems[i].id);
    if (idx >= 0) enabled[idx] = on;
    return ShellItemsValue(enabled);
}

}  // namespace lycan
