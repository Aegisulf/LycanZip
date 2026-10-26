#include "lycan/crashreport.h"
#include "lycan/datalocation.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <functional>
#include <sstream>

namespace fs = std::filesystem;

namespace lycan {
namespace {

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// Tab / line breaks become spaces; a snapshot like this is one line per item, separated by Tabs
std::wstring Flat(std::wstring s) {
    for (auto& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    const size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    size_t pos = 0;
    for (;;) {
        size_t i = s.find(sep, pos);
        if (i == std::wstring::npos) { out.push_back(s.substr(pos)); break; }
        out.push_back(s.substr(pos, i - pos));
        pos = i + 1;
    }
    return out;
}

}  // namespace

fs::path DefaultCrashDir() {
    wchar_t exe[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    fs::path dir = fs::path(std::wstring(exe, n)).parent_path();
    std::error_code ec;
    if (fs::exists(dir / L"LycanZip.portable", ec)) return dir / L"crashes";
    return UserDataDir() / L"crashes";
}

std::string BuildCrashSnapshot(const std::vector<fs::path>& archives, const std::vector<CrashTask>& tasks) {
    std::wstring w;
    for (auto const& a : archives) w += L"window\t" + Flat(a.wstring()) + L"\n";
    for (auto const& t : tasks)
        w += L"task\t" + Flat(t.kind) + L"\t" + Flat(t.state) + L"\t" + Flat(t.title) + L"\t" + Flat(t.target.wstring()) + L"\n";
    return ToUtf8(w);
}

bool WriteCrashReport(const fs::path& file, const CrashHeader& h, const char* snapshot, size_t snapshotLen) {
    // Called from the crash handler: use only the stack, fixed buffers and Win32 API
    wchar_t head[1024];
    const int n = swprintf_s(head, L"LycanZip crash report\ntime=%s\nversion=%s\nexception=0x%08lX\naddress=%s\ndump=%s\n[state]\n",
                             h.time, h.version, h.code, h.address, h.dumpName);
    if (n < 0) return false;
    char utf8[3072];
    const int m = WideCharToMultiByte(CP_UTF8, 0, head, n, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (m <= 0) return false;
    HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, utf8, static_cast<DWORD>(m), &written, nullptr) != 0;
    if (ok && snapshot && snapshotLen) ok = WriteFile(f, snapshot, static_cast<DWORD>(snapshotLen), &written, nullptr) != 0;
    CloseHandle(f);
    return ok;
}

bool ParseCrashReport(const fs::path& file, CrashReport& out) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    std::string raw((std::istreambuf_iterator<char>(in)), {});
    std::wstring all = FromUtf8(raw);
    if (!all.empty() && all[0] == 0xFEFF) all.erase(0, 1);  // a report written or edited by hand / PowerShell may have a UTF-8 BOM
    if (all.rfind(L"LycanZip crash report", 0) != 0) return false;
    out = CrashReport{};
    out.file = file;
    out.seen = EndsWith(file.wstring(), L".seen.txt");
    bool inState = false;
    std::wistringstream ss(all);
    std::wstring line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line == L"[state]") { inState = true; continue; }
        if (!inState) {
            auto eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
            if (k == L"time") out.time = v;
            else if (k == L"version") out.version = v;
            else if (k == L"exception") out.exception = v;
            else if (k == L"address") out.address = v;
            else if (k == L"dump" && !v.empty()) out.dump = file.parent_path() / v;
            continue;
        }
        auto f = Split(line, L'\t');
        if (f[0] == L"window" && f.size() >= 2 && !f[1].empty()) out.archives.emplace_back(f[1]);
        else if (f[0] == L"task" && f.size() >= 5) out.tasks.push_back({f[1], f[2], f[3], fs::path(f[4])});
    }
    return true;
}

std::vector<CrashReport> PendingCrashReports(const fs::path& dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return {};
    for (auto const& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        const std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"crash-", 0) == 0 && EndsWith(name, L".txt") && !EndsWith(name, L".seen.txt")) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end(), [](auto const& a, auto const& b) { return a.filename().wstring() > b.filename().wstring(); });  // newest first
    std::vector<CrashReport> out;
    for (auto const& f : files) {
        CrashReport r;
        if (ParseCrashReport(f, r)) out.push_back(std::move(r));
    }
    return out;
}

bool MarkCrashReportSeen(CrashReport& report) {
    if (report.seen) return true;
    std::wstring name = report.file.filename().wstring();
    if (!EndsWith(name, L".txt")) return false;
    name.resize(name.size() - 4);
    fs::path to = report.file.parent_path() / (name + L".seen.txt");
    std::error_code ec;
    fs::rename(report.file, to, ec);
    if (ec) return false;
    report.file = to;
    report.seen = true;
    return true;
}

int CleanupCrashLeftovers(const CrashReport& report) {
    int n = 0;
    for (auto const& t : report.tasks) {
        if (t.kind != L"Compress" || t.target.empty()) continue;
        fs::path tmp = t.target;
        tmp += L".lycantmp";
        std::error_code ec;
        if (fs::is_regular_file(tmp, ec) && fs::remove(tmp, ec)) ++n;
    }
    return n;
}

void PruneCrashDir(const fs::path& dir, size_t keep) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    std::vector<std::wstring> stems;
    for (auto const& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        std::wstring name = e.path().filename().wstring();
        if (name.rfind(L"crash-", 0) != 0) continue;
        if (EndsWith(name, L".seen.txt")) name.resize(name.size() - 9);
        else if (EndsWith(name, L".txt") || EndsWith(name, L".dmp")) name.resize(name.size() - 4);
        else continue;
        // only the reports we named ourselves: crash-YYYYMMDD-... (a file like crash-bad.txt is left alone, because it is not our name)
        bool stamp = name.size() > 15 && name[14] == L'-';
        for (size_t i = 6; stamp && i < 14; ++i) stamp = name[i] >= L'0' && name[i] <= L'9';
        if (!stamp) continue;
        if (std::find(stems.begin(), stems.end(), name) == stems.end()) stems.push_back(name);
    }
    std::sort(stems.begin(), stems.end(), std::greater<>());  // the name contains the time: newest first
    for (size_t i = keep; i < stems.size(); ++i)
        for (const wchar_t* ext : {L".txt", L".seen.txt", L".dmp"}) fs::remove(dir / (stems[i] + ext), ec);
}

}  // namespace lycan
