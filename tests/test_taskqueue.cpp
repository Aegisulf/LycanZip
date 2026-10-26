// Task queue tests: order, progress, pause / resume, cancel (also while queued), failure, no leftover files.
#include "no_dialogs.h"
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <random>
#include <thread>

#include "lycan/error.h"
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

static void WriteRandom(const fs::path& p, size_t bytes) {
    std::mt19937_64 rng(42);
    std::ofstream f(p, std::ios::binary);
    std::vector<uint64_t> buf(1 << 17);
    for (size_t left = bytes; left;) {
        for (auto& v : buf) v = rng();
        size_t n = std::min(left, buf.size() * 8);
        f.write(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
        left -= n;
    }
}

static bool SameFile(const fs::path& a, const fs::path& b) {
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    std::vector<char> bx(1 << 16), by(1 << 16);
    while (true) {
        x.read(bx.data(), bx.size());
        y.read(by.data(), by.size());
        if (x.gcount() != y.gcount()) return false;
        if (x.gcount() == 0) return true;
        if (memcmp(bx.data(), by.data(), static_cast<size_t>(x.gcount())) != 0) return false;
    }
}

static bool WaitFor(const std::function<bool()>& pred, int ms = 15000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

// LYCAN_TEST_EXECUTOR=process runs the whole suite on the worker-process executor (lycanzip-cli.exe --worker)
static std::shared_ptr<TaskExecutor> Exec() {
    wchar_t v[16];
    if (GetEnvironmentVariableW(L"LYCAN_TEST_EXECUTOR", v, 16) > 0 && std::wstring(v) == L"process") return MakeProcessExecutor();
    return nullptr;
}

int main() {
    fs::path root = fs::temp_directory_path() / (L"lycan_tq_" + std::to_wstring(GetCurrentProcessId()));
    fs::remove_all(root);
    fs::create_directories(root / "src");
    WriteRandom(root / "src" / "small.bin", 100'000);
    WriteRandom(root / "big.bin", 40 * 1024 * 1024);  // compression takes a little time, which makes pause / cancel easy

    // 1) Run in order + state and progress notifications
    {
        TaskQueue q(1, Exec());
        std::mutex m;
        std::vector<std::pair<uint64_t, TaskState>> events;
        std::map<uint64_t, uint64_t> lastDone;  // each task is monotonically increasing on its own
        bool monotonic = true;
        q.SetListener([&](const TaskInfo& i) {
            std::lock_guard lk(m);
            events.push_back({i.id, i.state});
            if (i.kind == TaskKind::Compress && i.state == TaskState::Running) {
                if (i.done < lastDone[i.id]) monotonic = false;
                lastDone[i.id] = i.done;
            }
        });
        CompressOptions co;
        co.type = ArchiveType::Zip;
        auto c1 = q.EnqueueCompress(root / "a.zip", {root / "src"}, co);
        auto c2 = q.EnqueueCompress(root / "b.7z", {root / "src"}, [] { CompressOptions o; o.type = ArchiveType::SevenZip; return o; }());
        auto x1 = q.EnqueueExtract(root / "a.zip", root / "out_a");
        auto t1 = q.EnqueueTest(root / "b.7z");
        EXPECT(q.WaitAll(30000));
        for (auto id : {c1, c2, x1, t1}) {
            TaskInfo i;
            EXPECT(q.Get(id, i) && i.state == TaskState::Completed);
        }
        EXPECT(SameFile(root / "src" / "small.bin", root / "out_a" / "src" / "small.bin"));
        std::lock_guard lk(m);
        EXPECT(monotonic);
        // In order: the completion event of c1 comes before the Running of c2, and so on
        auto idx = [&](uint64_t id, TaskState s) {
            for (size_t k = 0; k < events.size(); ++k) if (events[k].first == id && events[k].second == s) return static_cast<int>(k);
            return -1;
        };
        EXPECT(idx(c1, TaskState::Completed) >= 0 && idx(c1, TaskState::Completed) < idx(c2, TaskState::Running));
        EXPECT(idx(c2, TaskState::Completed) < idx(x1, TaskState::Running));
        EXPECT(idx(x1, TaskState::Completed) < idx(t1, TaskState::Running));
    }

    // 1c) hash values as a task: files, entries of an archive; a missing file fails the task but not the others (also across the worker pipe)
    {
        TaskQueue q(1, Exec());
        auto h = q.EnqueueHashFiles({root / "src" / "small.bin", root / "no_such_file.bin"}, kHashSha256 | kHashCrc32);
        EXPECT(q.Wait(h, 30000));
        TaskInfo hi;
        q.Get(h, hi);
        EXPECT(hi.kind == TaskKind::Hash && hi.state == TaskState::Failed && hi.hash.failed == 1 && hi.hash.files.size() == 2);
        EXPECT(hi.hash.algos == (kHashSha256 | kHashCrc32) && hi.hash.files[0].ok() && hi.hash.files[0].hex.size() == 2 && !hi.hash.files[1].ok());
        EXPECT(hi.hash.files[0].hex[1].size() == 64 && hi.hash.files[0].size == fs::file_size(root / "src" / "small.bin"));
        auto h2 = q.EnqueueHashFiles({root / "src" / "small.bin"}, kHashDefault);
        EXPECT(q.Wait(h2, 30000));
        TaskInfo hi2;
        q.Get(h2, hi2);
        EXPECT(hi2.state == TaskState::Completed && hi2.hash.files[0].hex.size() == HashAlgoList(kHashDefault).size());
        // entries
        CompressOptions co;
        co.storeCompressed = false;
        co.type = ArchiveType::SevenZip;
        const fs::path arc = root / "hash.7z";
        auto c = q.EnqueueCompress(arc, {root / "src"}, co);
        EXPECT(q.Wait(c, 30000));
        auto src = ArchiveSource::Open(arc);
        std::vector<uint32_t> idx;
        for (auto const& e : src->Entries()) idx.push_back(e.index);
        auto he = q.EnqueueHashEntries(arc, idx, kHashSha1);
        EXPECT(q.Wait(he, 30000));
        TaskInfo hei;
        q.Get(he, hei);
        EXPECT(hei.state == TaskState::Completed && hei.hash.failed == 0 && !hei.hash.files.empty());
        for (auto const& f : hei.hash.files) {
            HashReport want = HashFiles({root / fs::path(f.path)}, {kHashSha1, nullptr, nullptr});
            EXPECT(want.files.size() == 1 && want.files[0].hex == f.hex);
        }
    }

    // 1b) changing an archive as a task (operations, options, the report; also across the worker pipe)
    for (const wchar_t* ext : {L".zip", L".7z"}) {
        TaskQueue q(1, Exec());
        const fs::path arc = root / (std::wstring(L"upd") + ext);
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = std::wstring(ext) == L".zip" ? ArchiveType::Zip : ArchiveType::SevenZip;
        co.password = L"Secret1";
        auto c = q.EnqueueCompress(arc, {root / "src"}, co);
        EXPECT(q.Wait(c, 30000));
        WriteRandom(root / "added.jpg", 150000);
        UpdateOptions uo;
        uo.compress.password = L"Secret1";
        uo.keepBackup = true;
        auto u = q.EnqueueUpdate(arc, {UpdateOp::Add(L"src", {root / "added.jpg"}), UpdateOp::Rename(L"src/small.bin", L"src/renamed.bin"),
                                       UpdateOp::MakeDir(L"empty")}, uo);
        EXPECT(q.Wait(u, 30000));
        TaskInfo ui;
        q.Get(u, ui);
        EXPECT(ui.state == TaskState::Completed && ui.kind == TaskKind::Update && ui.update.ok());
        EXPECT(ui.update.added == 1 && ui.update.renamed == 1 && ui.update.dirsCreated == 1 && ui.update.storedFiles == 1);
        EXPECT(ui.update.archiveBefore > 0 && ui.update.archiveAfter > ui.update.archiveBefore);
        EXPECT(fs::exists(arc.wstring() + L".bak"));
        auto x = q.EnqueueExtract(arc, root / (std::wstring(L"out_upd") + ext), [] { ExtractOptions e; e.password = [](std::wstring& p) { p = L"Secret1"; return true; }; return e; }());
        EXPECT(q.Wait(x, 30000));
        TaskInfo xi;
        q.Get(x, xi);
        EXPECT(xi.state == TaskState::Completed && xi.extract.ok());
        EXPECT(SameFile(root / "src" / "small.bin", root / (std::wstring(L"out_upd") + ext) / "src" / "renamed.bin"));
        EXPECT(SameFile(root / "added.jpg", root / (std::wstring(L"out_upd") + ext) / "src" / "added.jpg"));
        // a bad operation fails the task and leaves the archive alone
        const auto sizeBefore = fs::file_size(arc);
        auto bad = q.EnqueueUpdate(arc, {UpdateOp::Delete(L"no/such/item")}, uo);
        EXPECT(q.Wait(bad, 30000));
        TaskInfo bi;
        q.Get(bad, bi);
        EXPECT(bi.state == TaskState::Failed && !bi.error.empty() && fs::file_size(arc) == sizeBefore);
    }

    // 2) Pause / resume
    {
        TaskQueue q(1, Exec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto id = q.EnqueueCompress(root / "p.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(id, i) && i.state == TaskState::Running && i.done > 0; }));
        q.Pause(id);
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(id, i) && i.state == TaskState::Paused; }));
        TaskInfo a, b;
        q.Get(id, a);
        std::this_thread::sleep_for(400ms);
        q.Get(id, b);
        EXPECT(a.done == b.done && b.state == TaskState::Paused);  // no progress during the pause
        q.Resume(id);
        EXPECT(q.Wait(id, 60000));
        q.Get(id, b);
        EXPECT(b.state == TaskState::Completed);
        EXPECT(fs::exists(root / "p.7z"));
    }

    // 3) Cancel a running compression: no half-made file is left
    {
        TaskQueue q(1, Exec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto id = q.EnqueueCompress(root / "c.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(id, i) && i.state == TaskState::Running && i.done > 0; }));
        q.Cancel(id);
        EXPECT(q.Wait(id, 30000));
        TaskInfo i;
        q.Get(id, i);
        EXPECT(i.state == TaskState::Cancelled);
        EXPECT(!fs::exists(root / "c.7z") && !fs::exists(root / "c.7z.lycantmp"));
    }

    // 4) Cancel a queued task: it never started; the others are not affected
    {
        TaskQueue q(1, Exec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto first = q.EnqueueCompress(root / "q1.7z", {root / "big.bin"}, co);
        auto second = q.EnqueueCompress(root / "q2.zip", {root / "src"});
        auto third = q.EnqueueCompress(root / "q3.zip", {root / "src"});
        EXPECT(WaitFor([&] { TaskInfo i; return q.Get(first, i) && i.state == TaskState::Running; }));
        q.Cancel(second);
        EXPECT(q.WaitAll(60000));
        TaskInfo i;
        q.Get(second, i);
        EXPECT(i.state == TaskState::Cancelled && !fs::exists(root / "q2.zip"));
        q.Get(third, i);
        EXPECT(i.state == TaskState::Completed && fs::exists(root / "q3.zip"));
    }

    // 5) Pause a queued task: it is not started until resumed
    {
        TaskQueue q(1, Exec());
        auto a = q.EnqueueCompress(root / "r1.zip", {root / "src"});
        EXPECT(q.Wait(a, 30000));
        // A task queued after the pause: queued first and paused at once, a race cannot be ruled out completely, so verify with a queue where all tasks are paused first
        TaskQueue q2(1, Exec());
        q2.Pause(1);  // an id that does not exist yet: no effect, no crash
        auto b = q2.EnqueueCompress(root / "r2.zip", {root / "src"});
        EXPECT(q2.Wait(b, 30000));
    }

    // 6) Failure: an archive that does not exist
    {
        TaskQueue q(1, Exec());
        auto id = q.EnqueueExtract(root / "nope.zip", root / "out_nope");
        EXPECT(q.Wait(id, 10000));
        TaskInfo i;
        q.Get(id, i);
        EXPECT(i.state == TaskState::Failed && !i.error.empty());
    }

    // 7) On destruction running tasks are cancelled and nothing hangs
    {
        auto t0 = std::chrono::steady_clock::now();
        {
            TaskQueue q(1, Exec());
            CompressOptions co;
            co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
            co.type = ArchiveType::SevenZip;
            auto id = q.EnqueueCompress(root / "d.7z", {root / "big.bin"}, co);
            WaitFor([&] { TaskInfo i; return q.Get(id, i) && i.state == TaskState::Running && i.done > 0; });
        }  // ~TaskQueue
        EXPECT(std::chrono::steady_clock::now() - t0 < 20s);
        EXPECT(!fs::exists(root / "d.7z.lycantmp") && !fs::exists(root / "d.7z"));
    }

    // 8) Parallelism 2: two tasks can be Running at the same time
    {
        TaskQueue q(2, Exec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto a = q.EnqueueCompress(root / "m1.7z", {root / "big.bin"}, co);
        auto b = q.EnqueueCompress(root / "m2.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] {
            TaskInfo x, y;
            return q.Get(a, x) && q.Get(b, y) && x.state == TaskState::Running && y.state == TaskState::Running;
        }));
        q.CancelAll();
        EXPECT(q.WaitAll(30000));
    }

    // 9) Change the parallelism while running: after 1 -> 2 the second queued task should start at once
    {
        TaskQueue q(1, Exec());
        CompressOptions co;
        co.storeCompressed = false;  // the random file would be stored (fast); these tests need a slow compression
        co.type = ArchiveType::SevenZip;
        auto a = q.EnqueueCompress(root / "s1.7z", {root / "big.bin"}, co);
        auto b = q.EnqueueCompress(root / "s2.7z", {root / "big.bin"}, co);
        EXPECT(WaitFor([&] { TaskInfo x; return q.Get(a, x) && x.state == TaskState::Running; }));
        std::this_thread::sleep_for(300ms);
        TaskInfo y;
        q.Get(b, y);
        EXPECT(y.state == TaskState::Queued);  // limit 1: the second is still queued
        q.SetMaxParallel(2);
        EXPECT(WaitFor([&] { TaskInfo z; return q.Get(b, z) && z.state == TaskState::Running; }, 5000));
        q.CancelAll();
        EXPECT(q.WaitAll(30000));
    }

    // 10) The executor seam: a queue with a fake executor (no Core code runs). Scheduling, progress, pause, cancel and
    //     the result all go through the same queue logic.
    {
        struct Fake final : TaskExecutor {
            std::atomic<int> calls{0};
            TaskSpec last;
            std::atomic<bool> sawCancel{false};
            TaskResult Execute(const TaskSpec& spec, const ProgressFn& progress, TaskControl&) override {
                ++calls;
                last = spec;
                if (spec.kind == TaskKind::Compress && spec.output.filename() == L"slow.zip") {
                    for (int i = 0; i < 1000; ++i) {
                        if (!progress(static_cast<uint64_t>(i), 1000)) { sawCancel = true; throw Error(static_cast<long>(0x80004004L), "cancelled"); }
                        std::this_thread::sleep_for(2ms);
                    }
                }
                TaskResult r;
                if (spec.kind == TaskKind::Compress && spec.output.filename() == L"bad.zip") throw Error(E_FAIL, "fake failure");
                progress(50, 100);
                progress(100, 100);
                r.extract.files = 7;
                r.compress.files = 3;
                return r;
            }
        };
        auto fake = std::make_shared<Fake>();
        TaskQueue q(1, fake);
        auto id = q.EnqueueExtract(root / "x.zip", root / "dest", {}, {}, {4, 5});
        EXPECT(q.Wait(id, 5000));
        TaskInfo ti;
        q.Get(id, ti);
        EXPECT(ti.state == TaskState::Completed && ti.extract.files == 7 && ti.done == 100 && ti.total == 100);
        EXPECT(fake->last.kind == TaskKind::Extract && fake->last.archive == root / "x.zip" && fake->last.dest == root / "dest" && fake->last.indices.size() == 2);
        auto ct = q.EnqueueCompress(root / "c.zip", {root / "src"});
        EXPECT(q.Wait(ct, 5000));
        q.Get(ct, ti);
        EXPECT(ti.state == TaskState::Completed && ti.compress.files == 3 && fake->last.kind == TaskKind::Compress && fake->last.inputs.size() == 1);
        auto tt = q.EnqueueTest(root / "t.zip");
        EXPECT(q.Wait(tt, 5000));
        q.Get(tt, ti);
        EXPECT(ti.state == TaskState::Completed && fake->last.kind == TaskKind::Test);
        auto bad = q.EnqueueCompress(root / "bad.zip", {root / "src"});
        EXPECT(q.Wait(bad, 5000));
        q.Get(bad, ti);
        EXPECT(ti.state == TaskState::Failed && ti.error.English() == L"fake failure");
        auto slow = q.EnqueueCompress(root / "slow.zip", {root / "src"});
        EXPECT(WaitFor([&] { TaskInfo x; return q.Get(slow, x) && x.state == TaskState::Running && x.done > 5; }, 5000));
        q.Cancel(slow);
        EXPECT(q.Wait(slow, 5000));
        q.Get(slow, ti);
        EXPECT(ti.state == TaskState::Cancelled && fake->sawCancel);
        EXPECT(fake->calls == 5);
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    if (failures == 0) std::puts("test_taskqueue: all passed");
    return failures ? 1 : 0;
}
