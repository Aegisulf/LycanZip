// Worker-process executor: every task runs in its own lycanzip-cli.exe --worker process.
// Covers: round trips, progress, cancel, pause / resume, callbacks that run in this process (password, conflict),
// errors, a crashing worker, priority (at start and while running), workers dying with the executor, parallel tasks.
#include "no_dialogs.h"
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <thread>

#include "lycan/archive_source.h"
#include "lycan/taskqueue.h"

using namespace lycan;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

static int failures = 0;
#define EXPECT(cond)                                                     \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #cond); \
            ++failures;                                                  \
        }                                                                \
    } while (0)

static void WriteRandom(const fs::path& p, size_t bytes, unsigned seed = 1) {
    fs::create_directories(p.parent_path());
    std::mt19937_64 rng(seed);
    std::ofstream f(p, std::ios::binary);
    std::vector<uint64_t> buf(1 << 17);
    for (size_t left = bytes; left;) {
        for (auto& v : buf) v = rng();
        size_t n = std::min(left, buf.size() * 8);
        f.write(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
        left -= n;
    }
}
static std::string ReadAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}
static void WriteText(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static bool WaitFor(const std::function<bool()>& pred, int ms = 15000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
static bool ProcessGone(DWORD pid, int ms) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return true;
    const bool gone = WaitForSingleObject(h, static_cast<DWORD>(ms)) == WAIT_OBJECT_0;
    CloseHandle(h);
    return gone;
}
static DWORD PriorityClassOf(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    const DWORD c = GetPriorityClass(h);
    CloseHandle(h);
    return c;
}

static bool EfficiencyModeOn(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h) return false;
    PROCESS_POWER_THROTTLING_STATE st{};
    st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    const bool ok = GetProcessInformation(h, ProcessPowerThrottling, &st, sizeof st) != 0;
    CloseHandle(h);
    return ok && (st.StateMask & PROCESS_POWER_THROTTLING_EXECUTION_SPEED) != 0;
}
// I/O priority of a process: 0 very low, 1 low, 2 normal (NtQueryInformationProcess, ProcessIoPriority = 33)
static int IoPriorityOf(DWORD pid) {
    using Fn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    static Fn query = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (!h || !query) { if (h) CloseHandle(h); return -1; }
    ULONG v = 99;
    ULONG len = 0;
    const LONG st = query(h, 33, &v, sizeof v, &len);
    CloseHandle(h);
    return st == 0 ? static_cast<int>(v) : -1;
}

