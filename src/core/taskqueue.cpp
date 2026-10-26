#include "lycan/taskqueue.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <utility>

#include "lycan/diag.h"
#include "lycan/error.h"
#include "lycan/tempdir.h"

namespace lycan {
namespace fs = std::filesystem;
namespace {

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

class NullSink final : public ByteSink {
public:
    void Write(const void*, size_t) override {}
};

}  // namespace

struct TaskQueue::Task {
    TaskInfo info;
    bool paused = false;
    bool cancelled = false;
    std::chrono::steady_clock::time_point lastNotify{};

    // What the task is (does not change after it is made, can be read without the lock)
    TaskSpec spec;
    TaskControl control;  // changes while running (priority)
};

TaskQueue::TaskQueue(unsigned maxParallel, std::shared_ptr<TaskExecutor> executor)
    : executor_(executor ? std::move(executor) : MakeInProcessExecutor()) {
    maxParallel_ = std::clamp(maxParallel, 1u, kMaxParallel);
    // The worker threads are always created up to the limit (an idle one just sleeps); the real parallelism is set by maxParallel_, so it can be changed while running
    for (unsigned i = 0; i < kMaxParallel; ++i) workers_.emplace_back([this] { Worker(); });
}

void TaskQueue::SetMaxParallel(unsigned maxParallel) {
    {
        std::lock_guard lk(m_);
        maxParallel_ = std::clamp(maxParallel, 1u, kMaxParallel);
    }
    cv_.notify_all();
}

TaskQueue::~TaskQueue() {
    std::vector<TaskInfo> cancelledNow;
    {
        std::lock_guard lk(m_);
        stopping_ = true;
        for (auto& [id, t] : tasks_) {
            if (t->info.state == TaskState::Queued) {
                t->info.state = TaskState::Cancelled;
                cancelledNow.push_back(t->info);
            }
        }
    }
    cv_.notify_all();
    for (auto& w : workers_) w.join();
    for (auto& i : cancelledNow) Notify(i);
}

void TaskQueue::SetListener(Listener l) {
    std::lock_guard lk(listenerM_);
    listener_ = std::move(l);
}

void TaskQueue::Notify(const TaskInfo& info) {
    std::lock_guard lk(listenerM_);
    if (listener_) listener_(info);
}

uint64_t TaskQueue::EnqueueExtract(fs::path archive, fs::path destDir, ExtractOptions options, OpenOptions open,
                                   std::vector<uint32_t> indices) {
    auto t = std::make_shared<Task>();
    t->info.kind = TaskKind::Extract;
    t->info.title = archive.filename().wstring();
    t->info.target = destDir;
    t->spec.kind = TaskKind::Extract;
    t->spec.archive = std::move(archive);
    t->spec.dest = std::move(destDir);
    t->spec.extractOptions = std::move(options);
    t->spec.openOptions = std::move(open);
    t->spec.indices = std::move(indices);
    TaskInfo copy;
    uint64_t id;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        id = t->info.id = nextId_++;
        tasks_[id] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return id;
}

uint64_t TaskQueue::EnqueueTest(fs::path archive, OpenOptions open) {
    auto t = std::make_shared<Task>();
    t->info.kind = TaskKind::Test;
    t->info.title = archive.filename().wstring();
    t->info.target = archive;
    t->spec.kind = TaskKind::Test;
    t->spec.archive = std::move(archive);
    t->spec.openOptions = std::move(open);
    TaskInfo copy;
    uint64_t id;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        id = t->info.id = nextId_++;
        tasks_[id] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return id;
}

uint64_t TaskQueue::EnqueueScan(fs::path archive, OpenOptions open) {
    auto t = std::make_shared<Task>();
    t->info.kind = TaskKind::Scan;
    t->info.title = archive.filename().wstring();
    t->info.target = archive;
    t->spec.kind = TaskKind::Scan;
    t->spec.archive = std::move(archive);
    t->spec.openOptions = std::move(open);
    TaskInfo copy;
    uint64_t id;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        id = t->info.id = nextId_++;
        tasks_[id] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return id;
}

uint64_t TaskQueue::EnqueueHashFiles(std::vector<fs::path> files, uint32_t algos) {
    auto t = std::make_shared<Task>();
    t->info.kind = TaskKind::Hash;
    t->info.title = files.size() == 1 ? files[0].filename().wstring() : std::to_wstring(files.size()) + L" files";
    if (!files.empty()) t->info.target = files[0];
    t->spec.kind = TaskKind::Hash;
    t->spec.inputs = std::move(files);
    t->spec.hashAlgos = NormalizeHashAlgos(algos);
    TaskInfo copy;
    uint64_t id;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        id = t->info.id = nextId_++;
        tasks_[id] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return id;
}

uint64_t TaskQueue::EnqueueHashEntries(fs::path archive, std::vector<uint32_t> indices, uint32_t algos, OpenOptions open) {
    auto t = std::make_shared<Task>();
    t->info.kind = TaskKind::Hash;
    t->info.title = archive.filename().wstring();
    t->info.target = archive;
    t->spec.kind = TaskKind::Hash;
    t->spec.archive = std::move(archive);
    t->spec.indices = std::move(indices);
    t->spec.openOptions = std::move(open);
    t->spec.hashAlgos = NormalizeHashAlgos(algos);
    TaskInfo copy;
    uint64_t id;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        id = t->info.id = nextId_++;
        tasks_[id] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return id;
}

uint64_t TaskQueue::EnqueueUpdate(fs::path archive, std::vector<UpdateOp> ops, UpdateOptions options) {
    auto t = std::make_shared<Task>();
    t->info.kind = TaskKind::Update;
    t->info.title = archive.filename().wstring();
    t->info.target = archive;
    t->spec.kind = TaskKind::Update;
    t->spec.archive = std::move(archive);
    t->spec.updateOps = std::move(ops);
    t->spec.updateOptions = std::move(options);
    TaskInfo copy;
    uint64_t id;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        id = t->info.id = nextId_++;
        tasks_[id] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return id;
}

uint64_t TaskQueue::EnqueueCompress(fs::path output, std::vector<fs::path> inputs, CompressOptions options) {
    auto t = std::make_shared<Task>();
    t->info.kind = TaskKind::Compress;
    t->info.title = output.filename().wstring();
    t->info.target = output;
    t->spec.kind = TaskKind::Compress;
    t->spec.output = std::move(output);
    t->spec.inputs = std::move(inputs);
    t->spec.compressOptions = std::move(options);
    TaskInfo copy;
    uint64_t id;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        id = t->info.id = nextId_++;
        tasks_[id] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return id;
}

uint64_t TaskQueue::Retry(uint64_t id, const fs::path& tempDir) {
    auto t = std::make_shared<Task>();
    {
        std::lock_guard lk(m_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return 0;
        t->spec = it->second->spec;
        t->info.kind = it->second->info.kind;
        t->info.title = it->second->info.title;
        t->info.target = it->second->info.target;
    }
    t->spec.openOptions.tempDir = tempDir;
    t->spec.compressOptions.tempDir = tempDir;
    t->spec.updateOptions.compress.tempDir = tempDir;
    TaskInfo copy;
    uint64_t nid;
    {
        std::lock_guard lk(m_);
        t->spec.priority = defaultPriority_;
        t->control.priority = defaultPriority_;
        nid = t->info.id = nextId_++;
        tasks_[nid] = t;
        copy = t->info;
    }
    cv_.notify_all();
    Notify(copy);
    return nid;
}

void TaskQueue::SetDefaultPriority(TaskPriority p) {
    std::lock_guard lk(m_);
    defaultPriority_ = p;
}

void TaskQueue::SetPriority(uint64_t id, TaskPriority p) {
    std::lock_guard lk(m_);
    auto it = tasks_.find(id);
    if (it != tasks_.end() && !it->second->info.Finished()) it->second->control.priority = p;
}

void TaskQueue::Pause(uint64_t id) {
    std::lock_guard lk(m_);
    auto it = tasks_.find(id);
    if (it != tasks_.end() && !it->second->info.Finished()) it->second->paused = true;
}

void TaskQueue::Resume(uint64_t id) {
    {
        std::lock_guard lk(m_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) return;
        it->second->paused = false;
    }
    cv_.notify_all();
}

void TaskQueue::Cancel(uint64_t id) {
    TaskInfo copy;
    bool notify = false;
    {
        std::lock_guard lk(m_);
        auto it = tasks_.find(id);
        if (it == tasks_.end() || it->second->info.Finished()) return;
        auto& t = *it->second;
        t.cancelled = true;
        if (t.info.state == TaskState::Queued) {  // not started yet: finish it right away
            t.info.state = TaskState::Cancelled;
            copy = t.info;
            notify = true;
        }
    }
    cv_.notify_all();
    if (notify) Notify(copy);
}

void TaskQueue::CancelAll() {
    std::vector<uint64_t> ids;
    {
        std::lock_guard lk(m_);
        for (auto& [id, t] : tasks_) ids.push_back(id);
    }
    for (auto id : ids) Cancel(id);
}

std::vector<TaskInfo> TaskQueue::Snapshot() const {
    std::lock_guard lk(m_);
    std::vector<TaskInfo> v;
    for (auto& [id, t] : tasks_) v.push_back(t->info);
    return v;
}

bool TaskQueue::Get(uint64_t id, TaskInfo& out) const {
    std::lock_guard lk(m_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return false;
    out = it->second->info;
    return true;
}

bool TaskQueue::Wait(uint64_t id, int timeoutMs) const {
    std::unique_lock lk(m_);
    auto done = [&] {
        auto it = tasks_.find(id);
        return it == tasks_.end() || it->second->info.Finished();
    };
    if (timeoutMs < 0) { cv_.wait(lk, done); return true; }
    return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), done);
}

bool TaskQueue::WaitAll(int timeoutMs) const {
    std::unique_lock lk(m_);
    auto done = [&] {
        return std::all_of(tasks_.begin(), tasks_.end(), [](auto& p) { return p.second->info.Finished(); });
    };
    if (timeoutMs < 0) { cv_.wait(lk, done); return true; }
    return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), done);
}

