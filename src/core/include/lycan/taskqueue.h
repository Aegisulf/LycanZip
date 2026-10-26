#pragma once
// Task queue: runs compress / extract / test in the background with progress, pause and cancel. Shared by the UI, the context menu and the CLI.
// The callbacks (Listener / password / progress) are called on a worker thread; the UI has to switch to the UI thread itself.
#include "lycan/message.h"
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "lycan/compress.h"
#include "lycan/extract.h"
#include "lycan/taskexec.h"

namespace lycan {

// TaskKind / TaskState live in taskexec.h (shared with the executors).

struct TaskInfo {
    uint64_t id = 0;
    TaskKind kind = TaskKind::Extract;
    TaskState state = TaskState::Queued;
    std::wstring title;          // for example the archive name
    std::filesystem::path target;  // extract: target folder; compress: output file; test: the archive (for the crash report, see crashreport.h)
    uint64_t done = 0, total = 0;  // progress (bytes); total 0 = not known yet
    Message error;          // message when Failed
    // Failed because the scratch folder is too small (see tempdir.h): the numbers and the folder, so the caller can offer
    // another place and Retry() the task
    bool tempSpace = false;
    uint64_t tempNeed = 0, tempHave = 0;
    std::wstring tempDir;
    ExtractReport extract;       // after it is done (Completed / Failed with a partial result)
    CompressReport compress;
    UpdateReport update;
    ScanReport scan;
    HashReport hash;
    bool Finished() const { return state == TaskState::Completed || state == TaskState::Failed || state == TaskState::Cancelled; }
};

class TaskQueue {
public:
    using Listener = std::function<void(const TaskInfo&)>;

    static constexpr unsigned kMaxParallel = 4;

    // maxParallel: number of tasks running at the same time (1..4). The disk is the bottleneck, so the default is 1 (one after the other).
    // executor: where the work is really done (see taskexec.h); the default runs the Core functions on a worker thread of this process.
    explicit TaskQueue(unsigned maxParallel = 1, std::shared_ptr<TaskExecutor> executor = nullptr);
    // Can be changed while running; tasks already running are not affected, tasks started later follow the new limit.
    void SetMaxParallel(unsigned maxParallel);
    ~TaskQueue();  // cancels all tasks and waits for the worker threads to end
    TaskQueue(const TaskQueue&) = delete;

    // State changes and progress (throttled to about once per 50 ms; completion / a state change is always reported). May be empty.
    void SetListener(Listener l);

    uint64_t EnqueueExtract(std::filesystem::path archive, std::filesystem::path destDir, ExtractOptions options = {},
                            OpenOptions open = {}, std::vector<uint32_t> indices = {});
    uint64_t EnqueueCompress(std::filesystem::path output, std::vector<std::filesystem::path> inputs,
                             CompressOptions options = {});
    uint64_t EnqueueTest(std::filesystem::path archive, OpenOptions open = {});
    // Changes an archive in place (see update.h); the archive must not be open anywhere in the app while this runs.
    // Hands every file of the archive to the installed antimalware product (see scan.h); password through open.password
    uint64_t EnqueueScan(std::filesystem::path archive, OpenOptions open = {});
    // Hash values (see hash.h) of files on disk, or of entries of an archive (`archive` + entry indices, password through open)
    uint64_t EnqueueHashFiles(std::vector<std::filesystem::path> files, uint32_t algos);
    uint64_t EnqueueHashEntries(std::filesystem::path archive, std::vector<uint32_t> indices, uint32_t algos, OpenOptions open = {});
    uint64_t EnqueueUpdate(std::filesystem::path archive, std::vector<UpdateOp> ops, UpdateOptions options = {});
    // Runs a finished task again with another scratch folder (a new task; the old one stays as it is). 0 = unknown task.
    uint64_t Retry(uint64_t id, const std::filesystem::path& tempDir);

    // Priority of the tasks (see TaskPriority; honoured by the process executor). SetDefaultPriority applies to tasks
    // enqueued afterwards; SetPriority changes a task that is queued or running (the running worker is told with the
    // next progress acknowledgement).
    void SetDefaultPriority(TaskPriority p);
    void SetPriority(uint64_t id, TaskPriority p);

    // Pause takes effect at the next progress report (7-Zip reports in blocks, usually within milliseconds). A queued task that is paused is not started.
    void Pause(uint64_t id);
    void Resume(uint64_t id);
    void Cancel(uint64_t id);  // no effect on a task that is finished
    void CancelAll();

    std::vector<TaskInfo> Snapshot() const;
    bool Get(uint64_t id, TaskInfo& out) const;

    // Blocks and waits. timeoutMs < 0 = wait without limit; false means a timeout.
    bool Wait(uint64_t id, int timeoutMs = -1) const;
    bool WaitAll(int timeoutMs = -1) const;

private:
    struct Task;
    void Worker();
    void Run(Task& t);
    void Notify(const TaskInfo& info);

    std::shared_ptr<TaskExecutor> executor_;
    mutable std::mutex m_;
    mutable std::condition_variable cv_;       // wakes the worker threads / completion notice
    std::map<uint64_t, std::shared_ptr<Task>> tasks_;
    std::vector<std::thread> workers_;
    Listener listener_;
    std::mutex listenerM_;
    uint64_t nextId_ = 1;
    unsigned maxParallel_ = 1;
    unsigned running_ = 0;
    TaskPriority defaultPriority_ = TaskPriority::Normal;
    bool stopping_ = false;
};

}  // namespace lycan
