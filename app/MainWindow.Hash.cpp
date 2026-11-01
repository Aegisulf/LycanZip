#include "pch.h"
// The main window: hash values, checksum files, "Calculate and save as name.sha256", the archive information.
#include "MainWindowShared.h"

namespace winrt::LycanZip::implementation
{
    // ---------------------------------------------------------------- Hash values

    // Which algorithm's checksum file "Save as a checksum file" writes (the strongest of the chosen ones) and its extension
    static uint32_t HashSumAlgo(uint32_t algos)
    {
        for (uint32_t a : {lycan::kHashSha256, lycan::kHashSha512, lycan::kHashSha384, lycan::kHashSha1, lycan::kHashMd5, lycan::kHashCrc32})
            if (algos & a) return a;
        return lycan::kHashSha256;
    }

    static std::wstring HashSumExt(uint32_t algo)
    {
        switch (algo)
        {
        case lycan::kHashSha256: return L"sha256";
        case lycan::kHashSha512: return L"sha512";
        case lycan::kHashSha384: return L"sha384";
        case lycan::kHashSha1: return L"sha1";
        case lycan::kHashMd5: return L"md5";
        default: return L"txt";  // CRC-32: "CRC32 (name) = value" lines are not a format of their own
        }
    }

    // Opens the VirusTotal page of a SHA-256 value in the browser of the user (nothing is uploaded: the address holds the hash and that is all).
    // Tests: LYCAN_TEST_URL_FILE=<file> appends the address to the file instead of opening it.
    static void OpenVirusTotal(std::string const& sha256Hex)
    {
        const std::wstring url = lycan::VirusTotalUrl(sha256Hex);
        if (url.empty()) return;
        if (const wchar_t* f = _wgetenv(L"LYCAN_TEST_URL_FILE"))
        {
            std::ofstream(f, std::ios::app) << lycan::ToUtf8(url) << "\n";
            return;
        }
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // The dialog of one request: the algorithms, the progress, a value per file and algorithm (selectable, with a copy button),
    // a box to compare with a pasted value, and "save as a checksum file" / "copy all".
    struct MainWindow::HashDialogState
    {
        HashRequest req;
        uint32_t algos = lycan::kHashDefault;
        uint64_t id = 0;
        bool running = false, have = false;
        bool vtDone = false;               // "Look up on VirusTotal": the page was opened (only once)
        lycan::HashReport report;
        ContentDialog dlg{nullptr};
        TextBox compare{nullptr};
        TextBlock compareResult{nullptr}, status{nullptr};
        ProgressBar bar{nullptr};
        StackPanel results{nullptr};
        ScrollViewer scroll{nullptr};
        std::vector<std::pair<uint32_t, CheckBox>> boxes;
        // a checksum file next to the file: what it says about this file
        int sideKind = 0;                  // 0 = there is none, 1 = it has a value for the file, 2 = it has none for this file, 3 = it could not be read
        uint32_t sideAlgo = 0, sideForced = 0;
        std::string sideHex;
        TextBlock sideResult{nullptr};
        std::function<void()> start, render, evaluate, checkSide;
    };

    fire_and_forget MainWindow::ShowHashDialogAsync(HashRequest req, bool closeWhenDone)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        const bool inArchive = !req.archive.empty();
        if (inArchive ? req.indices.empty() : req.files.empty()) co_return;
        if (inArchive && current_ && current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;

        using Microsoft::UI::Xaml::Media::SolidColorBrush;
        const Windows::UI::Color green{lycan_app::StatusColor(Windows::UI::Color{255, 0x6C, 0xCB, 0x5F})}, red{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})};
        auto st = std::make_shared<HashDialogState>();
        st->req = std::move(req);
        st->algos = st->req.virusTotal ? lycan::kHashSha256 : lycan::NormalizeHashAlgos(settings_.hashAlgos);   // (a lookup does not change what the user chose for the hash dialog)
        if (!st->req.sidecar.empty() && st->req.files.size() == 1)
        {
            const lycan::SumFile sf = lycan::ReadSumFile(st->req.sidecar);
            if (sf.error != lycan::SumFileError::None) st->sideKind = 3;
            else
            {
                st->sideKind = 2;
                const std::wstring want = fs::path(st->req.files[0]).filename().wstring();
                auto lastPart = [](std::wstring n) {
                    const size_t cut = n.find_last_of(L"/\\");
                    return cut == std::wstring::npos ? n : n.substr(cut + 1);
                };
                const lycan::SumEntry* hit = nullptr;
                for (auto const& e : sf.entries)
                    if (_wcsicmp(lastPart(e.name).c_str(), want.c_str()) == 0) { hit = &e; break; }
                if (!hit && sf.entries.size() == 1) hit = &sf.entries[0];   // a checksum file of one file, named after something else
                if (hit)
                {
                    st->sideKind = 1;
                    st->sideAlgo = hit->algo;
                    st->sideHex = hit->hex;
                    st->sideForced = hit->algo & ~st->algos;
                    st->algos |= hit->algo;   // the value has to be calculated to be compared
                }
            }
        }
        auto dlg = MakeDialog(lycan::Tr(st->req.virusTotal ? L"main.virustotal_lookup" : L"main.hash_values"), L"");
        st->dlg = dlg;

