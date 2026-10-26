#include "lycan/versioncheck.h"

#include <windows.h>
#include <appmodel.h>
#include <winhttp.h>

#include <algorithm>
#include <cwctype>
#include <vector>

#include "lycan/datalocation.h"
#include "lycan/shellreg.h"
#include "lycan/version.h"

namespace lycan {

std::optional<Version> ParseVersion(const std::string& text) {
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    int parts[3] = {0, 0, 0};
    int n = 0;
    while (true) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9' || n >= 3) return std::nullopt;
        long long v = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            v = v * 10 + (text[i] - '0');
            if (v > 100000) return std::nullopt;
            ++i;
        }
        parts[n++] = static_cast<int>(v);
        if (i == text.size()) break;
        if (text[i] != '.') return std::nullopt;
        ++i;
    }
    return Version{parts[0], parts[1], parts[2]};
}

bool IsNewerVersion(const Version& c, const Version& cur) {
    if (c.major != cur.major) return c.major > cur.major;
    if (c.minor != cur.minor) return c.minor > cur.minor;
    return c.patch > cur.patch;
}

namespace {

// The string value of "key" in a flat JSON object. Handles \" \\ \/ and \uXXXX only as far as plain ASCII; this file is ours.
std::optional<std::string> JsonString(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = 0;
    while ((p = j.find(pat, p)) != std::string::npos) {
        size_t q = p + pat.size();
        while (q < j.size() && (j[q] == ' ' || j[q] == '\t' || j[q] == '\r' || j[q] == '\n')) ++q;
        if (q >= j.size() || j[q] != ':') { p = q; continue; }
        ++q;
        while (q < j.size() && (j[q] == ' ' || j[q] == '\t' || j[q] == '\r' || j[q] == '\n')) ++q;
        if (q >= j.size() || j[q] != '"') return std::nullopt;
        ++q;
        std::string out;
        while (q < j.size() && j[q] != '"') {
            if (j[q] == '\\' && q + 1 < j.size()) {
                ++q;
                if (j[q] == 'n' || j[q] == 't' || j[q] == 'u') return std::nullopt;
            }
            out.push_back(j[q++]);
            if (out.size() > 512) return std::nullopt;
        }
        if (q >= j.size()) return std::nullopt;
        return out;
    }
    return std::nullopt;
}

bool HttpsLink(const std::string& s) { return s.rfind("https://", 0) == 0 && s.find_first_of(" \r\n\t\"<>") == std::string::npos; }

std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

}  // namespace

std::optional<LatestInfo> ParseLatestJson(const std::string& json) {
    LatestInfo info;
    const auto ver = JsonString(json, "version");
    if (!ver || !ParseVersion(*ver)) return std::nullopt;
    info.version = *ver;
    if (const auto r = JsonString(json, "released")) info.released = r->substr(0, 10);
    if (const auto pg = JsonString(json, "page")) {
        if (!HttpsLink(*pg)) return std::nullopt;
        info.page = *pg;
    }
    if (const auto cl = JsonString(json, "changelog")) {
        if (!HttpsLink(*cl)) return std::nullopt;
        info.changelog = *cl;
    }
    return info;
}

InstallChannel DetectChannel(bool packaged, bool portable, const std::wstring& exePath) {
    if (packaged) return InstallChannel::Store;
    if (IsInstalledLayout(std::filesystem::path(exePath).parent_path())) return InstallChannel::Installed;
    const std::wstring p = Lower(exePath);
    if (p.find(L"\\microsoft\\winget\\") != std::wstring::npos) return InstallChannel::Winget;
    if (p.find(L"\\scoop\\apps\\") != std::wstring::npos) return InstallChannel::Scoop;
    if (portable) return InstallChannel::Portable;
    return InstallChannel::Standard;
}

InstallChannel DetectChannel() {
    const DataLocation d = GetDataLocation();
    return DetectChannel(d.packaged, d.portable, (d.exeDir / L"LycanZip.exe").wstring());
}

bool UpdateCheckDue(long long last, long long now) {
    if (last <= 0 || last > now) return true;
    return now - last >= 7LL * 24 * 3600;
}

namespace {

struct Handle {
    HINTERNET h = nullptr;
    explicit Handle(HINTERNET x) : h(x) {}
    ~Handle() { if (h) WinHttpCloseHandle(h); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

}  // namespace

std::optional<LatestInfo> FetchLatest(const std::wstring& urlOverride) {
    const std::wstring url = urlOverride.empty() ? std::wstring(kLatestUrl) : urlOverride;
    URL_COMPONENTSW uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {}, path[1024] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return std::nullopt;
    const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!https) {
        // plain http only for a test server on this computer
        const std::wstring h = Lower(host);
        if (uc.nScheme != INTERNET_SCHEME_HTTP || (h != L"localhost" && h != L"127.0.0.1")) return std::nullopt;
    }
    const std::wstring ua = L"LycanZip/" LYCAN_VERSION_WSTRING;
    Handle session(WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h) return std::nullopt;
    WinHttpSetTimeouts(session.h, 5000, 5000, 5000, 8000);
    Handle conn(WinHttpConnect(session.h, host, uc.nPort, 0));
    if (!conn.h) return std::nullopt;
    Handle req(WinHttpOpenRequest(conn.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0));
    if (!req.h) return std::nullopt;
    // no cookies, no credentials, no redirect from https to http (the default policy)
    DWORD opt = WINHTTP_DISABLE_COOKIES;
    WinHttpSetOption(req.h, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
    if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) return std::nullopt;
    if (!WinHttpReceiveResponse(req.h, nullptr)) return std::nullopt;
    DWORD status = 0, sz = sizeof(status);
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX) || status != 200) return std::nullopt;
    std::string body;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req.h, &avail)) return std::nullopt;
        if (avail == 0) break;
        if (body.size() + avail > 16 * 1024) return std::nullopt;
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(req.h, buf.data(), avail, &got)) return std::nullopt;
        body.append(buf.data(), got);
    }
    return ParseLatestJson(body);
}

}  // namespace lycan
