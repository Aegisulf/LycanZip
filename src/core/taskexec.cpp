#include "lycan/taskexec.h"

#include "lycan/archive_source.h"

namespace lycan {
namespace {

class InProcessExecutor final : public TaskExecutor {
public:
    TaskResult Execute(const TaskSpec& spec, const ProgressFn& progress, TaskControl&) override {
        TaskResult r;
        switch (spec.kind) {
            case TaskKind::Extract: {
                OpenOptions oo = spec.openOptions;
                oo.progress = progress;
                auto src = ArchiveSource::Open(spec.archive, oo);
                ExtractOptions eo = spec.extractOptions;
                eo.progress = progress;
                r.extract = ExtractTo(*src, spec.dest, eo, spec.indices);
                if (!r.extract.ok()) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_extract_failed"); }
                break;
            }
            case TaskKind::Test: {
                OpenOptions oo = spec.openOptions;
                oo.progress = progress;
                auto src = ArchiveSource::Open(spec.archive, oo);
                // 7-Zip native test mode: one pass over the archive (per-entry ReadEntry is O(n^2) for solid archives)
                ExtractOptions to;
                to.progress = progress;
                to.password = spec.openOptions.password;
                TestReport trep = TestArchive(*src, to);
                for (auto& m : trep.errors) r.extract.errors.push_back(m);
                r.extract.warnings = trep.warnings;
                r.extract.files = trep.files;
                r.extract.wrongPassword = trep.wrongPassword;
                if (!trep.errors.empty()) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_test_failed"); }
                break;
            }
            case TaskKind::Compress: {
                CompressOptions co = spec.compressOptions;
                co.progress = progress;
                r.compress = CreateArchive(spec.output, spec.inputs, co);
                if (!r.compress.ok()) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_unreadable"); }
                break;
            }
            case TaskKind::Scan: {
                OpenOptions oo = spec.openOptions;
                oo.progress = progress;
                auto src = ArchiveSource::Open(spec.archive, oo);
                ScanOptions so;
                so.password = spec.openOptions.password;
                so.progress = progress;
                r.scan = ScanArchive(*src, so);
                if (r.scan.amsiHr != 0) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_no_amsi"); }
                else if (r.scan.Found()) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_threat_found"); }
                break;
            }
            case TaskKind::Hash: {
                HashOptions ho;
                ho.algos = spec.hashAlgos;
                ho.progress = progress;
                ho.password = spec.openOptions.password;
                if (spec.archive.empty()) {
                    r.hash = HashFiles(spec.inputs, ho);
                } else {
                    OpenOptions oo = spec.openOptions;
                    oo.progress = progress;
                    auto src = ArchiveSource::Open(spec.archive, oo);
                    r.hash = HashEntries(*src, spec.indices, ho);
                }
                if (r.hash.failed) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_hash_failed"); }
                break;
            }
            case TaskKind::Update: {
                UpdateOptions uo = spec.updateOptions;
                uo.progress = progress;
                r.update = UpdateArchive(spec.archive, spec.updateOps, uo);
                if (!r.update.ok()) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_unreadable"); }
                break;
            }
            case TaskKind::Repack: {
                RepackOptions ro;
                ro.compress = spec.compressOptions;
                ro.open = spec.openOptions;
                ro.inPlace = spec.repackInPlace;
                ro.skipDamaged = spec.repackSkipDamaged;
                ro.test = spec.repackTest;
                ro.scratch = spec.openOptions.tempDir;
                ro.progress = progress;
                r.repack = RepackArchive(spec.archive, spec.output, ro);
                if (!r.repack.ok()) { r.state = TaskState::Failed; r.error = Message::Make(L"core.task_test_failed"); }
                break;
            }
        }
        return r;
    }
};

}  // namespace

std::shared_ptr<TaskExecutor> MakeInProcessExecutor() { return std::make_shared<InProcessExecutor>(); }

}  // namespace lycan