        StackPanel sp;
        sp.Spacing(10);
        sp.MinWidth(680);
        dlg.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(760.0));  // the default (548) cuts the values off
        {
            TextBlock what;
            what.Text(inArchive ? hstring{lycan::Tf(L"main.item_s_archive", st->req.archive.filename().wstring(), st->req.indices.size())}
                      : !st->req.root.empty() ? hstring{lycan::Tf(L"sums.folder_files", st->req.root.filename().wstring(), st->req.files.size())}
                      : st->req.files.size() == 1 ? hstring{st->req.files[0].wstring()}
                                                  : hstring{lycan::Tf(L"fmt.n_files", st->req.files.size())});
            what.TextTrimming(TextTrimming::CharacterEllipsis);
            what.Opacity(0.75);
            sp.Children().Append(what);
        }

        // the algorithms
        {
            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.Spacing(2);
            for (uint32_t a : lycan::HashAlgoList(lycan::kHashAll))
            {
                CheckBox b;
                b.Content(box_value(hstring{lycan::HashAlgoName(a)}));
                b.MinWidth(0);
                b.IsChecked((st->algos & a) != 0);
                Automation::AutomationProperties::SetName(b, hstring{lycan::HashAlgoName(a)});
                b.Click([this, st](auto&&, auto&&) {
                    uint32_t mask = 0;
                    for (auto const& [algo, box] : st->boxes) if (box.IsChecked().Value()) mask |= algo;
                    if (mask == 0)  // at least one: keep the first one that was just switched off
                    {
                        mask = st->algos;
                        for (auto const& [algo, box] : st->boxes) box.IsChecked((mask & algo) != 0);
                    }
                    if (mask != st->algos) { st->algos = mask; st->start(); }
                });
                row.Children().Append(b);
                st->boxes.emplace_back(a, b);
            }
            sp.Children().Append(row);
        }

        // progress and the values
        st->bar = ProgressBar{};
        st->bar.Minimum(0);
        st->bar.Maximum(100);
        sp.Children().Append(st->bar);
        st->status = Caption(L"", 0.75);
        sp.Children().Append(st->status);
        st->sideResult = TextBlock{};
        st->sideResult.TextWrapping(TextWrapping::Wrap);
        st->sideResult.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        sp.Children().Append(st->sideResult);
        st->results = StackPanel{};
        st->results.Spacing(12);
        ScrollViewer sv;
        sv.Content(st->results);
        st->scroll = sv;
        sp.Children().Append(MakeCardBox(sv));

        // compare with a value that was pasted
        st->compare = TextBox{};
        st->compare.Header(box_value(lycan::Tr(L"main.paste_hash_value_compare")));
        st->compare.PlaceholderText(lycan::Tr(L"main.example_sha_256_written_on_website"));
        Automation::AutomationProperties::SetName(st->compare, lycan::Tr(L"main.hash_value_compare"));
        st->compare.TextChanged([st](auto&&, auto&&) { if (st->evaluate) st->evaluate(); });
        sp.Children().Append(st->compare);
        st->compareResult = TextBlock{};
        st->compareResult.TextWrapping(TextWrapping::Wrap);
        st->compareResult.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        sp.Children().Append(st->compareResult);
        sp.Children().Append(Caption(lycan::Tr(L"main.match_only_means_file_same_as"), 0.6));
        dlg.Content(sp);

        // the name of a file as it is shown: the filename; a file in an archive keeps its path; the files of a folder that was chosen are shown from that folder
        auto shownName = [st](std::wstring const& path) -> std::wstring {
            if (!st->req.archive.empty()) return path;
            if (!st->req.root.empty())
            {
                std::wstring rel = fs::path(path).lexically_relative(st->req.root.parent_path()).wstring();
                if (!rel.empty() && rel.rfind(L"..", 0) != 0) { std::replace(rel.begin(), rel.end(), L'\\', L'/'); return rel; }
            }
            return fs::path(path).filename().wstring();
        };

        // what the values look like
        st->render = [this, st, red, shownName]() {
            st->results.Children().Clear();
            // a few files show as they are; many scroll (with room at the right for the scroll bar)
            const bool many = st->report.files.size() > 3;
            st->scroll.VerticalScrollBarVisibility(many ? ScrollBarVisibility::Auto : ScrollBarVisibility::Disabled);
            st->scroll.MaxHeight(many ? 240.0 : std::numeric_limits<double>::infinity());
            st->results.Margin(ThicknessHelper::FromLengths(0, 0, many ? 18 : 0, 0));
            const auto list = lycan::HashAlgoList(st->report.algos);
            size_t shown = 0;
            for (auto const& f : st->report.files)
            {
                if (shown++ >= 200) break;
                StackPanel one;
                one.Spacing(3);
                TextBlock name;
                name.Text(hstring{shownName(f.path)});
                name.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
                name.TextTrimming(TextTrimming::CharacterEllipsis);
                Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(name, box_value(hstring{f.path}));
                one.Children().Append(name);
                if (!f.ok())
                {
                    TextBlock e;
                    e.Text(hstring{f.error.Text()});
                    e.TextWrapping(TextWrapping::Wrap);
                    e.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{red});
                    one.Children().Append(e);
                }
                else
                {
                    one.Children().Append(Caption(f.size < 1024 ? FormatSize(f.size) : lycan::Tf(L"fmt.size_bytes", FormatSize(f.size), f.size), 0.6));
                    for (size_t i = 0; i < list.size() && i < f.hex.size(); ++i)
                    {
                        Grid row;
                        row.ColumnSpacing(8);
                        ColumnDefinition c0, c1, c2;
                        c0.Width(GridLength{76, GridUnitType::Pixel});
                        c1.Width(GridLength{1, GridUnitType::Star});
                        c2.Width(GridLength{0, GridUnitType::Auto});
                        row.ColumnDefinitions().Append(c0);
                        row.ColumnDefinitions().Append(c1);
                        row.ColumnDefinitions().Append(c2);
                        TextBlock lbl;
                        lbl.Text(hstring{lycan::HashAlgoName(list[i])});
                        lbl.Opacity(0.7);
                        lbl.VerticalAlignment(VerticalAlignment::Center);
                        row.Children().Append(lbl);
                        TextBlock val;
                        val.Text(hstring{FromUtf8(f.hex[i])});
                        val.IsTextSelectionEnabled(true);
                        val.TextWrapping(TextWrapping::Wrap);
                        val.FontFamily(Microsoft::UI::Xaml::Media::FontFamily{L"Consolas"});
                        val.VerticalAlignment(VerticalAlignment::Center);
                        Grid::SetColumn(val, 1);
                        row.Children().Append(val);
                        Button copy;
                        copy.Content(MakeGlyph(L"", 12));
                        copy.Padding(ThicknessHelper::FromUniformLength(5));
                        copy.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
                        copy.BorderThickness(ThicknessHelper::FromUniformLength(0));
                        Automation::AutomationProperties::SetName(copy, lycan::Tr(L"preview.copy"));
                        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(copy, box_value(lycan::Tr(L"preview.copy")));
                        copy.Click([hex = f.hex[i]](auto&&, auto&&) {
                            Windows::ApplicationModel::DataTransfer::DataPackage dp;
                            dp.SetText(hstring{FromUtf8(hex)});
                            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
                        });
                        Grid::SetColumn(copy, 2);
                        row.Children().Append(copy);
                        if (list[i] == lycan::kHashSha256)   // look the file up at VirusTotal
                        {
                            ColumnDefinition c3;
                            c3.Width(GridLength{0, GridUnitType::Auto});
                            row.ColumnDefinitions().Append(c3);
                            Button vt;
                            vt.Content(MakeGlyph(L"\uE774", 12));
                            vt.Padding(ThicknessHelper::FromUniformLength(5));
                            vt.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
                            vt.BorderThickness(ThicknessHelper::FromUniformLength(0));
                            Automation::AutomationProperties::SetName(vt, lycan::Tr(L"main.virustotal_lookup"));
                            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(vt, box_value(lycan::Tr(L"main.virustotal_lookup_tip")));
                            vt.Click([hex = f.hex[i]](auto&&, auto&&) { OpenVirusTotal(hex); });
                            Grid::SetColumn(vt, 3);
                            row.Children().Append(vt);
                        }
                        one.Children().Append(row);
                    }
                }
                st->results.Children().Append(one);
            }
            if (st->report.files.size() > 200)
                st->results.Children().Append(Caption(lycan::Tf(L"main.more_file_s_not_listed_save", st->report.files.size() - 200), 0.65));
        };

        // the pasted value against what was computed
        st->evaluate = [st, green, red, shownName]() {
            auto res = st->compareResult;
            res.Text(L"");
            const std::wstring text{st->compare.Text()};
            if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return;
            auto paint = [&](hstring const& t, Windows::UI::Color c) { res.Text(t); res.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{c}); };
            const std::string want = lycan::NormalizeHashText(text);
            if (want.empty()) { paint(lycan::Tr(L"main.not_hash_value_only_0_9"), red); return; }
            const uint32_t mask = lycan::HashAlgosForLength(want.size());
            if (!mask)
            {
                paint(hstring{lycan::Tf(L"main.length_digits_matches_no_algorithm", want.size())}, red);
                return;
            }
            std::wstring which;
            for (uint32_t a : lycan::HashAlgoList(mask)) which += (which.empty() ? L"" : L" / ") + std::wstring(lycan::HashAlgoName(a));
            if (!st->have) { paint(lycan::Tr(L"main.still_calculating"), lycan_app::StatusColor(Windows::UI::Color{255, 200, 200, 200})); return; }
            const auto list = lycan::HashAlgoList(st->report.algos);
            for (auto const& f : st->report.files)
            {
                if (!f.ok()) continue;
                for (size_t i = 0; i < list.size() && i < f.hex.size(); ++i)
                    if ((list[i] & mask) && f.hex[i] == want)
                    {
                        paint(hstring{lycan::Tf(L"main.match", shownName(f.path), lycan::HashAlgoName(list[i]))}, green);
                        return;
                    }
            }
            if ((mask & st->report.algos) == 0) paint(hstring{lycan::Tf(L"main.has_not_been_calculated_tick_above", which)}, lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90}));
            else paint(hstring{lycan::Tf(L"main.no_match_no_file_has", which)}, red);
        };

        // the value of the checksum file next to the file against what was calculated
        st->checkSide = [st, green, red]() {
            if (st->sideKind == 0) return;
            const std::wstring name = st->req.sidecar.filename().wstring();
            auto paint = [&](hstring const& t, Windows::UI::Color c) { st->sideResult.Text(t); st->sideResult.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{c}); };
            const Windows::UI::Color amber{lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90})};
            if (st->sideKind == 3) { paint(hstring{lycan::Tf(L"sums.side_unusable", name)}, red); return; }
            if (st->sideKind == 2) { paint(hstring{lycan::Tf(L"sums.side_no_entry", name)}, amber); return; }
            if (!st->have) { paint(hstring{lycan::Tf(L"sums.side_waiting", name)}, lycan_app::StatusColor(Windows::UI::Color{255, 200, 200, 200})); return; }
            if ((st->report.algos & st->sideAlgo) == 0) { paint(hstring{lycan::Tf(L"sums.side_untick", name, lycan::HashAlgoName(st->sideAlgo))}, amber); return; }
            if (st->report.files.empty() || !st->report.files[0].ok()) { st->sideResult.Text(L""); return; }
            const auto list = lycan::HashAlgoList(st->report.algos);
            for (size_t i = 0; i < list.size() && i < st->report.files[0].hex.size(); ++i)
                if (list[i] == st->sideAlgo)
                {
                    if (st->report.files[0].hex[i] == st->sideHex) paint(hstring{lycan::Tf(L"sums.side_match", name)}, green);
                    else paint(hstring{lycan::Tf(L"sums.side_differ", name, FromUtf8(st->sideHex))}, red);
                    return;
                }
        };

        // (re)start the computation
        st->start = [this, st]() {
            if (st->id) { queue_.Cancel(st->id); hashHandlers_.erase(st->id); }
            st->report = {};
            st->have = false;
            st->results.Children().Clear();
            if (!st->req.virusTotal)
            {
                settings_.hashAlgos = st->algos & ~st->sideForced;   // what the checksum file asked for is not the user's choice
                SaveSettings();
            }
            const uint32_t sumAlgo = HashSumAlgo(st->algos);
            st->dlg.PrimaryButtonText(hstring{lycan::Tf(L"main.save_as", HashSumExt(sumAlgo))});
            st->dlg.IsPrimaryButtonEnabled(false);
            st->dlg.IsSecondaryButtonEnabled(false);
            st->running = true;
            st->bar.Value(0);
            st->bar.Visibility(Visibility::Visible);
            st->status.Text(lycan::Tr(L"main.calculating"));
            if (st->checkSide) st->checkSide();
            const bool inArchive = !st->req.archive.empty();
            st->id = inArchive ? queue_.EnqueueHashEntries(st->req.archive, st->req.indices, st->algos, MakeOpenOptions())
                               : queue_.EnqueueHashFiles(st->req.files, st->algos);
            hashHandlers_[st->id] = [this, st](lycan::TaskInfo const& info) {
                if (info.id != st->id) return;
                if (!info.Finished())
                {
                    const double pct = info.total ? 100.0 * static_cast<double>(info.done) / static_cast<double>(info.total) : 0.0;
                    st->bar.Value(pct);
                    st->status.Text(hstring{lycan::Tf(L"main.calculating_progress", std::lround(pct), FormatSize(info.done), FormatSize(info.total))});
                    return;
                }
                st->running = false;
                st->bar.Visibility(Visibility::Collapsed);
                if (info.state == lycan::TaskState::Cancelled) return;
                if (info.hash.files.empty())  // nothing came out: the archive could not be opened, or nothing to hash
                {
                    st->status.Text(hstring{info.error.empty() ? std::wstring(lycan::Tr(L"main.there_nothing_calculate")) : info.error.Text()});
                    return;
                }
                st->report = info.hash;
                st->have = true;
                st->render();
                std::wstring s = lycan::Tf(L"main.done_file_s", st->report.files.size() - st->report.failed, FormatSize(st->report.bytes));
                if (st->report.failed) s += lycan::Tf(L"main.could_not_be_calculated", st->report.failed);
                st->status.Text(hstring{s});
                st->dlg.IsPrimaryButtonEnabled(st->report.failed < st->report.files.size());
                st->dlg.IsSecondaryButtonEnabled(st->report.failed < st->report.files.size());
                st->evaluate();
                if (st->checkSide) st->checkSide();
                if (st->req.virusTotal && !st->vtDone && st->report.failed == 0 && st->report.files.size() == 1)
                {
                    const auto list = lycan::HashAlgoList(st->report.algos);
                    for (size_t i = 0; i < list.size() && i < st->report.files[0].hex.size(); ++i)
                        if (list[i] == lycan::kHashSha256)
                        {
                            st->vtDone = true;
                            OpenVirusTotal(st->report.files[0].hex[i]);
                            st->dlg.Hide();   // done: the browser has the page (the hash stays in the dialog when something went wrong instead)
                        }
                }
            };
        };

        dlg.SecondaryButtonText(lycan::Tr(L"main.copy_all"));
        dlg.CloseButtonText(lycan::Tr(L"main.close"));
        dlg.DefaultButton(ContentDialogButton::Close);
        dlg.PrimaryButtonClick([this, st](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
            a.Cancel(true);  // the dialog stays open
            SaveHashSumsAsync(st);
        });
        dlg.SecondaryButtonClick([st](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
            a.Cancel(true);
            if (!st->have) return;
            const auto list = lycan::HashAlgoList(st->report.algos);
            const bool disk = st->req.archive.empty();
            const fs::path base = !st->req.root.empty() ? st->req.root.parent_path() : disk && !st->req.files.empty() ? st->req.files[0].parent_path() : fs::path{};
            const std::string text = list.size() == 1 ? lycan::HashReportText(st->report, list[0], base) : lycan::HashReportTextAll(st->report, base);
            Windows::ApplicationModel::DataTransfer::DataPackage dp;
            dp.SetText(hstring{FromUtf8(text)});
            Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
            st->status.Text(lycan::Tr(L"main.copied_to_clipboard_period"));
        });
        st->start();
        dialogWidthDip_ = 820;   // (the compact window's width while the dialog is open)
        co_await ShowDialogAsync(dlg);
        dialogWidthDip_ = 640;

        if (st->id)
        {
            if (st->running) queue_.Cancel(st->id);
            hashHandlers_.erase(st->id);
        }
        st->start = nullptr;  // they hold st: let go of each other
        st->render = nullptr;
        st->evaluate = nullptr;
        st->checkSide = nullptr;
        st->dlg = nullptr;
        if (closeWhenDone && !closing_ && !current_ && view_ == View::Home) Close();  // the window was only the host of this dialog
    }

    // "Save as a checksum file": the file of the strongest chosen algorithm, names relative to the folder it is saved in
    fire_and_forget MainWindow::SaveHashSumsAsync(std::shared_ptr<HashDialogState> st)
    {
        auto strong = get_strong();
        if (st->running || !st->have || st->report.files.empty()) co_return;
        const uint32_t algo = HashSumAlgo(st->report.algos);
        const std::wstring ext = HashSumExt(algo);
        const bool inArchive = !st->req.archive.empty();
        const fs::path first = inArchive ? st->req.archive : !st->req.root.empty() ? st->req.root : st->req.files[0];
        std::wstring stem = inArchive || st->req.files.size() == 1 || !st->req.root.empty() ? first.filename().wstring() : first.parent_path().filename().wstring();
        if (stem.empty()) stem = L"checksums";
        fs::path out;
        if (const wchar_t* t = _wgetenv(L"LYCAN_TEST_HASH_SAVE")) out = t;  // tests: no picker
        else out = fs::path{std::wstring(co_await lycan_app::PickSaveFileAsync(AppWindow().Id(), L"LycanZip.HashSums", first.parent_path(), stem + L"." + ext, {{ext + L" file", ext}},
                                                                               lycan::Tr(L"main.save_checksum_file")))};
        if (out.empty()) co_return;
        const std::string text = lycan::HashReportText(st->report, algo, inArchive ? fs::path{} : out.parent_path());
        std::ofstream f(out, std::ios::binary | std::ios::trunc);
        f << text;
        f.close();
        if (!f.good() || text.empty()) { ShowMessage(lycan::Tr(L"main.could_not_save"), hstring{lycan::Tf(L"main.could_not_write", out.wstring())}); co_return; }
        st->status.Text(hstring{lycan::Tf(L"main.saved", out.wstring())});
    }

    void MainWindow::HashNodes(std::vector<uint32_t> const& nodes)
    {
        if (!current_ || !tree_ || nodes.empty()) return;
        HashRequest req;
        req.archive = currentPath_;
        req.indices = tree_->EntryIndices(nodes);  // folders bring their files
        ShowHashDialogAsync(std::move(req));
    }

    // "Look up on VirusTotal" for one file of the archive: its SHA-256 is calculated from the archive (nothing is unpacked to disk), then the page opens
    void MainWindow::VirusTotalNode(uint32_t node)
    {
        if (!current_ || !tree_ || tree_->Node(node).isDir || tree_->Node(node).entryIndex == lycan::kNoEntry) return;
        HashRequest req;
        req.archive = currentPath_;
        req.indices = {tree_->Node(node).entryIndex};
        req.virusTotal = true;
        ShowHashDialogAsync(std::move(req));
    }

    // ... and for the archive file itself
    void MainWindow::OnVirusTotalArchiveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!current_) return;
        HashRequest req;
        req.files = {currentPath_};
        req.virusTotal = true;
        ShowHashDialogAsync(std::move(req));
    }

    void MainWindow::OnHashArchiveClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!current_) return;
        HashRequest req;
        req.files = {currentPath_};
        req.sidecar = lycan::FindArchiveSidecars(currentPath_).archiveSum;
        ShowHashDialogAsync(std::move(req));
    }

    // ---------------------------------------------------------------- Verify checksum files

    // What the checks of a checksum file are shown in; the checks run on a thread of their own (they only read files)
    struct MainWindow::VerifyState
    {
        std::vector<fs::path> sums;
        fs::path archive;                     // empty = the files next to each checksum file; otherwise the files inside this archive
        lycan::ArchiveSumOptions match;       // archive: what the user said about the names, when they could not be matched
        std::atomic<bool> cancel{false}, closed{false};
        std::atomic<ULONGLONG> lastTick{0};
        uint64_t runId = 0;                   // UI thread only: an older run that ends late is ignored
        bool running = false;
        ContentDialog dlg{nullptr};
        ProgressBar bar{nullptr};
        TextBlock status{nullptr};
        StackPanel results{nullptr};
        StackPanel ask{nullptr};
        TextBox strip{nullptr}, add{nullptr};
    };

    static hstring SumFileErrorText(lycan::SumFileError e)
    {
        switch (e)
        {
        case lycan::SumFileError::Unreadable: return lycan::Tr(L"sums.error_unreadable");
        case lycan::SumFileError::TooBig: return lycan::Tr(L"sums.error_too_big");
        case lycan::SumFileError::NoLines: return lycan::Tr(L"sums.error_no_lines");
        default: return {};
        }
    }

    fire_and_forget MainWindow::RunVerifyAsync(std::shared_ptr<VerifyState> st)
    {
        auto strong = get_strong();
        const uint64_t run = ++st->runId;
        st->running = true;
        st->cancel = false;
        st->results.Children().Clear();
        st->ask.Visibility(Visibility::Collapsed);
        st->bar.Value(0);
        st->bar.Visibility(Visibility::Visible);
        st->status.Text(lycan::Tr(L"sums.checking"));
        const std::vector<fs::path> sums = st->sums;
        const fs::path archive = st->archive;
        const lycan::ArchiveSumOptions match = st->match;
        const lycan::OpenOptions oo = archive.empty() ? lycan::OpenOptions{} : MakeOpenOptions();
        const auto dispatcher = dispatcher_;

        std::vector<lycan::SumFile> files;
        std::vector<lycan::SumVerifyReport> reports;
        std::wstring failure;
        bool cancelled = false;
        co_await resume_background();
        try
        {
            std::unique_ptr<lycan::ArchiveSource> src;
            if (!archive.empty()) src = lycan::ArchiveSource::Open(archive, oo);
            for (auto const& file : sums)
            {
                if (st->cancel) { cancelled = true; break; }
                lycan::SumVerifyOptions vo;
                vo.password = oo.password;
                vo.progress = [st, dispatcher, run](uint64_t done, uint64_t total) {
                    if (st->cancel) return false;
                    const ULONGLONG now = GetTickCount64();
                    if (now - st->lastTick.load() > 120)
                    {
                        st->lastTick = now;
                        dispatcher.TryEnqueue([st, run, done, total] {
                            if (st->closed || run != st->runId) return;
                            const double pct = total ? 100.0 * static_cast<double>(done) / static_cast<double>(total) : 0.0;
                            st->bar.Value(pct);
                            st->status.Text(hstring{lycan::Tf(L"sums.checking_progress", std::lround(pct), FormatSize(done), FormatSize(total))});
                        });
                    }
                    return true;
                };
                files.push_back(lycan::ReadSumFile(file));
                reports.push_back(src ? lycan::VerifySumAgainstArchive(files.back(), *src, vo, match) : lycan::VerifySumFile(files.back(), vo));
            }
        }
        catch (lycan::Error const& e)
        {
            if (e.cancelled()) cancelled = true;
            else failure = e.message().Text();
        }
        catch (std::exception const& e) { failure = FromUtf8(e.what()); }
        co_await resume_ui{dispatcher_};
        if (st->closed || run != st->runId) co_return;

        st->running = false;
        st->bar.Visibility(Visibility::Collapsed);
        if (cancelled) { st->status.Text(L""); co_return; }
        if (!failure.empty()) { st->status.Text(hstring{failure}); co_return; }

        using Microsoft::UI::Xaml::Media::SolidColorBrush;
        const Windows::UI::Color green{lycan_app::StatusColor(Windows::UI::Color{255, 0x6C, 0xCB, 0x5F})}, red{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})},
            amber{lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90})};
        auto colored = [](hstring const& text, Windows::UI::Color c, bool bold) {
            TextBlock t;
            t.Text(text);
            t.TextWrapping(TextWrapping::Wrap);
            t.Foreground(SolidColorBrush{c});
            if (bold) t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            return t;
        };
        size_t fine = 0;
        bool needAsk = false;
        for (size_t i = 0; i < reports.size(); ++i)
        {
            const lycan::SumVerifyReport& r = reports[i];
            StackPanel one;
            one.Spacing(4);
            TextBlock title;
            title.Text(hstring{sums[i].filename().wstring()});
            title.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            title.TextTrimming(TextTrimming::CharacterEllipsis);
            Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(title, box_value(hstring{sums[i].wstring()}));
            one.Children().Append(title);
            if (r.error != lycan::SumFileError::None)
            {
                one.Children().Append(colored(SumFileErrorText(r.error), red, false));
                st->results.Children().Append(one);
                continue;
            }
            if (r.allMatch())
            {
                ++fine;
                one.Children().Append(colored(hstring{lycan::Tf(L"sums.all_match", r.total)}, green, true));
                one.Children().Append(Caption(lycan::Tf(L"sums.bytes_read", FormatSize(r.bytes)), 0.6));
            }
            else
            {
                one.Children().Append(colored(hstring{lycan::Tf(L"sums.some_match", r.matched, r.total)}, red, true));
                size_t shown = 0;
                for (auto const& p : r.problems)
                {
                    if (shown++ >= 300) break;
                    Grid row;
                    row.ColumnSpacing(10);
                    ColumnDefinition c0, c1;
                    c0.Width(GridLength{1, GridUnitType::Star});
                    c1.Width(GridLength{0, GridUnitType::Auto});
                    row.ColumnDefinitions().Append(c0);
                    row.ColumnDefinitions().Append(c1);
                    TextBlock name;
                    name.Text(hstring{p.name});
                    name.TextTrimming(TextTrimming::CharacterEllipsis);
                    Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(name, box_value(hstring{p.name}));
                    row.Children().Append(name);
                    TextBlock what = p.status == lycan::SumStatus::Mismatch ? colored(lycan::Tr(L"sums.differs"), red, false)
                                     : p.status == lycan::SumStatus::Missing ? colored(lycan::Tr(L"sums.not_found"), amber, false)
                                                                             : colored(hstring{lycan::Tf(L"sums.could_not_read", p.error.Text())}, red, false);
                    Grid::SetColumn(what, 1);
                    row.Children().Append(what);
                    one.Children().Append(row);
                }
                if (r.problems.size() > 300) one.Children().Append(Caption(lycan::Tf(L"sums.more_not_listed", r.problems.size() - 300), 0.65));
            }
            if (!r.extra.empty())
            {
                one.Children().Append(colored(hstring{lycan::Tf(L"sums.extra_files", r.extra.size())}, amber, false));
                size_t shown = 0;
                for (auto const& e : r.extra)
                {
                    if (shown++ >= 20) break;
                    one.Children().Append(Caption(e, 0.65));
                }
                if (r.extra.size() > 20) one.Children().Append(Caption(lycan::Tf(L"sums.more_not_listed", r.extra.size() - 20), 0.65));
            }
            if (!archive.empty() && r.confident)
            {
                if (r.sumSkip) one.Children().Append(Caption(lycan::Tf(L"sums.names_skip_checksum", r.sumSkip), 0.6));
                if (r.entrySkip) one.Children().Append(Caption(lycan::Tf(L"sums.names_skip_archive", r.entrySkip), 0.6));
            }
            if (!archive.empty() && !r.confident) needAsk = true;
            if (files[i].badLines) one.Children().Append(Caption(lycan::Tf(L"sums.bad_lines", files[i].badLines), 0.6));
            st->results.Children().Append(one);
        }
        st->status.Text(reports.size() > 1 ? hstring{lycan::Tf(L"sums.summary_many", fine, reports.size())} : hstring{});
        if (needAsk) st->ask.Visibility(Visibility::Visible);
    }

    fire_and_forget MainWindow::ShowVerifyDialogAsync(std::vector<fs::path> sums, fs::path archive, bool closeWhenDone)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        if (sums.empty()) co_return;
        if (!archive.empty() && current_ && current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;

        auto st = std::make_shared<VerifyState>();
        st->sums = std::move(sums);
        st->archive = std::move(archive);
        auto dlg = MakeDialog(lycan::Tr(L"sums.verify_title"), L"");
        st->dlg = dlg;

        StackPanel sp;
        sp.Spacing(10);
        sp.MinWidth(680);
        dlg.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(760.0));
        {
            TextBlock what;
            what.Text(!st->archive.empty() ? hstring{lycan::Tf(L"sums.of_archive", st->archive.filename().wstring())}
                      : st->sums.size() == 1 ? hstring{lycan::Tf(L"sums.of_folder", st->sums[0].parent_path().wstring())}
                                             : hstring{lycan::Tf(L"sums.n_checksum_files", st->sums.size())});
            what.TextTrimming(TextTrimming::CharacterEllipsis);
            what.Opacity(0.75);
            sp.Children().Append(what);
        }
        st->bar = ProgressBar{};
        st->bar.Minimum(0);
        st->bar.Maximum(100);
        sp.Children().Append(st->bar);
        st->status = Caption(L"", 0.75);
        sp.Children().Append(st->status);
        st->results = StackPanel{};
        st->results.Spacing(14);
        ScrollViewer sv;
        sv.Content(st->results);
        sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        sv.MaxHeight(340);
        st->results.Margin(ThicknessHelper::FromLengths(0, 0, 18, 0));
        sp.Children().Append(MakeCardBox(sv));

        // the names of the checksum file are not the names in the archive: the user says how they differ
        st->ask = StackPanel{};
        st->ask.Spacing(8);
        st->ask.Visibility(Visibility::Collapsed);
        {
            TextBlock note;
            note.Text(lycan::Tr(L"sums.names_not_found"));
            note.TextWrapping(TextWrapping::Wrap);
            note.Opacity(0.85);
            st->ask.Children().Append(note);
            st->strip = TextBox{};
            st->strip.Header(box_value(lycan::Tr(L"sums.strip_prefix")));
            st->strip.PlaceholderText(lycan::Tr(L"sums.strip_example"));
            st->ask.Children().Append(st->strip);
            st->add = TextBox{};
            st->add.Header(box_value(lycan::Tr(L"sums.add_prefix")));
            st->add.PlaceholderText(lycan::Tr(L"sums.add_example"));
            st->ask.Children().Append(st->add);
            Button again;
            again.Content(box_value(lycan::Tr(L"sums.check_again")));
            again.Click([this, st](auto&&, auto&&) {
                if (st->running) return;
                st->match.manual = true;
                st->match.stripPrefix = std::wstring{st->strip.Text()};
                st->match.addPrefix = std::wstring{st->add.Text()};
                RunVerifyAsync(st);
            });
            st->ask.Children().Append(again);
        }
        sp.Children().Append(st->ask);
        sp.Children().Append(Caption(lycan::Tr(L"sums.match_only_means"), 0.6));
        dlg.Content(sp);
        dlg.CloseButtonText(lycan::Tr(L"main.close"));
        dlg.DefaultButton(ContentDialogButton::Close);

        RunVerifyAsync(st);
        dialogWidthDip_ = 820;
        co_await ShowDialogAsync(dlg);
        dialogWidthDip_ = 640;

        st->closed = true;
        st->cancel = true;
        st->dlg = nullptr;
        if (closeWhenDone && !closing_ && !current_ && view_ == View::Home) Close();  // the window was only the host of this dialog
    }

    // Explorer starts one process per selected file: the paths of one selection arrive one by one and are checked in one dialog
    void MainWindow::QueueVerify(std::vector<fs::path> paths, bool closeWhenDone)
    {
        for (auto const& p : paths)
            if (std::find(verifyPaths_.begin(), verifyPaths_.end(), p) == verifyPaths_.end()) verifyPaths_.push_back(p);
        verifyClose_ = verifyClose_ || closeWhenDone;
        if (!verifyTimer_)
        {
            verifyTimer_ = dispatcher_.CreateTimer();
            verifyTimer_.IsRepeating(false);
            verifyTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->FlushVerify(); });
        }
        verifyTimer_.Interval(500ms);
        verifyTimer_.Stop();
        verifyTimer_.Start();
    }

    void MainWindow::FlushVerify()
    {
        if (verifyTimer_) verifyTimer_.Stop();
        if (verifyPaths_.empty()) return;
        auto paths = std::move(verifyPaths_);
        verifyPaths_.clear();
        const bool close = verifyClose_;
        verifyClose_ = false;
        std::sort(paths.begin(), paths.end());  // the processes arrive in any order: a steady order for the list
        ShowVerifyDialogAsync(std::move(paths), {}, close);
    }

    // The files and folders of a request (right-click on anything, or the command line): folders bring every file in them. Listing a big
    // folder takes a moment, so it is done off the UI thread.
    fire_and_forget MainWindow::HashRequestAsync(std::vector<fs::path> paths, bool closeWhenDone)
    {
        auto strong = get_strong();
        HashRequest hr;
        bool anyFolder = false;
        co_await resume_background();
        for (auto const& p : paths)
        {
            std::error_code ec;
            if (fs::is_directory(p, ec)) anyFolder = true;
        }
        hr.files = anyFolder ? lycan::ExpandForHash(paths) : paths;
        if (paths.size() == 1 && anyFolder) hr.root = paths[0];
        else if (paths.size() == 1) hr.sidecar = lycan::FindArchiveSidecars(paths[0]).archiveSum;
        co_await resume_ui{dispatcher_};
        if (hr.files.empty())
        {
            ShowMessage(lycan::Tr(L"main.no_files"), lycan::Tr(L"sums.no_files_to_hash"));
            co_return;
        }
        ShowHashDialogAsync(std::move(hr), closeWhenDone);
    }

    // ---------------------------------------------------------------- "Calculate and save as name.sha256"

    // The calculation runs on a thread of its own; the window that asked for it stays out of sight unless it takes long (see HashSaveAsync)
    struct HashSaveState
    {
        std::atomic<bool> cancel{false}, finished{false};
        std::atomic<uint64_t> done{0}, total{0};
        lycan::SumWriteResult result;
        std::wstring error;
        bool cancelled = false;
    };

    static fire_and_forget HashSaveJob(std::shared_ptr<HashSaveState> st, std::vector<fs::path> paths, fs::path out, fs::path base)
    {
        co_await resume_background();
        try
        {
            if (const wchar_t* d = _wgetenv(L"LYCAN_TEST_HASH_DELAY"))   // tests: a slow calculation, to see the progress
                for (int ms = _wtoi(d); ms > 0 && !st->cancel; ms -= 50) Sleep(50);
            if (st->cancel) { st->cancelled = true; st->finished = true; co_return; }
            lycan::SumWriteOptions o;
            o.algo = lycan::kHashSha256;
            o.progress = [st](uint64_t d, uint64_t t) { st->done = d; st->total = t; return !st->cancel.load(); };
            st->result = lycan::WriteSumFile(out, paths, base, o);
        }
        catch (lycan::Error const& e)
        {
            if (e.cancelled()) st->cancelled = true;
            else st->error = e.message().Text();
        }
        catch (std::exception const& e) { st->error = FromUtf8(e.what()); }
        st->finished = true;
    }

    // The SHA-256 of the chosen files and folders into name.sha256: one item = its name (the file is next to it), several = the name of the folder they
    // are in (the file is in that folder). Nothing is shown when it is quick: a notification says it is done. A window comes when it takes long (progress,
    // a way to cancel), when the file is there already (replace it or keep both) and when something went wrong.
    fire_and_forget MainWindow::HashSaveAsync(std::vector<fs::path> paths, bool closeWhenDone)
    {
        auto strong = get_strong();
        OpGuard guard{this};
        if (paths.empty()) co_return;
        auto finishHost = [this, closeWhenDone]() {
            if (closeWhenDone && !closing_ && !current_ && view_ == View::Home) Close();  // the window was only the host of this
        };

        const fs::path dir = paths[0].parent_path();
        std::wstring stem = paths.size() == 1 ? paths[0].filename().wstring() : dir.filename().wstring();
        if (stem.empty() || stem.back() == L':') stem = L"checksums";
        fs::path out = dir / (stem + L".sha256");

        std::error_code ec;
        if (fs::exists(out, ec))
        {
            auto dlg = MakeDialog(lycan::Tr(L"sums.save_exists_title"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"sums.save_exists", out.filename().wstring())});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"sums.save_replace"));
            dlg.SecondaryButtonText(lycan::Tr(L"sums.save_keep_both"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Secondary);   // nothing is lost by default
            const auto r = co_await ShowDialogAsync(dlg);
            if (r == ContentDialogResult::Secondary) out = UniquePath(out);
            else if (r != ContentDialogResult::Primary) { finishHost(); co_return; }
        }

        auto st = std::make_shared<HashSaveState>();
        HashSaveJob(st, paths, out, dir);

        // quick: wait a moment without showing anything
        const auto start = std::chrono::steady_clock::now();
        while (!st->finished && std::chrono::steady_clock::now() - start < 1s)
        {
            co_await resume_after(50ms);
            co_await resume_ui{dispatcher_};
        }

        ContentDialog dlg{nullptr};
        if (!st->finished)
        {
            // it takes long: the window appears, with the progress and a way out
            using Microsoft::UI::Xaml::Media::SolidColorBrush;
            dlg = MakeDialog(lycan::Tr(L"sums.save_title"), L"");
            StackPanel sp;
            sp.Spacing(10);
            sp.MinWidth(420);
            TextBlock name;
            name.Text(hstring{out.filename().wstring()});
            name.TextTrimming(TextTrimming::CharacterEllipsis);
            name.Opacity(0.75);
            sp.Children().Append(name);
            ProgressBar bar;
            bar.Minimum(0);
            bar.Maximum(100);
            sp.Children().Append(bar);
            TextBlock status = Caption(lycan::Tr(L"main.calculating"), 0.75);
            status.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(status);
            dlg.Content(sp);
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.PrimaryButtonText(lycan::Tr(L"main.show_explorer"));
            dlg.IsPrimaryButtonEnabled(false);
            dlg.PrimaryButtonClick([out](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
                a.Cancel(true);  // the dialog stays open
                if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(out.c_str()))
                {
                    SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                    ILFree(pidl);
                }
            });
            const auto q = dispatcher_;
            const Windows::UI::Color green{lycan_app::StatusColor(Windows::UI::Color{255, 0x6C, 0xCB, 0x5F})}, red{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})};
            // follows the calculation until it ends; then shows what came out (the dialog stays until it is closed)
            [](std::shared_ptr<HashSaveState> st, Microsoft::UI::Dispatching::DispatcherQueue q, ContentDialog dlg, ProgressBar bar, TextBlock status, fs::path out,
               Windows::UI::Color green, Windows::UI::Color red) -> fire_and_forget {
                while (!st->finished)
                {
                    co_await resume_after(100ms);
                    co_await resume_ui{q};
                    const uint64_t t = st->total, d = st->done;
                    const double pct = t ? 100.0 * static_cast<double>(d) / static_cast<double>(t) : 0.0;
                    bar.Value(pct);
                    status.Text(hstring{lycan::Tf(L"main.calculating_progress", std::lround(pct), FormatSize(d), FormatSize(t))});
                }
                co_await resume_ui{q};
                bar.Visibility(Visibility::Collapsed);
                if (st->cancelled) { dlg.Hide(); co_return; }
                std::wstring text;
                if (!st->error.empty()) text = st->error;
                else
                {
                    text = lycan::Tf(L"sums.save_done", out.wstring());
                    if (st->result.skipped) text += lycan::Tf(L"sums.save_done_skipped", st->result.skipped);
                    dlg.IsPrimaryButtonEnabled(true);
                }
                status.Text(hstring{text});
                status.Opacity(1);
                status.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{st->error.empty() ? green : red});
                dlg.CloseButtonText(lycan::Tr(L"main.close"));
            }(st, q, dlg, bar, status, out, green, red);
            co_await ShowDialogAsync(dlg);
            if (!st->finished)   // closed while it ran: cancel (the file is not left behind)
            {
                st->cancel = true;
                while (!st->finished) { co_await resume_after(50ms); co_await resume_ui{dispatcher_}; }
            }
            finishHost();
            co_return;
        }

        // quick: it is done
        if (st->cancelled) { finishHost(); co_return; }
        if (!st->error.empty())
        {
            auto err = MakeDialog(lycan::Tr(L"sums.save_failed_title"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.could_not_write", out.wstring()) + L"\n" + st->error});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            err.Content(tb);
            err.CloseButtonText(lycan::Tr(L"main.close"));
            co_await ShowDialogAsync(err);
            finishHost();
            co_return;
        }
        if (settings_.notifications && !closing_)
        {
            notifier_.Notify(lycan::Tr(L"sums.notify_title"),
                             st->result.skipped ? lycan::Tf(L"sums.notify_skipped", out.filename().wstring(), st->result.skipped) : out.filename().wstring(), false);
            // the notification goes away when the program ends: stay until it has been shown
            for (int i = 0; i < 40 && notifier_.BalloonActive() && !closing_; ++i)
            {
                co_await resume_after(300ms);
                co_await resume_ui{dispatcher_};
            }
        }
        finishHost();
    }

    void MainWindow::ShowNotice(Border const& strip, bool warning, hstring const& title, hstring const& message,
                                std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> const& links)
    {
        Grid g;
        g.ColumnSpacing(8);
        for (GridUnitType t : {GridUnitType::Auto, GridUnitType::Star, GridUnitType::Auto, GridUnitType::Auto})
        {
            ColumnDefinition c;
            c.Width(GridLength{1, t});
            g.ColumnDefinitions().Append(c);
        }
        FontIcon icon;
        icon.Glyph(warning ? L"\uE7BA" : L"\uE946");
        icon.FontSize(14);
        icon.VerticalAlignment(VerticalAlignment::Center);
        if (warning) icon.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 255, 200, 90})});
        g.Children().Append(icon);
        Grid text;   // the title (bold) and the message (dim, cut off with an ellipsis when it does not fit)
        text.ColumnSpacing(10);
        text.VerticalAlignment(VerticalAlignment::Center);
        {
            ColumnDefinition c0, c1;
            c0.Width(GridLength{0, GridUnitType::Auto});
            c1.Width(GridLength{1, GridUnitType::Star});
            text.ColumnDefinitions().Append(c0);
            text.ColumnDefinitions().Append(c1);
        }
        TextBlock t;
        t.Text(title);
        t.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        text.Children().Append(t);
        if (!message.empty())
        {
            TextBlock m;
            m.Text(message);
            m.Opacity(0.75);
            m.TextTrimming(TextTrimming::CharacterEllipsis);
            Grid::SetColumn(m, 1);
            text.Children().Append(m);
        }
        Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(text, box_value(hstring{std::wstring{title} + (message.empty() ? L"" : L"  ") + std::wstring{message}}));
        Grid::SetColumn(text, 1);
        g.Children().Append(text);
        StackPanel buttons;
        buttons.Orientation(Orientation::Horizontal);
        buttons.Spacing(2);
        for (auto const& [label, action] : links)
        {
            HyperlinkButton b;
            b.Content(box_value(label.first));
            b.Padding(ThicknessHelper::FromLengths(8, 2, 8, 2));
            if (!label.second.empty()) Microsoft::UI::Xaml::Controls::ToolTipService::SetToolTip(b, box_value(label.second));
            b.Click([action = action](auto&&, auto&&) { action(); });
            buttons.Children().Append(b);
        }
        Grid::SetColumn(buttons, 2);
        g.Children().Append(buttons);
        Button close;
        close.Content(MakeGlyph(L"\uE711", 10));
        close.Padding(ThicknessHelper::FromUniformLength(6));
        close.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0, 0, 0, 0}});
        close.BorderThickness(ThicknessHelper::FromUniformLength(0));
        Automation::AutomationProperties::SetName(close, lycan::Tr(L"main.close"));
        close.Click([strip, weak = get_weak()](auto&&, auto&&) {
            auto self = weak.get();
            if (!self || !self->AnimOn()) { strip.Visibility(Visibility::Collapsed); return; }
            // fold away: fade out and move up a little, then collapse
            using namespace Microsoft::UI::Xaml::Media;
            using namespace Microsoft::UI::Xaml::Media::Animation;
            auto ct = strip.RenderTransform().try_as<CompositeTransform>();
            if (!ct) { ct = CompositeTransform{}; strip.RenderTransform(ct); }
            Storyboard sb;
            for (auto [target, prop, to] : {std::tuple<DependencyObject, const wchar_t*, double>{strip, L"Opacity", 0.0}, {ct, L"TranslateY", -8.0}})
            {
                DoubleAnimation a;
                a.To(to);
                a.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(150)));
                Storyboard::SetTarget(a, target);
                Storyboard::SetTargetProperty(a, prop);
                sb.Children().Append(a);
            }
            sb.Completed([strip, ct, sb](auto&&, auto&&) { sb.Stop(); strip.Visibility(Visibility::Collapsed); strip.Opacity(1); ct.TranslateY(0); });
            sb.Begin();
        });
        Grid::SetColumn(close, 3);
        g.Children().Append(close);
        strip.Child(g);
        const bool wasHidden = strip.Visibility() == Visibility::Collapsed;
        strip.Visibility(Visibility::Visible);
        if (wasHidden) FadeIn(strip, -8, 200);   // comes down into place
    }

    // The notice above an archive that was opened: a checksum file sits next to it
    void MainWindow::ShowSumsBanner()
    {
        SumsStrip().Visibility(Visibility::Collapsed);
        std::error_code ec;
        if (!current_ || currentPath_.empty() || !fs::is_regular_file(currentPath_, ec)) return;
        const lycan::ArchiveSidecars sc = lycan::FindArchiveSidecars(currentPath_);
        if (sc.archiveSum.empty() && sc.contentsSum.empty()) return;
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        if (!sc.archiveSum.empty())
            links.push_back({{lycan::Tr(L"sums.link_archive"), hstring{lycan::Tf(L"sums.verify_archive_with", sc.archiveSum.filename().wstring())}},
                             [this, sum = sc.archiveSum]() {
                                 HashRequest hr;
                                 hr.files = {currentPath_};
                                 hr.sidecar = sum;
                                 ShowHashDialogAsync(std::move(hr));
                             }});
        if (!sc.contentsSum.empty())
            links.push_back({{lycan::Tr(L"sums.link_files"), hstring{lycan::Tf(L"sums.verify_files_with", sc.contentsSum.filename().wstring())}},
                             [this, sum = sc.contentsSum]() { ShowVerifyDialogAsync({sum}, currentPath_); }});
        ShowNotice(SumsStrip(), false, lycan::Tr(L"sums.banner_title"), {}, links);
    }

    // ---------------------------------------------------------------- Archive information

    // "Archive information": what the archive is (the Windows Properties window is about the file; this is about the archive)
    fire_and_forget MainWindow::ShowArchiveInfoAsync()
    {
        auto strong = get_strong();
        if (!current_) co_return;
        uint64_t files = 0, dirs = 0, unpacked = 0, packed = 0;
        uint32_t encrypted = 0;
        for (auto const& e : current_->Entries())
        {
            if (e.isDir) { ++dirs; continue; }
            ++files;
            unpacked += e.size;
            packed += e.packedSize;
            if (e.isEncrypted) ++encrypted;
        }
        const lycan::ArchiveDetails d = current_->Details();
        const lycan::Capabilities caps = current_->Caps();
        std::error_code ec;
        const uint64_t disk = fs::file_size(currentPath_, ec);
        uint64_t mtime = 0;
        {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(currentPath_.c_str(), GetFileExInfoStandard, &fa))
                mtime = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
        }

        std::vector<std::pair<std::wstring, std::wstring>> rows;
        rows.emplace_back(lycan::Tr(L"main.info_location"), currentPath_.parent_path().wstring());
        rows.emplace_back(lycan::Tr(L"main.format"), current_->FormatName());
        if (!ec) rows.emplace_back(lycan::Tr(L"main.file_size"), lycan::Tf(L"fmt.size_bytes", FormatSize(disk), disk));
        rows.emplace_back(lycan::Tr(L"main.properties"), lycan::Tf(L"main.file_s_folder_s", files, dirs));
        {
            std::wstring s = lycan::Tf(L"fmt.size_bytes", FormatSize(unpacked), unpacked);
            if (unpacked > 0 && !ec) s += lycan::Tf(L"main.archive", std::lround(100.0 * static_cast<double>(disk) / static_cast<double>(unpacked)));
            rows.emplace_back(lycan::Tr(L"main.original_size"), s);
        }
        if (!d.method.empty()) rows.emplace_back(lycan::Tr(L"main.compression_method"), d.method);
        {
            std::wstring s = caps.solid ? lycan::Tr(L"main.yes") : lycan::Tr(L"main.no");
            if (caps.solid && d.blocks) s += lycan::Tf(L"main.block_s", d.blocks);
            rows.emplace_back(lycan::Tr(L"main.solid"), s);
        }
        rows.emplace_back(lycan::Tr(L"main.info_encrypted_label"), caps.encrypted ? lycan::Tf(L"main.yes_encrypted_file_s", encrypted) : lycan::Tr(L"main.no"));
        if (d.volumes > 1) rows.emplace_back(lycan::Tr(L"main.volumes"), lycan::Tf(L"main.volume_s", d.volumes));
        if (mtime) rows.emplace_back(lycan::Tr(L"main.modified"), FormatTime(mtime));
        rows.emplace_back(lycan::Tr(L"main.can_be_changed"), caps.canModify ? lycan::Tr(L"main.yes_files_can_be_added_deleted") : lycan::Tr(L"main.no_format_can_only_be_extracted"));

        auto dlg = MakeDialog(hstring{currentPath_.filename().wstring()}, L"\uE7B8");
        dlg.Resources().Insert(box_value(L"ContentDialogMaxWidth"), box_value(720.0));
        StackPanel sp;
        sp.Spacing(6);
        sp.MinWidth(620);
        Grid grid;
        grid.ColumnSpacing(14);
        grid.RowSpacing(5);
        ColumnDefinition c0, c1;
        c0.Width(GridLength{110, GridUnitType::Pixel});
        c1.Width(GridLength{1, GridUnitType::Star});
        grid.ColumnDefinitions().Append(c0);
        grid.ColumnDefinitions().Append(c1);
        int r = 0;
        for (auto const& [label, value] : rows)
        {
            RowDefinition rd;
            rd.Height(GridLength{0, GridUnitType::Auto});
            grid.RowDefinitions().Append(rd);
            TextBlock l;
            l.Text(hstring{label});
            l.Opacity(0.7);
            Grid::SetRow(l, r);
            grid.Children().Append(l);
            TextBlock v;
            v.Text(hstring{value});
            v.TextWrapping(TextWrapping::Wrap);
            v.IsTextSelectionEnabled(true);
            Grid::SetRow(v, r);
            Grid::SetColumn(v, 1);
            grid.Children().Append(v);
            ++r;
        }
        sp.Children().Append(grid);
        if (!d.comment.empty())
        {
            sp.Children().Append(Caption(lycan::Tr(L"main.archive_comment"), 0.7));
            TextBlock c;
            c.Text(hstring{d.comment});
            c.TextWrapping(TextWrapping::Wrap);
            c.IsTextSelectionEnabled(true);
            ScrollViewer sv;
            sv.MaxHeight(160);
            sv.Content(c);
            sp.Children().Append(MakeCardBox(sv));
        }
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.calculate_hash_values"));
        dlg.SecondaryButtonText(lycan::Tr(L"main.show_explorer"));
        dlg.CloseButtonText(lycan::Tr(L"main.close"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const auto res = co_await ShowDialogAsync(dlg);
        if (res == ContentDialogResult::Primary) OnHashArchiveClick(nullptr, nullptr);
        else if (res == ContentDialogResult::Secondary && fs::exists(currentPath_, ec))
        {
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(currentPath_.c_str()))
            {
                SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
                ILFree(pidl);
            }
        }
    }

    void MainWindow::OnArchiveInfoClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { ShowArchiveInfoAsync(); }

    void MainWindow::OnVerifySumsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { VerifySumsFromPickerAsync(); }

    // "Verify a checksum file…": pick the file, then check the files inside the open archive against it
    fire_and_forget MainWindow::VerifySumsFromPickerAsync()
    {
        auto strong = get_strong();
        if (!current_) co_return;
        auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeChecksum, false, false);
        if (files.Size() == 0) co_return;
        ShowVerifyDialogAsync({fs::path{std::wstring{files.GetAt(0)}}}, currentPath_);
    }
}