static std::mutex g_pidM;
static std::vector<DWORD> g_pids;
static std::shared_ptr<TaskExecutor> MakeExec() {
    ProcessExecutorOptions o;
    o.onSpawn = [](unsigned long pid) { std::lock_guard lk(g_pidM); g_pids.push_back(pid); };
    return MakeProcessExecutor(o);
}
static DWORD LastPid() { std::lock_guard lk(g_pidM); return g_pids.empty() ? 0 : g_pids.back(); }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    fs::path root = fs::temp_directory_path() / (L"lycan_worker_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root);
    WriteText(root / "src" / "a.txt", std::string(50000, 'a'));
    WriteText(root / "src" / "sub" / "b.txt", "bravo");
    WriteRandom(root / "src" / "r.bin", 3 << 20, 5);
    WriteRandom(root / "big.bin", 160 << 20, 9);  // slow to compress with LZMA: time to cancel / pause / re-prioritise

    // 1) compress -> test -> extract round trip, with progress
    {
        TaskQueue q(1, MakeExec());
        std::vector<TaskState> seen;
        std::mutex m;
        uint64_t lastDone = 0;
        bool backwards = false;
        q.SetListener([&](const TaskInfo& i) {
            std::lock_guard lk(m);
            if (i.kind == TaskKind::Compress) {
                if (i.done < lastDone) backwards = true;
                lastDone = i.done;
            }
            if (seen.empty() || seen.back() != i.state) seen.push_back(i.state);
        });
        CompressOptions co;
        co.type = ArchiveType::SevenZip;
        const auto t0 = std::chrono::steady_clock::now();
        auto c = q.EnqueueCompress(root / "a.7z", {root / "src"}, co);
        EXPECT(q.Wait(c, 60000));
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        TaskInfo ci;
        q.Get(c, ci);
        EXPECT(ci.state == TaskState::Completed && ci.compress.ok() && ci.compress.files == 3 && fs::exists(root / "a.7z"));
        EXPECT(ci.done == ci.total && ci.total > 0);
        EXPECT(!backwards);
        std::printf("   worker round trip (spawn + compress 3 MB): %lld ms\n", static_cast<long long>(ms));
        const auto t1 = std::chrono::steady_clock::now();
        auto t = q.EnqueueTest(root / "a.7z");
        EXPECT(q.Wait(t, 60000));
        std::printf("   a small test task through a worker: %lld ms (process start + load 7z.dll + run)\n",
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count()));
        TaskInfo ti;
        q.Get(t, ti);
        EXPECT(ti.state == TaskState::Completed && ti.extract.files == 3);
        auto e = q.EnqueueExtract(root / "a.7z", root / "out1");
        EXPECT(q.Wait(e, 60000));
        TaskInfo ei;
        q.Get(e, ei);
        EXPECT(ei.state == TaskState::Completed && ei.extract.ok() && ei.extract.files == 3 && ei.extract.outputDir == (root / "out1"));
        EXPECT(ReadAll(root / "out1" / "src" / "sub" / "b.txt") == "bravo");
        EXPECT(ReadAll(root / "out1" / "src" / "r.bin") == ReadAll(root / "src" / "r.bin"));
        std::lock_guard lk(m);
        EXPECT(!seen.empty() && seen.front() == TaskState::Queued && seen.back() == TaskState::Completed);
    }

    // 2) cancel while compressing: ends quickly as Cancelled, the worker is gone, nothing is left behind
    {
        TaskQueue q(1, MakeExec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto c = q.EnqueueCompress(root / "cancel.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(c, i) && i.state == TaskState::Running && i.done > (4u << 20); }, 20000));
        const DWORD pid = LastPid();
        q.Cancel(c);
        EXPECT(q.Wait(c, 20000));
        TaskInfo ci;
        q.Get(c, ci);
        EXPECT(ci.state == TaskState::Cancelled);
        EXPECT(!fs::exists(root / "cancel.7z") && !fs::exists(root / "cancel.7z.lycantmp"));
        EXPECT(ProcessGone(pid, 5000));
    }

    // 3) pause / resume: no progress while paused, completes after resume
    {
        TaskQueue q(1, MakeExec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        co.level = 1;
        auto c = q.EnqueueCompress(root / "pause.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(c, i) && i.state == TaskState::Running && i.done > (2u << 20); }, 20000));
        q.Pause(c);
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(c, i) && i.state == TaskState::Paused; }, 10000));
        TaskInfo a, b;
        q.Get(c, a);
        std::this_thread::sleep_for(700ms);
        q.Get(c, b);
        EXPECT(b.state == TaskState::Paused && b.done == a.done);
        q.Resume(c);
        EXPECT(q.Wait(c, 120000));
        q.Get(c, b);
        EXPECT(b.state == TaskState::Completed && fs::exists(root / "pause.7z"));
    }

    // 4) callbacks run in THIS process: password (open + extract) and conflict
    {
        CompressOptions co;
        co.type = ArchiveType::SevenZip;
        co.password = L"s3cret";
        co.overwrite = true;
        CreateArchive(root / "enc.7z", {root / "src"}, co);
        CompressOptions zc;
        zc.password = L"zipPw";
        zc.overwrite = true;
        CreateArchive(root / "enc.zip", {root / "src"}, zc);
        TaskQueue q(1, MakeExec());
        std::atomic<int> asked{0};
        std::atomic<DWORD> askThread{0};
        auto pw = [&](std::wstring& out) { ++asked; askThread = GetCurrentThreadId(); out = L"zipPw"; return true; };
        ExtractOptions eo;
        eo.password = pw;
        OpenOptions oo;
        oo.password = pw;
        auto e = q.EnqueueExtract(root / "enc.zip", root / "out2", eo, oo);
        EXPECT(q.Wait(e, 60000));
        TaskInfo ei;
        q.Get(e, ei);
        EXPECT(ei.state == TaskState::Completed && ei.extract.ok() && ReadAll(root / "out2" / "src" / "sub" / "b.txt") == "bravo");
        EXPECT(asked > 0 && askThread != 0 && askThread != GetCurrentThreadId());  // asked on the queue's thread, not the worker's
        // wrong password
        ExtractOptions bad;
        bad.password = [](std::wstring& o) { o = L"nope"; return true; };
        auto e2 = q.EnqueueExtract(root / "enc.zip", root / "out3", bad);
        EXPECT(q.Wait(e2, 60000));
        q.Get(e2, ei);
        EXPECT(ei.state == TaskState::Failed && ei.extract.wrongPassword);
        // declined
        ExtractOptions none;
        none.password = [](std::wstring&) { return false; };
        auto e3 = q.EnqueueExtract(root / "enc.zip", root / "out4", none);
        EXPECT(q.Wait(e3, 60000));
        q.Get(e3, ei);
        EXPECT(ei.state != TaskState::Completed);
        // conflict: files exist, the callback sees the details and chooses per file
        WriteText(root / "out5" / "src" / "a.txt", "old a");
        WriteText(root / "out5" / "src" / "sub" / "b.txt", "old b");
        std::set<std::wstring> seen;
        ExtractOptions ask;
        ask.overwrite = OverwriteMode::Ask;
        ask.onConflict = [&](const ConflictInfo& ci) {
            seen.insert(ci.entryPath);
            ConflictAnswer a;
            a.action = ci.entryPath.find(L"a.txt") != std::wstring::npos ? ConflictAction::Overwrite : ConflictAction::Skip;
            EXPECT(ci.existingSize == 5 && ci.newSize > 0 && !ci.existing.empty());
            return a;
        };
        auto e4 = q.EnqueueExtract(root / "a.7z", root / "out5", ask);
        EXPECT(q.Wait(e4, 60000));
        q.Get(e4, ei);
        EXPECT(ei.state == TaskState::Completed || ei.extract.skipped.size() == 1);
        EXPECT(seen.size() == 2);
        EXPECT(ReadAll(root / "out5" / "src" / "a.txt") == std::string(50000, 'a'));
        EXPECT(ReadAll(root / "out5" / "src" / "sub" / "b.txt") == "old b");
        // a suspiciously big archive (6 MB of zeros in a few KB): the question comes from the worker; no = nothing is written, yes = it goes on
        {
            WriteText(root / "zeros" / "z.bin", std::string(6 << 20, '\0'));
            CompressOptions zc;
            zc.type = ArchiveType::Zip;
            zc.storeCompressed = false;
            CreateArchive(root / "zeros.zip", {root / "zeros" / "z.bin"}, zc);
            int largeAsked = 0;
            uint64_t declared = 0;
            ExtractOptions lo;
            lo.largeBytes = 1 << 20;
            lo.largeRatio = 100;
            lo.onLarge = [&](const LargeExtractInfo& li) { ++largeAsked; declared = li.declaredBytes; return false; };
            auto l1 = q.EnqueueExtract(root / "zeros.zip", root / "out7", lo);
            EXPECT(q.Wait(l1, 60000));
            q.Get(l1, ei);
            EXPECT(largeAsked == 1 && declared == (6u << 20));
            EXPECT(ei.state != TaskState::Completed && !fs::exists(root / "out7" / "z.bin"));
            lo.onLarge = [&](const LargeExtractInfo&) { ++largeAsked; return true; };
            auto l2 = q.EnqueueExtract(root / "zeros.zip", root / "out8", lo);
            EXPECT(q.Wait(l2, 60000));
            q.Get(l2, ei);
            EXPECT(largeAsked == 2 && ei.state == TaskState::Completed && fs::exists(root / "out8" / "z.bin") && fs::file_size(root / "out8" / "z.bin") == (6u << 20));
            // the normal archive of this test (a few times its size at most) does not ask
            ExtractOptions normal;
            normal.largeBytes = 1;
            normal.largeRatio = 100;
            normal.onLarge = [&](const LargeExtractInfo&) { ++largeAsked; return false; };
            auto l3 = q.EnqueueExtract(root / "a.7z", root / "out9", normal);
            EXPECT(q.Wait(l3, 60000));
            q.Get(l3, ei);
            EXPECT(largeAsked == 2 && ei.state == TaskState::Completed);
        }
    }

    // 5) errors come back as Failed with the message; nothing hangs
    {
        TaskQueue q(1, MakeExec());
        auto e = q.EnqueueExtract(root / "missing.zip", root / "out6");
        EXPECT(q.Wait(e, 30000));
        TaskInfo ei;
        q.Get(e, ei);
        EXPECT(ei.state == TaskState::Failed && !ei.error.empty());
        auto t = q.EnqueueTest(root / "src" / "a.txt");  // not an archive
        EXPECT(q.Wait(t, 30000));
        q.Get(t, ei);
        EXPECT(ei.state == TaskState::Failed);
    }

    // 6) a worker that crashes only fails its own task; the next task works
    {
        SetEnvironmentVariableW(L"LYCAN_TEST_WORKER_CRASH", L"300");
        TaskQueue q(1, MakeExec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto c = q.EnqueueCompress(root / "crash.7z", {root / "big.bin"}, co);
        EXPECT(q.Wait(c, 60000));
        TaskInfo ci;
        q.Get(c, ci);
        EXPECT(ci.state == TaskState::Failed);
        EXPECT(ci.error.English().find(L"unexpectedly") != std::wstring::npos);
        EXPECT(!fs::exists(root / "crash.7z.lycantmp"));  // the host removes the half-written output of a crashed compression
        SetEnvironmentVariableW(L"LYCAN_TEST_WORKER_CRASH", nullptr);
        auto e = q.EnqueueExtract(root / "a.7z", root / "out7");
        EXPECT(q.Wait(e, 60000));
        q.Get(e, ci);
        EXPECT(ci.state == TaskState::Completed);
    }

    // 7) priority: at start, and changed while the task runs
    {
        TaskQueue q(1, MakeExec());
        q.SetDefaultPriority(TaskPriority::BelowNormal);
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto c = q.EnqueueCompress(root / "prio1.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(c, i) && i.state == TaskState::Running && i.done > (2u << 20); }, 20000));
        const DWORD pid = LastPid();
        EXPECT(PriorityClassOf(pid) == BELOW_NORMAL_PRIORITY_CLASS);
        q.SetPriority(c, TaskPriority::Normal);
        EXPECT(WaitFor([&] { return PriorityClassOf(pid) == NORMAL_PRIORITY_CLASS; }, 5000));
        q.SetPriority(c, TaskPriority::Background);
        // background mode: efficiency mode (EcoQoS) is on and the I/O priority is "very low" (GetPriorityClass does not show it)
        EXPECT(WaitFor([&] { return EfficiencyModeOn(pid); }, 5000));
        EXPECT(WaitFor([&] { return IoPriorityOf(pid) == 0; }, 5000));
        q.SetPriority(c, TaskPriority::Normal);
        // leaving background mode: no efficiency mode and no "very low" I/O any more. Which I/O priority Windows gives back depends on
        // the machine (normal here, low on a CI runner), so only "not very low" is checked
        EXPECT(WaitFor([&] { return !EfficiencyModeOn(pid) && IoPriorityOf(pid) > 0; }, 5000));
        std::printf("   after normal: efficiency mode %d, I/O priority %d\n", EfficiencyModeOn(pid) ? 1 : 0, static_cast<int>(IoPriorityOf(pid)));
        q.CancelAll();
        EXPECT(q.WaitAll(30000));
    }

    // 8) workers are tied to the executor: destroying it ends a running worker
    DWORD orphan = 0;
    {
        auto exec = MakeExec();
        auto q = std::make_unique<TaskQueue>(1, exec);
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto c = q->EnqueueCompress(root / "orphan.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] { TaskInfo i; return q->Get(c, i) && i.state == TaskState::Running && i.done > (2u << 20); }, 20000));
        orphan = LastPid();
        q.reset();     // cancels and waits
        exec.reset();  // closes the job object
    }
    EXPECT(ProcessGone(orphan, 5000));

    // 9) parallel tasks, each in its own process
    {
        TaskQueue q(4, MakeExec());
        size_t firstNew;
        { std::lock_guard lk(g_pidM); firstNew = g_pids.size(); }
        std::vector<uint64_t> ids;
        for (int i = 0; i < 6; ++i) ids.push_back(q.EnqueueExtract(root / "a.7z", root / (L"par" + std::to_wstring(i))));
        EXPECT(q.WaitAll(120000));
        int ok = 0;
        for (auto id : ids) { TaskInfo ti; q.Get(id, ti); if (ti.state == TaskState::Completed && ti.extract.files == 3) ++ok; }
        EXPECT(ok == 6);
        std::set<DWORD> distinct;
        size_t spawned;
        { std::lock_guard lk(g_pidM); distinct.insert(g_pids.begin() + firstNew, g_pids.end()); spawned = g_pids.size() - firstNew; }
        // every task had its own process; a process id may be given out again once its process has ended, so only the four that ran at
        // the same time must differ
        EXPECT(spawned == 6 && distinct.size() >= 4);
    }

    // 10) too little room for the tar of a tar.gz: the worker reports it with the numbers, Retry() with another folder works
    {
        TaskQueue q(1, MakeExec());
        CompressOptions co;
        co.type = ArchiveType::TarGz;
        SetEnvironmentVariableW(L"LYCAN_TEST_FREE_SPACE", L"100000");  // the worker inherits it
        auto id = q.EnqueueCompress(root / "ts.tar.gz", {root / "src"}, co);
        EXPECT(q.Wait(id, 60000));
        SetEnvironmentVariableW(L"LYCAN_TEST_FREE_SPACE", nullptr);
        TaskInfo ti;
        q.Get(id, ti);
        EXPECT(ti.state == TaskState::Failed && ti.tempSpace && ti.tempNeed >= (3u << 20) && ti.tempHave == 100000);
        EXPECT(!ti.error.empty() && ti.error.English().find(L"@@") == std::wstring::npos);
        EXPECT(!fs::exists(root / "ts.tar.gz"));
        auto id2 = q.Retry(id, root / "scratch2" / "LycanZip");
        EXPECT(id2 != 0 && id2 != id);
        EXPECT(q.Wait(id2, 60000));
        q.Get(id2, ti);
        EXPECT(ti.state == TaskState::Completed && fs::exists(root / "ts.tar.gz") && fs::is_directory(root / "scratch2" / "LycanZip"));
        EXPECT(q.Retry(999999, root) == 0);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::puts("test_worker: all passed");
    return failures ? 1 : 0;
}
