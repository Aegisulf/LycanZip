#pragma once
// The diagnostic log: a text file that says what the program did and what went wrong, for finding out why something did not
// work (and for attaching to a bug report). It never leaves the computer.
//
//   * Errors and warnings are always written (a few lines when something is wrong); the detailed messages (info and debug:
//     what was opened, which task started and ended and how long it took) only when the user has switched "detailed log" on
//     in the settings (debug_log=true) or the environment variable LYCAN_LOG is "debug" / "1".
//   * Paths are written as file names only, unless "full paths" is switched on (log_full_paths=true). File contents, passwords
//     and settings values are never written. A path with a drive letter inside a message is cut to its last part as well (network
//     paths and other ways of writing a path are not recognised: look at the log before you share it).
//   * Where: logs\ next to LycanZip.exe in portable mode (the same rule as the crash reports), otherwise
//     UserDataDir()\logs\ (datalocation.h; LYCAN_LOG_DIR overrides it). lycanzip.log, and when it is 2 MB the older ones are renamed
//     to lycanzip.1.log .. lycanzip.3.log (the oldest is deleted): never more than about 8 MB.
//   * Every process of LycanZip (the window, the worker processes of the tasks, the command line) writes to the same file: a line
//     is one write, so lines are never mixed; each line carries the process id and what kind of process it is.
// Writing never throws and never blocks for long: a log that cannot be written is silently skipped.
#include <filesystem>
#include <format>
#include <string>

namespace lycan::diag {

enum class Level { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Once per process: `process` is the tag of the lines ("app", "worker", "cli"). Reads the settings (debug_log, log_full_paths)
// and the environment; a second call does nothing.
void Init(const wchar_t* process);
// The window changes the switches while it runs.
void Configure(bool detailed, bool fullPaths);
bool Detailed();

std::filesystem::path Dir();           // the folder of the log files
std::filesystem::path CurrentFile();   // Dir() / lycanzip.log

// A path as the log shows it: the file name, or the whole path when "full paths" is on.
std::wstring Name(const std::filesystem::path& p);

void Write(Level level, const std::wstring& message);
std::wstring FromUtf8(const std::string& utf8);   // for the messages of lycan::Error (what() is UTF-8)

template <class... A> void Err(std::wformat_string<A...> f, A&&... a) { Write(Level::Error, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Warn(std::wformat_string<A...> f, A&&... a) { Write(Level::Warn, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Info(std::wformat_string<A...> f, A&&... a) { if (Detailed()) Write(Level::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A> void Debug(std::wformat_string<A...> f, A&&... a) { if (Detailed()) Write(Level::Debug, std::format(f, std::forward<A>(a)...)); }

// The first lines of a run (info): the version, Windows, the engine, portable or not.
void LogStartup();

// What a bug report needs: the version, Windows, the engine, portable or not, where the settings and the log are, the switches,
// and the last `maxLogLines` lines of the log. UTF-8 free text (this is not translated: it is for the person who reads the report).
std::wstring DiagnosticText(size_t maxLogLines = 40);
// The last lines of the log (the current file; the one before when that is short)
std::wstring RecentLog(size_t maxLines);

}  // namespace lycan::diag