void TaskQueue::Worker() {
    std::unique_lock lk(m_);
    for (;;) {
        std::shared_ptr<Task> next;
        cv_.wait(lk, [&] {
            if (stopping_) return true;
            if (running_ >= maxParallel_) return false;
            for (auto& [id, t] : tasks_)
                if (t->info.state == TaskState::Queued && !t->paused && !t->cancelled) { next = t; return true; }
            return false;
        });
        if (stopping_) return;
        next->info.state = TaskState::Running;
        ++running_;
        TaskInfo started = next->info;
        lk.unlock();
        Notify(started);
        Run(*next);
        lk.lock();
        --running_;
        cv_.notify_all();  // a slot is free: let the other worker threads check whether a task is waiting
    }
}

namespace {
const wchar_t* KindName(TaskKind k) {
    switch (k) {
        case TaskKind::Extract: return L"extract";
        case TaskKind::Compress: return L"compress";
        case TaskKind::Test: return L"test";
        case TaskKind::Update: return L"update";
        case TaskKind::Scan: return L"scan";
        case TaskKind::Hash: return L"hash";
    }
    return L"?";
}
}  // namespace

void TaskQueue::Run(Task& t) {
    using clock = std::chrono::steady_clock;
    const auto startedAt = clock::now();
    diag::Info(L"task {} ({}) started: {}", t.info.id, KindName(t.info.kind), diag::Name(t.info.target));

    // Progress callback: updates the state, handles pause / cancel, throttles the notifications
    auto onProgress = [this, &t](uint64_t done, uint64_t total) -> bool {
        TaskInfo toNotify;
        bool doNotify = false;
        bool keepGoing;
        {
            std::unique_lock lk(m_);
            t.info.done = done;
            t.info.total = total;
            if (t.paused && !t.cancelled && !stopping_) {
                t.info.state = TaskState::Paused;
                TaskInfo c = t.info;
                lk.unlock();
                Notify(c);
                lk.lock();
                cv_.wait(lk, [&] { return !t.paused || t.cancelled || stopping_; });
                if (!t.cancelled && !stopping_) {
                    t.info.state = TaskState::Running;
                    toNotify = t.info;
                    doNotify = true;
                }
            }
            keepGoing = !t.cancelled && !stopping_;
            auto now = clock::now();
            if (!doNotify && now - t.lastNotify >= std::chrono::milliseconds(50)) {
                t.lastNotify = now;
                toNotify = t.info;
                doNotify = true;
            }
        }
        if (doNotify) Notify(toNotify);
        return keepGoing;
    };

    TaskState finalState = TaskState::Completed;
    Message error;
    bool tempSpace = false;
    uint64_t tempNeed = 0, tempHave = 0;
    std::wstring tempDir;
    ExtractReport erep;
    CompressReport crep;
    UpdateReport urep;
    ScanReport srep;
    HashReport hrep;
    try {
        TaskResult r = executor_->Execute(t.spec, onProgress, t.control);
        finalState = r.state;
        error = std::move(r.error);
        erep = std::move(r.extract);
        crep = std::move(r.compress);
        urep = std::move(r.update);
        srep = std::move(r.scan);
        hrep = std::move(r.hash);
    } catch (const Error& e) {
        std::lock_guard lk(m_);
        finalState = (e.cancelled() || t.cancelled) ? TaskState::Cancelled : TaskState::Failed;
        if (finalState == TaskState::Failed) {
            error = e.message();
            if (e.tempSpace()) {
                if (ParseTempSpaceMessage(e.message(), tempNeed, tempHave, tempDir)) tempSpace = true;
            }
        }
    } catch (const std::exception& e) {
        finalState = TaskState::Failed;
        error = FromUtf8(e.what());
    }

    {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - startedAt).count();
        if (finalState == TaskState::Failed) diag::Warn(L"task {} ({}) failed after {} ms: {}", t.info.id, KindName(t.info.kind), ms, error.English());
        else diag::Info(L"task {} ({}) {} after {} ms", t.info.id, KindName(t.info.kind), finalState == TaskState::Cancelled ? L"cancelled" : L"finished", ms);
    }
    TaskInfo done;
    {
        std::lock_guard lk(m_);
        t.info.state = finalState;
        t.info.error = error;
        t.info.tempSpace = tempSpace;
        t.info.tempNeed = tempNeed;
        t.info.tempHave = tempHave;
        t.info.tempDir = tempDir;
        t.info.extract = std::move(erep);
        t.info.compress = std::move(crep);
        t.info.update = std::move(urep);
        t.info.scan = std::move(srep);
        t.info.hash = std::move(hrep);
        if (finalState == TaskState::Completed && t.info.total) t.info.done = t.info.total;
        done = t.info;
    }
    cv_.notify_all();
    Notify(done);
}

}  // namespace lycan
