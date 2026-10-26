#pragma once
// The part of a task that does the actual work, separated from the queue (scheduling, pause / cancel, state, notification).
//
// TaskQueue decides WHEN a task runs and keeps its state; a TaskExecutor decides WHERE and HOW it runs. The default
// executor runs the Core functions on the calling (worker) thread. The seam exists so that an executor can later run
// the work in a separate worker process (priority per task, crash isolation, background work without a window).
#include "lycan/message.h"
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "lycan/compress.h"
#include "lycan/extract.h"
#include "lycan/hash.h"
#include "lycan/scan.h"
#include "lycan/update.h"

namespace lycan {

enum class TaskKind { Extract, Compress, Test, Update, Scan, Hash };
enum class TaskState { Queued, Running, Paused, Completed, Failed, Cancelled };

// How much the machine may be used for a task. Only an executor that runs the work in its own process can honour it
// fully (7z.dll starts its own threads): Normal = as usual; BelowNormal = lower CPU priority; Background = Windows'
// background mode (low CPU, I/O and memory priority) plus efficiency mode (EcoQoS).
enum class TaskPriority { Normal, BelowNormal, Background };

// Shared between the queue and a running Execute(): the queue may change things while the task runs.
struct TaskControl {
    std::atomic<TaskPriority> priority{TaskPriority::Normal};
};

// What to do. Plain data plus the callbacks of the Core options (password / conflict). `progress` of the options is
// ignored: the executor gets its own progress function in Execute().
struct TaskSpec {
    TaskKind kind = TaskKind::Extract;
    std::filesystem::path archive, dest, output;  // extract / test: archive (+ dest); compress: output
    std::vector<std::filesystem::path> inputs;    // compress
    ExtractOptions extractOptions;
    OpenOptions openOptions;
    CompressOptions compressOptions;
    std::vector<uint32_t> indices;                // extract: empty = everything
    std::vector<UpdateOp> updateOps;              // update: change `archive` in place (update.h)
    UpdateOptions updateOptions;
    uint32_t hashAlgos = kHashDefault;            // hash: the algorithms; the files are `inputs`, or the `indices` of the entries of `archive`
    TaskPriority priority = TaskPriority::Normal; // at start (later changes arrive through TaskControl)
};

// What came out. Completed / Failed carry the reports; a cancelled task is reported by throwing lycan::Error with
// cancelled() == true (like the Core functions do) or by returning Cancelled.
struct TaskResult {
    TaskState state = TaskState::Completed;
    Message error;
    ExtractReport extract;
    CompressReport compress;
    UpdateReport update;
    ScanReport scan;
    HashReport hash;
};

class TaskExecutor {
public:
    virtual ~TaskExecutor() = default;
    // Runs one task to completion; blocks the calling thread (TaskQueue calls it from its own worker threads, possibly
    // several at once). `progress(done, total)` returns false when the task must stop (cancel); it also blocks while the
    // task is paused. May throw lycan::Error / std::exception exactly like the Core functions.
    virtual TaskResult Execute(const TaskSpec& spec, const ProgressFn& progress, TaskControl& control) = 0;
};

// Runs the Core functions in this process (the default). Ignores the priority.
std::shared_ptr<TaskExecutor> MakeInProcessExecutor();

struct ProcessExecutorOptions {
    std::filesystem::path workerExe;                  // lycanzip-cli.exe (started with --worker <pipe>); empty = next to this module
    std::function<void(unsigned long pid)> onSpawn;   // for tests: called with the process id of every worker
};

// Runs every task in its own worker process (priority per task, a crash of 7z.dll only fails that task). The worker
// processes are tied to this executor by a job object: they end when the executor (or its process) goes away.
// The callbacks of the options (progress, password, conflict) still run in this process, on the calling thread.
std::shared_ptr<TaskExecutor> MakeProcessExecutor(ProcessExecutorOptions options = {});

// Entry point of a worker process (lycanzip-cli --worker <read> <write>, the numbers are inherited pipe handles):
// serves one task and returns the exit code.
int RunWorker(uintptr_t readHandle, uintptr_t writeHandle);

}  // namespace lycan
