#include "pch.h"
#include "lycan/i18n.h"
#include "TaskRow.h"
#include "ScanText.h"
#if __has_include("TaskRow.g.cpp")
#include "TaskRow.g.cpp"
#endif

namespace winrt::LycanZip::implementation
{
    static std::wstring SizeText(uint64_t b)
    {
        wchar_t buf[64];
        const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
        double v = static_cast<double>(b);
        int u = 0;
        while (v >= 1024 && u < 4) { v /= 1024; ++u; }
        if (u == 0) swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(b));
        else swprintf_s(buf, L"%.1f %s", v, units[u]);
        return buf;
    }

    static std::wstring DurationText(double sec)
    {
        long long s = static_cast<long long>(sec + 0.5);
        if (s < 1) return lycan::Tr(L"task.under_1_second");
        if (s < 60) return lycan::Tf(L"preview.s", s);
        if (s < 3600) return lycan::Tf(L"task.min_s", s / 60, s % 60);
        return lycan::Tf(L"task.h_min", s / 3600, (s % 3600) / 60);
    }

    // The lit part of the moon: the right side is a fixed half circle, the left is the arc of the "terminator" ellipse; p<0.5 bulges right (crescent), p>0.5 bulges left (gibbous)
    static Microsoft::UI::Xaml::Media::Geometry MakeMoonGeometry(double p)
    {
        using namespace Microsoft::UI::Xaml::Media;
        if (p <= 0.004) return nullptr;
        constexpr double R = 20;
        PathFigure fig;
        fig.StartPoint({static_cast<float>(R), 0});
        fig.IsClosed(true);
        ArcSegment a1;
        a1.Point({static_cast<float>(R), static_cast<float>(2 * R)});
        a1.Size({static_cast<float>(R), static_cast<float>(R)});
        a1.SweepDirection(SweepDirection::Clockwise);
        ArcSegment a2;
        a2.Point({static_cast<float>(R), 0});
        double rx = std::max(0.01, std::abs(1.0 - 2.0 * p) * R);
        a2.Size({static_cast<float>(rx), static_cast<float>(R)});
        a2.SweepDirection(p < 0.5 ? SweepDirection::Counterclockwise : SweepDirection::Clockwise);
        fig.Segments().Append(a1);
        fig.Segments().Append(a2);
        PathGeometry g;
        g.Figures().Append(fig);
        return g;
    }

    // Shared brushes: 0 silver-white (moonlight), 1 light red (failed), 2 grey (cancelled), 3 dark symbol, 4 white symbol, 5 dark blue-grey when paused (so the white pause symbol can be seen)
    static Microsoft::UI::Xaml::Media::Brush SharedBrush(int i)
    {
        static std::vector<Microsoft::UI::Xaml::Media::SolidColorBrush> brushes(6, nullptr);
        static const uint32_t rgb[6] = {0xE6EEFF, 0xFF8A80, 0x8A93A8, 0x0A1030, 0xFFFFFF, 0x6F7FAA};
        if (!brushes[i])
        {
            uint32_t v = rgb[i];
            brushes[i] = Microsoft::UI::Xaml::Media::SolidColorBrush{
                Windows::UI::Color{255, static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)}};
        }
        return brushes[i];
    }

    void TaskRow::Apply(lycan::TaskInfo const& info)
    {
        using lycan::TaskKind;
        using lycan::TaskState;

        const wchar_t* verb = info.kind == TaskKind::Extract ? lycan::Tr(L"main.extraction") : info.kind == TaskKind::Compress ? lycan::Tr(L"main.compression")
                              : info.kind == TaskKind::Update ? lycan::Tr(L"main.change_archive") : info.kind == TaskKind::Scan ? lycan::Tr(L"main.scan_malware")
                              : info.kind == TaskKind::Repack ? lycan::Tr(info.repackInPlace ? L"main.password_removal" : L"main.conversion") : lycan::Tr(L"task.test");
        title_ = hstring{std::wstring(verb) + std::wstring{lycan::Tr(L"fmt.gap")} + info.title};

        std::wstring status;
        switch (info.state)
        {
        case TaskState::Queued: status = lycan::Tr(L"task.waiting"); break;
        case TaskState::Running:
        {
            // Speed: sampled at most every 0.4 s, exponentially smoothed; remaining time = remaining amount / speed
            auto now = std::chrono::steady_clock::now();
            if (lastTime_.time_since_epoch().count() == 0 || info.done < lastDone_) { lastTime_ = now; lastDone_ = info.done; speed_ = 0; }
            else
            {
                double dt = std::chrono::duration<double>(now - lastTime_).count();
                if (dt >= 0.4)
                {
                    double inst = static_cast<double>(info.done - lastDone_) / dt;
                    speed_ = speed_ > 0 ? 0.7 * speed_ + 0.3 * inst : inst;
                    lastTime_ = now;
                    lastDone_ = info.done;
                }
            }
            if (info.total)
            {
                wchar_t pct[16];
                swprintf_s(pct, L"%.0f%%", 100.0 * static_cast<double>(info.done) / static_cast<double>(info.total));
                status = pct;
                if (speed_ > 1)
                {
                    status += lycan::Tf(L"task.s", SizeText(static_cast<uint64_t>(speed_)));
                    status += lycan::Tf(L"task.left", DurationText(static_cast<double>(info.total - std::min(info.done, info.total)) / speed_));
                }
            }
            else status = lycan::Tf(L"task.working", SizeText(info.done)) + (speed_ > 1 ? lycan::Tf(L"task.s", SizeText(static_cast<uint64_t>(speed_))) : std::wstring());
            break;
        }
        case TaskState::Paused: status = lycan::Tf(L"task.paused", info.total ? SizeText(info.done) + L" / " + SizeText(info.total) : SizeText(info.done)); break;
        case TaskState::Completed:
            if (info.kind == TaskKind::Extract)
            {
                status = lycan::Tf(L"task.done", info.extract.files);
                if (!info.extract.skipped.empty()) status += lycan::Tf(L"task.skipped", info.extract.skipped.size());
                if (info.extract.macMetadataSkipped) status += lycan::Tf(L"task.macos_metadata_item_s_skipped", info.extract.macMetadataSkipped);
                if (!info.extract.rejected.empty()) status += lycan::Tf(L"task.unsafe_rejected", info.extract.rejected.size());
                if (!info.extract.renamed.empty()) status += lycan::Tf(L"task.renamed", info.extract.renamed.size());
                if (!info.extract.warnings.empty()) status += lycan::Tf(L"main.data_after_end_compressed_stream", info.extract.warnings.size());
            }
            else if (info.kind == TaskKind::Compress)
            {
                status = lycan::Tf(L"task.done_sizes", info.compress.files, SizeText(info.compress.bytesIn), SizeText(info.compress.bytesOut));
                if (info.compress.storedFiles) status += lycan::Tf(L"task.already_compressed_file_s_stored", info.compress.storedFiles);
                if (info.compress.thumbsDbSkipped) status += lycan::Tf(L"task.thumbs_db_skipped", info.compress.thumbsDbSkipped);
                if (!info.compress.warnings.empty()) status += std::wstring{lycan::Tr(L"fmt.gap")} + L"⚠ " + info.compress.warnings[0].Text();
            }
            else if (info.kind == TaskKind::Scan) status = lycan::Tr(L"task.done_plain") + lycan_app::ScanSummary(info.scan, SizeText(info.scan.bytes));
            else if (info.kind == TaskKind::Repack)
            {
                const auto& r = info.repack;
                status = lycan::Tf(L"task.done_sizes", r.files, SizeText(r.archiveBefore), SizeText(r.archiveAfter));
                if (r.tested) status += lycan::Tr(L"task.tested");
                if (!r.skipped.empty()) status += lycan::Tf(L"task.left_out", r.skipped.size());
                if (!r.warnings.empty()) status += std::wstring{lycan::Tr(L"fmt.gap")} + L"⚠ " + r.warnings[0].Text();
            }
            else if (info.kind == TaskKind::Update)
            {
                const auto& u = info.update;
                status = lycan::Tr(L"main.done");
                auto part = [&](uint32_t n, const wchar_t* text) { if (n) status += std::wstring{lycan::Tr(status.size() > 2 ? L"fmt.comma" : L"fmt.gap")} + lycan::Tf(text, n); };
                part(u.added + u.replaced, L"main.status_added");
                part(u.deleted, L"main.deleted");
                part(u.renamed, L"main.renamed");
                part(u.dirsCreated, L"main.new_folder_s");
                part(u.unchanged, L"main.unchanged");
                part(u.notAdded, L"task.new_files_not_added");
                if (u.storedFiles) status += lycan::Tf(L"task.already_compressed_file_s_stored", u.storedFiles);
                status += std::wstring{lycan::Tr(L"fmt.gap")} + SizeText(u.archiveBefore) + L" → " + SizeText(u.archiveAfter);
                if (!u.warnings.empty()) status += std::wstring{lycan::Tr(L"fmt.gap")} + L"⚠ " + u.warnings[0].Text();
            }
            else
            {
                status = lycan::Tr(L"task.integrity_test_passed");
                if (!info.extract.warnings.empty()) status += lycan::Tf(L"main.data_after_end_compressed_stream", info.extract.warnings.size());
            }
            break;
        case TaskState::Failed:
            status = lycan::Tf(L"task.failed", info.error.Text());
            if (info.kind == TaskKind::Extract && info.extract.wrongPassword) status = lycan::Tr(L"task.failed_wrong_password_start_again");
            else if (info.kind == TaskKind::Extract && !info.extract.errors.empty()) status += lycan::Tf(L"fmt.paren", info.extract.errors[0].Text());
            if (info.kind == TaskKind::Extract && !info.extract.wrongPassword && !info.extract.kept.empty())
            {
                // the files that came out with an error and were kept: say which, so that a damaged file is not taken for a good one
                std::wstring names;
                for (size_t i = 0; i < info.extract.kept.size() && i < 3; ++i)
                {
                    std::wstring k = info.extract.kept[i].Text();
                    if (const auto colon = k.find(L" : "); colon != std::wstring::npos) k.resize(colon);
                    names += (names.empty() ? std::wstring{} : std::wstring{lycan::Tr(L"fmt.list_sep")}) + k;
                }
                if (info.extract.kept.size() > 3) names += L"…";
                status += std::wstring{lycan::Tr(L"fmt.gap")} + lycan::Tf(L"task.extracted_kept", info.extract.kept.size(), names);
            }
            if (info.kind == TaskKind::Compress && !info.compress.errors.empty()) status += lycan::Tf(L"fmt.paren", info.compress.errors[0].Text());
            if (info.kind == TaskKind::Scan)
                status = info.scan.amsiHr ? lycan_app::ScanProblemText(info.scan.amsiHr) : lycan::Tr(L"task.failed_plain") + lycan_app::ScanSummary(info.scan, SizeText(info.scan.bytes));
            if (info.kind == TaskKind::Update && !info.update.errors.empty()) status += lycan::Tf(L"fmt.paren", info.update.errors[0].Text());
            if (info.kind == TaskKind::Repack && !info.repack.errors.empty()) status += lycan::Tf(L"fmt.paren", info.repack.errors[0].Text());
            break;
        case TaskState::Cancelled: status = lycan::Tr(L"task.cancelled"); break;
        }
        status_ = hstring{status};

        bool finished = info.Finished();
        progress_ = info.total ? 100.0 * static_cast<double>(info.done) / static_cast<double>(info.total) : 0.0;
        indeterminate_ = !finished && info.total == 0;
        showProgress_ = !finished;
        pause_ = info.state == TaskState::Running;
        resume_ = info.state == TaskState::Paused;
        cancel_ = !finished;
        finishedOk_ = info.state == TaskState::Completed;
        if (finishedOk_)
        {
            if (info.kind == TaskKind::Extract) outputPath_ = hstring{info.extract.outputDir.wstring()};
            else if (info.kind == TaskKind::Compress && !info.compress.outputs.empty())
                outputPath_ = hstring{info.compress.outputs.front().parent_path().wstring()};
        }

        if (finished && info.kind != TaskKind::Test)
        {
            base_ = info;
            haveBase_ = true;
        }

        // Moon phase: done = full moon; failed = light red; cancelled = grey; otherwise by progress
        const double p = finishedOk_ ? 1.0 : std::clamp(progress_ / 100.0, 0.0, 1.0);
        lit_ = MakeMoonGeometry(p);
        error_ = info.state == TaskState::Failed;
        litBrush_ = SharedBrush(error_ ? 1 : info.state == TaskState::Cancelled ? 2 : info.state == TaskState::Paused ? 5 : 0);
        if (info.state == TaskState::Completed) { glyph_ = L"\uE73E"; glyphBrush_ = SharedBrush(3); }      // check mark (dark, on top of the full moon)
        else if (error_) { glyph_ = L"\uE783"; glyphBrush_ = SharedBrush(4); }                              // error badge
        else if (info.state == TaskState::Cancelled) { glyph_ = L"\uE711"; glyphBrush_ = SharedBrush(4); }  // ✕
        else if (info.state == TaskState::Paused) { glyph_ = L"\uE769"; glyphBrush_ = SharedBrush(4); }     // pause
        else { glyph_ = L""; glyphBrush_ = SharedBrush(4); }

        for (auto n : {L"LitGeometry", L"LitBrush", L"StateGlyph", L"StateGlyphBrush", L"StateGlyphVisibility", L"RingErrorVisibility",
                       L"SpinnerVisibility", L"Title", L"StatusText", L"Progress", L"IsIndeterminate", L"PauseVisibility", L"ResumeVisibility",
                       L"CancelVisibility", L"OpenFolderVisibility", L"ProgressVisibility", L"OutputPath", L"TestVisibility", L"DeleteVisibility"})
            Raise(n);
    }
}
