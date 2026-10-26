// The diagnostic log (diag.h): what is written at which level, paths as file names, one line per message, several threads and
// several processes writing to the same file, rotation (never more than four files of 2 MB), the end of the log for the bug report.
#include "no_dialogs.h"
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "lycan/diag.h"

namespace fs = std::filesystem;
using namespace lycan;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> v;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) v.push_back(l);
    }
    return v;
}

static bool Has(const std::string& text, const char* needle) { return text.find(needle) != std::string::npos; }

int main(int argc, char** argv) {
    // a child process of the multi-process test: writes its lines and ends
    if (argc >= 3 && std::string(argv[1]) == "--child") {
        diag::Init(L"child");
        diag::Configure(false, false);
        const std::wstring tag = std::wstring(argv[2], argv[2] + strlen(argv[2]));
        for (int i = 0; i < 500; ++i) diag::Warn(L"child {} line {}", tag, i);
        return 0;
    }

    const fs::path root = fs::temp_directory_path() / (L"lycan_test_diag_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    SetEnvironmentVariableW(L"LYCAN_LOG_DIR", (root / L"logs").c_str());
    SetEnvironmentVariableW(L"LYCAN_LOG", nullptr);
    const fs::path log = root / L"logs" / L"lycanzip.log";

    diag::Init(L"test");
    diag::Configure(false, false);  // whatever the settings of this machine say
    EXPECT(diag::Dir() == root / L"logs" && diag::CurrentFile() == log);

    // ---- levels: errors and warnings always, info and debug only when the detailed log is on
    diag::Info(L"hidden info");
    diag::Debug(L"hidden debug");
    diag::Warn(L"visible warn {}", 1);
    diag::Err(L"visible error {}", L"x");
    std::string text = ReadAll(log);
    EXPECT(Has(text, "visible warn 1") && Has(text, "visible error x") && !Has(text, "hidden"));
    EXPECT(Has(text, "WARN ") && Has(text, "ERROR") && Has(text, "[" ) && Has(text, " test]"));
    EXPECT(text.size() > 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0);   // UTF-8 with a BOM, once
    EXPECT(text.find("\xEF\xBB\xBF", 3) == std::string::npos);
    diag::Configure(true, false);
    EXPECT(diag::Detailed());
    diag::Info(L"shown info");
    diag::Debug(L"shown debug");
    text = ReadAll(log);
    EXPECT(Has(text, "INFO  shown info") && Has(text, "DEBUG shown debug"));
    diag::Configure(false, false);

    // ---- a path is a file name unless full paths are on; one message is one line; UTF-8
    EXPECT(diag::Name(L"C:\\Users\\me\\secret folder\\a b.zip") == L"a b.zip");
    EXPECT(diag::Name(L"C:\\") == L"C:\\");
    diag::Configure(false, true);
    EXPECT(diag::Name(L"C:\\Users\\me\\a.zip") == L"C:\\Users\\me\\a.zip");
    diag::Configure(false, false);
    diag::Warn(L"two\nlines\tand a tab\r\n,名稱");
    text = ReadAll(log);
    EXPECT(Has(text, "two lines and a tab  ,\xE5\x90\x8D\xE7\xA8\xB1"));   // the new lines are spaces, the characters are UTF-8
    const auto before = Lines(text);
    EXPECT(!before.empty() && before.back().find("two lines") != std::string::npos);   // the whole message is the last line

    // ---- the paths in a message are cut to their last part (unless full paths are on)
    {
        diag::Warn(L"cannot open C:\\Users\\me\\secret dir\\a b.zip because it is locked");
        diag::Warn(L"copy D:\\one\\x.txt to E:\\two\\three\\y.txt failed");
        diag::Warn(L"folder \u300cC:\\Users\\me\\Temp\\\u300d has no room, \"F:\\q\\r.log\" too");
        const std::string t2 = ReadAll(log);
        EXPECT(Has(t2, "cannot open a b.zip because it is locked") && !Has(t2, "secret dir"));
        EXPECT(Has(t2, "copy x.txt to y.txt failed") && !Has(t2, "three"));
        EXPECT(Has(t2, "has no room, \"r.log\" too") && !Has(t2, "Users"));
        diag::Configure(false, true);
        diag::Warn(L"kept C:\\Users\\me\\whole.zip");
        EXPECT(Has(ReadAll(log), "kept C:\\Users\\me\\whole.zip"));
        diag::Configure(false, false);
        diag::Warn(L"not a path: a:b c:\\ d:/e 5:\\x");
        EXPECT(Has(ReadAll(log), "not a path: a:b"));
    }

    // ---- several threads: every line is there, whole
    {
        std::vector<std::thread> ts;
        for (int t = 0; t < 8; ++t)
            ts.emplace_back([t] { for (int i = 0; i < 300; ++i) diag::Warn(L"thread {} line {} {}", t, i, std::wstring(40, L'x')); });
        for (auto& th : ts) th.join();
        std::map<std::string, int> seen;
        for (auto const& l : Lines(ReadAll(log))) {
            const auto at = l.find("thread ");
            if (at == std::string::npos) continue;
            EXPECT(l.size() > 40 && l.substr(l.size() - 40) == std::string(40, 'x'));   // not cut, not mixed
            ++seen[l.substr(at)];
        }
        EXPECT(seen.size() == 8 * 300);
        for (auto const& [k, n] : seen) if (n != 1) { EXPECT(false); break; }
    }

    // ---- several processes: the same file, nothing lost, nothing mixed
    {
        wchar_t exe[MAX_PATH * 2];
        GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        std::vector<PROCESS_INFORMATION> kids;
        for (int k = 0; k < 4; ++k) {
            std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --child c" + std::to_wstring(k);
            STARTUPINFOW si{sizeof(si)};
            PROCESS_INFORMATION pi{};
            EXPECT(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
            kids.push_back(pi);
        }
        for (auto& pi : kids) {
            WaitForSingleObject(pi.hProcess, 30000);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        std::map<std::string, int> seen;
        for (auto const& l : Lines(ReadAll(log))) {
            const auto at = l.find("child c");
            if (at == std::string::npos) continue;
            EXPECT(l.find(" child]") != std::string::npos);   // the tag of the process that wrote it
            ++seen[l.substr(at)];
        }
        EXPECT(seen.size() == 4 * 500);
        for (auto const& [k, n] : seen) if (n != 1) { EXPECT(false); break; }
    }

    // ---- the end of the log (for the bug report)
    diag::Warn(L"the very last message");
    {
        const std::wstring recent = diag::RecentLog(3);
        std::string r(recent.begin(), recent.end());
        EXPECT(Lines(r).size() == 3 && Has(r, "the very last message"));
        const std::wstring d = diag::DiagnosticText(5);
        EXPECT(d.find(L"LycanZip ") == 0 && d.find(L"Log folder: ") != std::wstring::npos && d.find(L"the very last message") != std::wstring::npos &&
               d.find(L"Windows ") != std::wstring::npos && d.find(L"7z.dll:") != std::wstring::npos);
    }

    // ---- rotation: four files of 2 MB at most, the oldest goes
    {
        const std::wstring big(900, L'z');
        for (int i = 0; i < 12000; ++i) diag::Warn(L"fill {} {}", i, big);   // about 11 MB
        const fs::path dir = root / L"logs";
        EXPECT(fs::exists(dir / L"lycanzip.log") && fs::exists(dir / L"lycanzip.1.log") && fs::exists(dir / L"lycanzip.2.log") && fs::exists(dir / L"lycanzip.3.log"));
        EXPECT(!fs::exists(dir / L"lycanzip.4.log"));
        uint64_t total = 0;
        for (auto const& e : fs::directory_iterator(dir)) {
            total += e.file_size();
            EXPECT(e.file_size() < (2u << 20) + 4096);
        }
        EXPECT(total < 4ull * (2u << 20) + 16384);
        // the newest lines are in the current file
        const std::string cur = ReadAll(dir / L"lycanzip.log");
        EXPECT(Has(cur, "fill 11999 "));
        EXPECT(!Has(ReadAll(dir / L"lycanzip.3.log"), "fill 11999 "));
        // and RecentLog reads across the files
        const std::wstring tail = diag::RecentLog(10);
        EXPECT(Lines(std::string(tail.begin(), tail.end())).size() == 10);
    }

    fs::remove_all(root, ec);
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("diag tests passed");
    return 0;
}
