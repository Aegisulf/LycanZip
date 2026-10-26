#pragma once
// The optional check for a new version of LycanZip. The program asks one small file (latest.json on the web site) at most once in
// seven days, only when the user turned the check on, and sends nothing but the program name and version in the User-Agent.
// A failure (no network, a bad answer) is silent. This module has the pure parts (versions, the file, the way the program was
// installed) and the one HTTP request.
#include <optional>
#include <string>

namespace lycan {

struct Version {
    int major = 0, minor = 0, patch = 0;
    bool operator==(const Version& o) const { return major == o.major && minor == o.minor && patch == o.patch; }
};

// "1.2.3", "v1.2" (missing parts are 0). Anything else (a pre-release suffix, letters, empty) is not a version.
std::optional<Version> ParseVersion(const std::string& text);
bool IsNewerVersion(const Version& candidate, const Version& current);

struct LatestInfo {
    std::string version;     // "1.0.0"
    std::string released;    // "2026-11-01" (shown only)
    std::string page;        // the download page
    std::string changelog;   // the changelog page
};

// Reads latest.json: a flat object of strings. Returns nothing when "version" is missing or not a version, or when a link is not https.
std::optional<LatestInfo> ParseLatestJson(const std::string& json);

// How this copy of the program got here; it decides who updates it.
enum class InstallChannel { Store, Winget, Scoop, Portable, Standard, Installed };

// From the facts. Store = the app has a package identity; Winget/Scoop = the exe is in the folder those tools use; Installed = made by our installer
// (<root>\current\LycanZip.exe with <root>\Update.exe, see shellreg.h IsInstalledLayout; it updates itself, updatefeed.h).
InstallChannel DetectChannel(bool packaged, bool portable, const std::wstring& exePath);
InstallChannel DetectChannel();   // the running program

// The seven-day rule (seconds since 1970). A last check in the future (clock changed) counts as due.
bool UpdateCheckDue(long long lastCheck, long long now);

// One HTTPS GET of the file (the address of the site, or LYCAN_UPDATE_URL for tests; http is accepted only for localhost).
// Short timeouts, no redirects to other hosts, answer at most 16 KB. Returns nothing on any failure.
std::optional<LatestInfo> FetchLatest(const std::wstring& urlOverride = L"");

constexpr const wchar_t* kLatestUrl = L"https://aegisulf.com/lycanzip/latest.json";

}  // namespace lycan
