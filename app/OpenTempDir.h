#pragma once
// LycanZip's temporary files in the app (the settings page can move them: Settings::tempDir):
//  - "open with the associated program": <base>\LycanZip\open\<pid>-<tick>-<seq>\  (see below for the clean-up)
//  - dragged files:                       <base>\LycanZip\open\drag-<pid>-<tick>-<seq>\
//  - the scratch file of a tar.gz (Core): <base>\LycanZip\lycanzip_*.tmp  (deleted by the system when closed)
// <base> is the folder chosen in the settings, or the system temp folder.
// The program that was started may still read the files after LycanZip exits (an installer reading its .cab files), so
// nothing is deleted by a timer and a folder is only deleted when nothing can be using it:
//  - on exit a process deletes its own "open" folders, except those of programs it started that are still running (or
//    whose process is unknown: they stay). Whatever cannot be deleted is left alone;
//  - at start-up MainWindow::CleanupStaleTemp deletes what is older than a day;
//  - the settings page has a "clear" button (ClearOpenTemp).
#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace lycan_app {

namespace detail {
struct Launched {
    std::filesystem::path dir;
    HANDLE proc;  // may be null: the shell handed the file to a program that was already running
};
inline std::mutex& Mx() { static std::mutex m; return m; }
inline std::vector<Launched>& LaunchList() { static std::vector<Launched> v; return v; }
inline std::filesystem::path& CustomBase() { static std::filesystem::path p; return p; }   // from the settings
inline std::filesystem::path& SessionBase() { static std::filesystem::path p; return p; }  // chosen in a "no room" question, until exit
}  // namespace detail

// The folder from the settings (empty = the system temp folder); a folder that cannot be created counts as empty
inline void SetCustomTempBase(const std::filesystem::path& p) {
    std::error_code ec;
    if (!p.empty()) std::filesystem::create_directories(p, ec);
    detail::CustomBase() = (!p.empty() && std::filesystem::is_directory(p, ec)) ? p : std::filesystem::path{};
}
// A place picked in the "not enough room" question for the rest of this run (empty = none)
inline void SetSessionTempBase(const std::filesystem::path& p) { detail::SessionBase() = p; }

inline std::filesystem::path TempBase() {
    if (!detail::SessionBase().empty()) return detail::SessionBase();
    if (!detail::CustomBase().empty()) return detail::CustomBase();
    std::error_code ec;
    auto t = std::filesystem::temp_directory_path(ec);
    return ec ? std::filesystem::path{} : t;
}

inline std::filesystem::path OpenTempRoot() {
    const auto b = TempBase();
    return b.empty() ? b : b / L"LycanZip" / L"open";
}

// What to give Core as its scratch folder (OpenOptions / CompressOptions::tempDir): empty = the system temp folder
inline std::filesystem::path ScratchOverride() {
    if (!detail::SessionBase().empty()) return detail::SessionBase() / L"LycanZip";
    if (!detail::CustomBase().empty()) return detail::CustomBase() / L"LycanZip";
    return {};
}

// The folder under the open-root that contains `file` (empty when it is not under it)
inline std::filesystem::path OpenTempOwnerDir(const std::filesystem::path& file) {
    const auto root = OpenTempRoot();
    for (auto p = file.parent_path(); !p.empty() && p != p.parent_path(); p = p.parent_path())
        if (p.parent_path() == root) return p;
    return {};
}

// Remember that a program was started from `dir`; takes ownership of `proc` (may be null)
inline void TrackLaunch(const std::filesystem::path& dir, HANDLE proc) {
    if (dir.empty()) { if (proc) CloseHandle(proc); return; }
    std::lock_guard lk(detail::Mx());
    detail::LaunchList().push_back({dir, proc});
}

namespace detail {
// "<pid>-..." or "drag-<pid>-..." -> pid (0 = not one of ours)
inline unsigned long OwnerPid(const std::wstring& name) {
    const wchar_t* s = name.c_str();
    if (name.rfind(L"drag-", 0) == 0) s += 5;
    wchar_t* stop = nullptr;
    const unsigned long pid = wcstoul(s, &stop, 10);
    return (stop == s || *stop != L'-') ? 0 : pid;
}
inline bool ProcessAlive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}
// a program started from this folder that may still be running keeps it alive
inline bool InUseByLaunched(const std::filesystem::path& dir) {
    std::lock_guard lk(Mx());
    for (auto& l : LaunchList())
        if (l.dir == dir && !(l.proc && WaitForSingleObject(l.proc, 0) == WAIT_OBJECT_0)) return true;
    return false;
}
inline uint64_t TreeSize(const std::filesystem::path& p) {
    uint64_t n = 0;
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(p, std::filesystem::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) n += it->file_size(e2);
    }
    return n;
}
}  // namespace detail

// Delete this process's "open" folders (called on exit), except those of programs it started that may still be running.
// Dragged files ("drag-...") stay: the target may still be copying them. Folders of earlier processes are left to
// MainWindow::CleanupStaleTemp (older than a day at start-up). Whatever cannot be deleted is simply left.
inline void CleanOwnOpenTempDirs() {
    const auto root = OpenTempRoot();
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return;
    const std::wstring prefix = std::to_wstring(GetCurrentProcessId()) + L"-";
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().filename().wstring().rfind(prefix, 0) != 0) continue;  // not ours ("drag-..." folders are not either)
        if (detail::InUseByLaunched(it->path())) continue;
        std::error_code rc;
        std::filesystem::remove_all(it->path(), rc);
    }
}

// How much the temporary folders hold now (bytes)
inline uint64_t OpenTempUsage() {
    const auto root = OpenTempRoot();
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return 0;
    return detail::TreeSize(root);
}

// "Clear" button: deletes every temporary folder that nothing of ours is using (a folder of another running LycanZip, or of
// a program started from this one that is still running, stays). Returns the bytes that were freed.
inline uint64_t ClearOpenTemp() {
    const auto root = OpenTempRoot();
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return 0;
    uint64_t freed = 0;
    for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const unsigned long pid = detail::OwnerPid(it->path().filename().wstring());
        if (pid == GetCurrentProcessId() ? detail::InUseByLaunched(it->path()) : (pid != 0 && detail::ProcessAlive(pid))) continue;
        const uint64_t before = detail::TreeSize(it->path());
        std::error_code rc;
        std::filesystem::remove_all(it->path(), rc);
        std::error_code e2;
        if (!std::filesystem::exists(it->path(), e2)) freed += before;
        else freed += before - detail::TreeSize(it->path());  // some files were in use: what could go went
    }
    return freed;
}

}  // namespace lycan_app
