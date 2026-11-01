#include "pch.h"
// The main window: extracting and testing, the conflict question, the task panel.
#include "MainWindowShared.h"

namespace winrt::LycanZip::implementation
{
    // ---------------------------------------------------------------- Extract / test

    std::vector<uint32_t> MainWindow::SelectedNodes()
    {
        std::vector<uint32_t> out;
        if (!entries_) return out;
        for (auto const& it : ActiveList().SelectedItems()) out.push_back(it.as<LycanZip::EntryRow>().NodeId());
        return out;
    }

    // Selected items -> Entry indices of the archive (a folder with its content)
    std::vector<uint32_t> MainWindow::SelectedIndices()
    {
        if (!tree_) return {};
        return tree_->EntryIndices(SelectedNodes());
    }

    // The prefix of the folder being viewed: when items selected here are extracted, this upper path is not kept
    std::wstring MainWindow::StripPrefixForView() const
    {
        if (!tree_ || curNode_ == tree_->Root()) return {};
        return tree_->Node(curNode_).path + L"/";
    }

    fire_and_forget MainWindow::StartExtract(fs::path dest, bool selectionOnly, std::optional<lycan::ExtractMode> modeArg)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        // take the selection before the dialog (password) appears, so a later change of the selection does not affect this run
        std::vector<uint32_t> indices;
        std::wstring strip;
        if (selectionOnly)
        {
            indices = SelectedIndices();
            if (indices.empty()) co_return;
            strip = StripPrefixForView();
        }
        StartExtractCore(std::move(dest), std::move(indices), std::move(strip), selectionOnly, modeArg);
    }

    // The folders / files of the menus (list or folder tree): straight into the destination, folder names on top
    fire_and_forget MainWindow::ExtractNodes(std::vector<uint32_t> nodes, std::wstring strip, bool askFolder)
    {
        auto strong = get_strong();
        if (!current_ || !tree_ || nodes.empty()) co_return;
        fs::path dest = currentPath_.parent_path();
        if (askFolder)
        {
            dest = fs::path(std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, currentPath_.parent_path(),
                                                                             lycan::Tr(L"main.choose_folder_extract"))));
            if (dest.empty()) co_return;
            RememberRecentExtract(dest);
        }
        auto indices = tree_->EntryIndices(nodes);
        if (indices.empty()) co_return;
        StartExtractCore(std::move(dest), std::move(indices), std::move(strip), true, std::nullopt);
    }

    fire_and_forget MainWindow::StartExtractCore(fs::path dest, std::vector<uint32_t> indices, std::wstring strip, bool selectionOnly,
                                                 std::optional<lycan::ExtractMode> modeArg)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        if (!co_await EnsurePasswordAsync()) co_return;

        lycan::ExtractOptions eo;
        eo.skipMacMetadata = settings_.skipMacMetadata;
        eo.keepDamaged = settings_.keepDamaged;
        // a selection goes straight into the destination (no wrapping folder); a whole archive follows the mode
        const lycan::ExtractMode mode = selectionOnly ? lycan::ExtractMode::NoFolder : modeArg.value_or(settings_.extractMode);
        eo.smart = mode == lycan::ExtractMode::Auto;
        if (mode == lycan::ExtractMode::Folder) dest = dest / lycan::ArchiveBaseName(currentPath_);
        eo.stripPrefix = strip;
        eo.overwrite = lycan::ToOverwriteMode(settings_.conflict);
        if (eo.overwrite == lycan::OverwriteMode::Ask) { auto rule = std::make_shared<std::atomic<int>>(0); eo.onConflict = [this, rule](lycan::ConflictInfo const& ci) { return AskConflict(ci, rule); }; }
        ApplyLargeGuard(eo);
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };

        TaskMeta meta;
        meta.archive = currentPath_;
        meta.wholeArchive = !selectionOnly;
        {
            std::lock_guard lk(metaM_);
            uint64_t id = queue_.EnqueueExtract(currentPath_, dest, eo, MakeOpenOptions(), indices);
            taskMeta_[id] = std::move(meta);
        }
    }

    fire_and_forget MainWindow::OnExtractHereClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        StartExtract(currentPath_.parent_path());
    }

    void MainWindow::OnExtractHereSplitClick(Microsoft::UI::Xaml::Controls::SplitButton const&, Microsoft::UI::Xaml::Controls::SplitButtonClickEventArgs const&)
    {
        OnExtractHereClick(nullptr, nullptr);
    }

    // The button that ends the compact window: it appears when nothing is running any more ("Done" when everything worked, "Close" when
    // something failed); Enter (it takes the focus) and Esc press it
    void MainWindow::UpdateCompactDone()
    {
        bool show = false, allOk = true;
        if (compact_ && !closing_ && busyOps_ == 0 && !dialogOpen_ && !quickRunning_ && pendingPaths_.empty())
        {
            bool any = false;
            show = true;
            for (auto const& t : queue_.Snapshot())
            {
                if (!t.Finished()) { show = false; break; }
                any = true;
                const bool ok = t.state == lycan::TaskState::Completed && (t.kind != lycan::TaskKind::Extract || t.extract.ok()) &&
                                (t.kind != lycan::TaskKind::Compress || t.compress.ok()) && (t.kind != lycan::TaskKind::Update || t.update.ok());
                if (!ok && !(t.kind == lycan::TaskKind::Test && t.state == lycan::TaskState::Cancelled)) allOk = false;
            }
            show = show && any;
        }
        const bool was = CompactDoneButton().Visibility() == Visibility::Visible;
        if (show)
        {
            const hstring label = allOk ? lycan::Tr(L"main.done") : lycan::Tr(L"main.close");
            CompactDoneButton().Content(box_value(label));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(CompactDoneButton(), label);
        }
        CompactDoneButton().Visibility(show ? Visibility::Visible : Visibility::Collapsed);
        if (show && !was) CompactDoneButton().Focus(FocusState::Programmatic);
    }

    void MainWindow::OnCompactDoneClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!compact_ || CompactDoneButton().Visibility() != Visibility::Visible) return;  // (Esc while the button is not there: nothing)
        FadeOutAndClose();
    }

    void MainWindow::UpdateCompactOptions()
    {
        CompactOptions().Visibility(compact_ && !compactHost_ ? Visibility::Visible : Visibility::Collapsed);
        if (!compact_ || compactHost_) return;
        auto setBox = [](CheckBox const& box, const wchar_t* text, const wchar_t* name, bool on, bool visible) {
            box.Content(box_value(hstring{text}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, hstring{name});
            box.IsChecked(on);
            box.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
        };
        const bool was = loadingSettings_;
        loadingSettings_ = true;  // setting IsChecked from code must not write back
        if (compactExtract_)
        {
            setBox(OptFirst(), lycan::Tr(L"main.open_folder"), lycan::Tr(L"main.open_folder_when_done"), settings_.openFolderAfterExtract, !compactTest_);
            setBox(OptSecond(), L"", L"", false, false);
        }
        else
        {
            setBox(OptFirst(), lycan::Tr(L"main.show_explorer"), lycan::Tr(L"main.after_completion_show_explorer"), settings_.revealAfterCompress, true);
            setBox(OptSecond(), lycan::Tr(L"main.test_integrity"), lycan::Tr(L"main.after_completion_test_integrity"), settings_.testAfterCompress, true);
        }
        setBox(OptClose(), lycan::Tr(L"main.close_window"), lycan::Tr(L"main.after_completion_close_window"), settings_.closeCompactWhenDone, true);
        loadingSettings_ = was;
    }

    void MainWindow::OnCompactOptionClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        const bool first = OptFirst().IsChecked().Value(), second = OptSecond().IsChecked().Value();
        if (compactExtract_)
            settings_.openFolderAfterExtract = first;
        else
        {
            settings_.revealAfterCompress = first;
            settings_.testAfterCompress = second;
        }
        settings_.closeCompactWhenDone = OptClose().IsChecked().Value();
        LoadSettingsToUi();  // keep the settings page in step
        SaveSettings();
        if (settings_.closeCompactWhenDone) CheckCompactAutoClose();  // it was kept open only because of the option
    }

    void MainWindow::ExtractHereWith(lycan::ExtractMode mode)
    {
        if (current_) StartExtract(currentPath_.parent_path(), false, mode);
    }

    void MainWindow::UpdateExtractButton()
    {
        using lycan::ExtractMode;
        // the folder name in the label is shortened so that the toolbar does not grow with long archive names
        std::wstring base = current_ ? lycan::ArchiveBaseName(currentPath_) : std::wstring(lycan::Tr(L"main.file_name_short"));
        if (base.size() > 18) base = base.substr(0, 17) + L"…";
        auto label = [&](ExtractMode m, bool asMenuItem) -> std::wstring {
            switch (m) {
                case ExtractMode::NoFolder: return lycan::Tr(L"main.extract_here_no_folder");
                case ExtractMode::Folder: return lycan::Tf(L"main.extract_folder", base);
                default: return asMenuItem ? lycan::Tr(L"main.extract_here_automatic") : lycan::Tr(L"main.extract_here");
            }
        };
        const ExtractMode main = settings_.extractMode;
        const std::wstring text = label(main, false);
        ExtractHereText().Text(hstring{text});
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(ExtractHereButton(), hstring{text});
        std::wstring tip = main == ExtractMode::NoFolder ? lycan::Tr(L"main.extract_here_straight_into_folder")
                         : main == ExtractMode::Folder   ? lycan::Tr(L"main.extract_here_always_into_folder")
                                                         : lycan::Tr(L"main.extract_here_ctrl_e_creates_name");
        ToolTipService::SetToolTip(ExtractHereButton(), box_value(hstring{tip}));
        MenuFlyout flyout;
        for (ExtractMode m : {ExtractMode::Auto, ExtractMode::NoFolder, ExtractMode::Folder}) {
            if (m == main) continue;
            MenuFlyoutItem it;
            it.Text(hstring{label(m, true)});
            it.Click([weak = get_weak(), m](auto&&, auto&&) { if (auto s = weak.get()) s->ExtractHereWith(m); });
            flyout.Items().Append(it);
        }
        ExtractHereButton().Flyout(flyout);
        BuildExtractToMenu();
    }

    void MainWindow::OnExtractToSplitClick(SplitButton const&, SplitButtonClickEventArgs const&) { OnExtractToClick(nullptr, nullptr); }

    // The drop-down of "Extract to...": the folders that were chosen before, and the folders people extract to most (extracts there at once)
    void MainWindow::BuildExtractToMenu()
    {
        MenuFlyout flyout;
        auto weak = get_weak();
        flyout.Opening([weak](IInspectable const& sender, IInspectable const&) {
            auto s = weak.get();
            if (!s) return;
            auto menu = sender.as<MenuFlyout>();
            menu.Items().Clear();
            auto add = [&](std::wstring const& dir, std::wstring const& label) {
                MenuFlyoutItem it;
                std::wstring shown = label;
                if (shown.size() > 56) shown = shown.substr(0, 26) + L"…" + shown.substr(shown.size() - 29);   // a long path: cut in the middle
                it.Text(hstring{shown});
                it.Icon(MenuIcon(L"\uE8B7"));
                ToolTipService::SetToolTip(it, box_value(hstring{dir}));
                it.Click([weak, dir](auto&&, auto&&) {
                    if (auto self = weak.get()) { if (self->current_) { self->RememberRecentExtract(fs::path{dir}); self->StartExtract(fs::path{dir}); } }
                });
                menu.Items().Append(it);
            };
            for (auto const& d : s->settings_.recentExtract) add(d, d);
            if (!s->settings_.recentExtract.empty()) menu.Items().Append(MenuFlyoutSeparator{});
            struct Known { const KNOWNFOLDERID* id; const wchar_t* label; };
            for (auto const& k : {Known{&FOLDERID_Desktop, L"main.desktop"}, Known{&FOLDERID_Documents, L"main.documents"}, Known{&FOLDERID_Downloads, L"main.downloads"}})
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(*k.id, 0, nullptr, &path)) && path)
                {
                    add(path, std::wstring{lycan::Tr(k.label)});
                    CoTaskMemFree(path);
                }
            }
        });
        ExtractToButton().Flyout(flyout);
    }

    fire_and_forget MainWindow::OnExtractToClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, currentPath_.parent_path(),
                                                                            lycan::Tr(L"main.choose_folder_extract")))};
        if (!dir.empty()) { RememberRecentExtract(dir); StartExtract(dir); }
    }

    // Selection bar / context menu: extract only the selected items
    fire_and_forget MainWindow::OnExtractSelHereClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        StartExtract(currentPath_.parent_path(), true);
    }

    fire_and_forget MainWindow::OnExtractSelToClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_ || SelectedNodes().empty()) co_return;
        const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeExtractDest, currentPath_.parent_path(),
                                                                            lycan::Tr(L"main.choose_folder_extract")))};
        if (!dir.empty()) { RememberRecentExtract(dir); StartExtract(dir, true); }
    }

    // "Scan for malware": a task; what it found (or why it could not) is told when it is done
    fire_and_forget MainWindow::OnScanClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        if (current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;  // without the password the encrypted files are skipped; ask first
        queue_.EnqueueScan(currentPath_, MakeOpenOptions());
    }

    void MainWindow::FinishScanTask(lycan::TaskInfo const& info)
    {
        if (info.state == lycan::TaskState::Cancelled) return;
        auto const& r = info.scan;
        if (r.amsiHr != 0)
        {
            ShowMessage(lycan::Tr(L"main.scan_could_not_run"), hstring{lycan_app::ScanProblemText(r.amsiHr) + L"\n\n" +
                                                      lycan::Tr(L"main.scan_hands_files_antivirus_product")});
            return;
        }
        if (!r.Found()) return;  // the card says what was scanned
        std::wstring list;
        size_t shown = 0;
        for (auto const& t : r.threats)
        {
            if (shown++ >= 15) break;
            list += L"\n• " + t.path;
            if (t.blocked) list += lycan::Tr(L"main.blocked_by_system_policy");
            else if (!t.name.empty())
            {
                list += L"\n    " + t.name;
                if (!t.severity.empty() || !t.category.empty())
                    list += lycan::Tf(L"fmt.paren", t.severity + (t.severity.empty() || t.category.empty() ? std::wstring() : std::wstring{lycan::Tr(L"fmt.comma")}) + t.category);
            }
        }
        if (r.threats.size() > shown) list += L"\n" + lycan::Tf(L"main.more", r.threats.size() - shown);
        bool unmatched = false;
        for (auto const& t : r.threats) if (!t.blocked && t.name.empty()) unmatched = true;
        if (unmatched && !r.threatNames.empty())
        {
            std::wstring all;
            for (auto const& n : r.threatNames) all += (all.empty() ? std::wstring{} : std::wstring{lycan::Tr(L"fmt.list_sep")}) + n;
            list += L"\n\n" + lycan::Tf(L"main.threat_names_logged_by_windows", all);
        }
        ShowMessage(lycan::Tr(L"main.suspicious_files_found"),
                    hstring{lycan::Tf(L"main.installed_antivirus_product_thinks", r.threats.size()) + list + L"\n\n" +
                            lycan::Tr(L"main.do_not_open_these_files_antivirus") + L"\n\n" + lycan::Tr(L"scan.affected_item_note")});
    }

    fire_and_forget MainWindow::OnTestClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        if (!current_) co_return;
        if (!co_await EnsurePasswordAsync()) co_return;
        queue_.EnqueueTest(currentPath_, MakeOpenOptions());
    }

    // ---------------------------------------------------------------- Conflict question

    // Automatic rules (the index in the drop-down of the dialog; 0 = ask every time). When it cannot be decided (for example the modified time is missing) it is always "keep both", so no data is lost.
    static const wchar_t* const kAutoRuleNames[] = {
        L"main.ask_every_time_no_automatic_choice", L"main.keep_newer", L"main.keep_older", L"main.keep_larger", L"main.keep_smaller", L"main.skip_if_contents_identical"};

    static lycan::ConflictAction ApplyAutoRule(int rule, lycan::ConflictInfo const& ci)
    {
        using A = lycan::ConflictAction;
        auto pick = [](bool known, uint64_t existing, uint64_t incoming, bool preferHigher) {
            if (!known) return A::Rename;
            if (existing == incoming) return A::Skip;  // tie: keep the existing one
            return (incoming > existing) == preferHigher ? A::Overwrite : A::Skip;
        };
        const bool timeKnown = ci.existingMTime && ci.newMTime;
        switch (rule)
        {
        case 1: return pick(timeKnown, ci.existingMTime, ci.newMTime, true);
        case 2: return pick(timeKnown, ci.existingMTime, ci.newMTime, false);
        case 3: return pick(true, ci.existingSize, ci.newSize, true);
        case 4: return pick(true, ci.existingSize, ci.newSize, false);
        case 5: return (timeKnown && ci.existingSize == ci.newSize && ci.existingMTime == ci.newMTime) ? A::Skip : A::Rename;
        }
        return A::Rename;
    }

    // The check of a suspiciously big archive: on unless Settings > Extract says no (the tests make the limits small: LYCAN_TEST_LARGE_BYTES)
    void MainWindow::ApplyLargeGuard(lycan::ExtractOptions& eo)
    {
        if (!settings_.bombCheck) return;
        eo.onLarge = [this](lycan::LargeExtractInfo const& i) { return AskLarge(i); };
        wchar_t buf[24];
        if (GetEnvironmentVariableW(L"LYCAN_TEST_LARGE_BYTES", buf, static_cast<DWORD>(std::size(buf))) > 0)
        {
            eo.largeBytes = _wcstoui64(buf, nullptr, 10);
            eo.largeRatio = 50;
        }
    }

    bool MainWindow::AskLarge(lycan::LargeExtractInfo const& info)
    {
        auto req = std::make_shared<LargeRequest>();
        req->info = info;
        Post([this, req] { ShowLargeDialog(req); });
        std::unique_lock lk(req->m);
        while (!req->done)
        {
            if (closing_) return false;
            req->cv.wait_for(lk, 200ms);
        }
        return req->answer;
    }

    fire_and_forget MainWindow::ShowLargeDialog(std::shared_ptr<LargeRequest> req)
    {
        auto strong = get_strong();
        PlaySfx(L"conflict");
        auto const& li = req->info;
        auto dlg = MakeDialog(lycan::Tr(L"main.archive_unpacks_something_very_big"), L"\uE7BA");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(460);
        const uint64_t ratio = li.packedBytes ? li.declaredBytes / li.packedBytes : 0;
        TextBlock tb;
        tb.Text(hstring{ratio ? lycan::Tf(L"main.archive_only_unpacks_about_times", FormatSize(li.packedBytes), FormatSize(li.declaredBytes), ratio)
                              : lycan::Tf(L"main.unpacks_about", FormatSize(li.declaredBytes))});
        tb.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
        tb.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(tb);
        TextBlock note;
        note.Text(lycan::Tr(L"main.far_more_than_normal_archive_may"));
        note.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(note);
        CheckBox never;
        never.Content(box_value(lycan::Tr(L"main.do_not_check_again_can_be")));
        sp.Children().Append(never);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.continue_extracting"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const bool go = co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary;
        if (go && never.IsChecked().Value())
        {
            settings_.bombCheck = false;
            SaveSettings();
            LoadSettingsToUi();
        }
        req->Finish(go);
    }

    lycan::ConflictAnswer MainWindow::AskConflict(lycan::ConflictInfo const& info, std::shared_ptr<std::atomic<int>> const& rule)
    {
        if (int r = rule->load()) return {ApplyAutoRule(r, info), false};
        auto req = std::make_shared<ConflictRequest>();
        req->info = info;
        req->rule = rule;
        Post([this, req] { ShowConflictDialog(req); });

        std::unique_lock lk(req->m);
        while (!req->done)
        {
            if (closing_) return {lycan::ConflictAction::Cancel, false};
            req->cv.wait_for(lk, 200ms);
        }
        return req->answer;
    }

    fire_and_forget MainWindow::ShowConflictDialog(std::shared_ptr<ConflictRequest> req)
    {
        auto strong = get_strong();
        auto const& ci = req->info;
        PlaySfx(L"conflict");

        auto dlg = MakeDialog(lycan::Tr(L"main.conflict_title"), L"\uE8C8");
        StackPanel sp;
        sp.Spacing(14);
        sp.MinWidth(440);

        const std::wstring fileName = ci.existing.filename().wstring();
        const FileCategory cat = CategoryOfName(fileName, false, false);
        {
            StackPanel where;
            where.Spacing(2);
            TextBlock n;
            n.Text(hstring{fileName});
            n.FontSize(16);
            n.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            n.TextTrimming(TextTrimming::CharacterEllipsis);
            where.Children().Append(n);
            TextBlock loc = Caption(ci.existing.parent_path().wstring(), 0.65);
            loc.IsTextSelectionEnabled(true);
            where.Children().Append(loc);
            sp.Children().Append(where);
        }

        // Two versions side by side: the existing file vs the version in the archive, marking the newer / larger one
        const bool known = ci.existingMTime && ci.newMTime;
        const bool newer = known && ci.newMTime > ci.existingMTime;
        const bool older = known && ci.newMTime < ci.existingMTime;
        auto makeSide = [&](const wchar_t* label, uint64_t size, uint64_t mtime, std::wstring const& badge, bool accentBadge) {
            StackPanel side;
            side.Spacing(6);
            side.Children().Append(Caption(label, 0.7));
            StackPanel line;
            line.Orientation(Orientation::Horizontal);
            line.Spacing(8);
            line.Children().Append(MakeGlyph(GlyphForCategory(cat), 24, BrushForCategory(cat)));
            StackPanel info;
            TextBlock sz;
            sz.Text(hstring{FormatSize(size)});
            sz.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            info.Children().Append(sz);
            info.Children().Append(Caption(FormatTime(mtime), 0.75));
            line.Children().Append(info);
            side.Children().Append(line);
            if (!badge.empty())
            {
                Border pill;
                pill.CornerRadius(CornerRadiusHelper::FromUniformRadius(10));
                pill.Padding(ThicknessHelper::FromLengths(8, 1, 8, 1));
                pill.HorizontalAlignment(HorizontalAlignment::Left);
                auto c = accentBadge ? AccentColorNow() : Windows::UI::Color{255, 160, 170, 190};
                pill.Background(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{0x30, c.R, c.G, c.B}});
                TextBlock bt;
                bt.Text(hstring{badge});
                bt.FontSize(12);
                bt.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{c});
                pill.Child(bt);
                side.Children().Append(pill);
            }
            return MakeCardBox(side, 14, 12);
        };
        auto sizeNote = [&](bool forArchive) -> std::wstring {
            if (ci.newSize == ci.existingSize) return lycan::Tr(L"main.same_size");
            bool bigger = forArchive ? ci.newSize > ci.existingSize : ci.existingSize > ci.newSize;
            return bigger ? lycan::Tr(L"main.larger") : lycan::Tr(L"main.smaller");
        };
        Grid cmp;
        cmp.ColumnSpacing(12);
        for (int i = 0; i < 2; ++i) { ColumnDefinition cd; cd.Width(GridLength{1, GridUnitType::Star}); cmp.ColumnDefinitions().Append(cd); }
        auto left = makeSide(lycan::Tr(L"main.existing_file"), ci.existingSize, ci.existingMTime, older ? lycan::Tr(L"main.newer") : (known && !newer ? L"" : L""), true);
        auto right = makeSide(lycan::Tr(L"main.version_archive"), ci.newSize, ci.newMTime, newer ? lycan::Tr(L"main.newer") : older ? lycan::Tr(L"main.older") : (known ? lycan::Tr(L"main.same_time") : L""), newer);
        Grid::SetColumn(right, 1);
        cmp.Children().Append(left);
        cmp.Children().Append(right);
        sp.Children().Append(cmp);
        sp.Children().Append(Caption(lycan::Tf(L"main.conflict_archive_version", sizeNote(true)), 0.65));

        RadioButtons options;
        options.Header(box_value(lycan::Tr(L"main.what_would_you_like_do")));
        options.Items().Append(TitledOption(lycan::Tr(L"main.overwrite_existing_file"), lycan::Tr(L"main.replace_with_version_archive")));
        options.Items().Append(TitledOption(lycan::Tr(L"main.keep_both"), lycan::Tr(L"main.new_file_renamed_automatically")));
        options.Items().Append(TitledOption(lycan::Tr(L"main.skip_file"), lycan::Tr(L"main.keep_existing_file_do_not_extract")));
        options.SelectedIndex(1);  // the default is "keep both": no data is lost
        sp.Children().Append(options);

        CheckBox all;
        all.Content(box_value(lycan::Tr(L"main.apply_same_choice_all_later")));
        sp.Children().Append(all);

        // Automatic choice: decided file by file by the rule (newer / older / larger / smaller / skip when the same); once chosen, no conflict of this extraction is asked again
        ComboBox autoRule;
        autoRule.Header(box_value(lycan::Tr(L"main.choose_automatically")));
        autoRule.HorizontalAlignment(HorizontalAlignment::Stretch);
        Automation::AutomationProperties::SetName(autoRule, lycan::Tr(L"main.automatic_choice"));
        for (auto n : kAutoRuleNames) autoRule.Items().Append(box_value(lycan::Tr(n)));
        autoRule.SelectedIndex(0);
        autoRule.SelectionChanged([options, all, autoRule](auto&&, auto&&) {
            const bool manual = autoRule.SelectedIndex() <= 0;
            options.IsEnabled(manual);
            all.IsEnabled(manual);
        });
        sp.Children().Append(autoRule);

        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel_extraction"));
        dlg.DefaultButton(ContentDialogButton::Primary);

        auto result = co_await ShowDialogAsync(dlg);
        if (result != ContentDialogResult::Primary)
        {
            req->Finish({lycan::ConflictAction::Cancel, false});
            co_return;
        }
        if (int r = autoRule.SelectedIndex(); r > 0)
        {
            if (req->rule) req->rule->store(r);
            req->Finish({ApplyAutoRule(r, ci), false});
            co_return;
        }
        lycan::ConflictAction a = options.SelectedIndex() == 0 ? lycan::ConflictAction::Overwrite
                                  : options.SelectedIndex() == 2 ? lycan::ConflictAction::Skip
                                                                 : lycan::ConflictAction::Rename;
        req->Finish({a, all.IsChecked().Value()});
    }

    // ---------------------------------------------------------------- Task panel

    void MainWindow::OnTaskUpdate(lycan::TaskInfo const& info)
    {
        if (info.kind == lycan::TaskKind::Hash)  // the dialog shows it (no card, no notification)
        {
            auto h = hashHandlers_.find(info.id);
            if (h != hashHandlers_.end())
            {
                auto fn = h->second;  // it may restart the task and so change the map
                fn(info);
            }
            return;
        }
        if (info.kind == lycan::TaskKind::Test)
        {
            auto ft = testFor_.find(info.id);
            if (ft != testFor_.end())
            {
                OnFollowupTestUpdate(ft->second, info);  // shown inside the card of the finished compression
                return;
            }
        }
        LycanZip::TaskRow row{nullptr};
        auto it = taskById_.find(info.id);
        if (it == taskById_.end())
        {
            row = make<implementation::TaskRow>(info.id);
            taskById_.insert_or_assign(info.id, row);
            taskRows_.InsertAt(0, row);  // newest on top
            PlaySfx(L"start");
        }
        else row = it->second;
        get_self<implementation::TaskRow>(row)->Apply(info);
        NotifyStateChanged(info.Finished() || info.state == lycan::TaskState::Queued);  // new task / finished: update at once; progress only once per second
        const bool panelWasHidden = TaskPanel().Visibility() != Visibility::Visible;
        TaskPanel().Visibility(Visibility::Visible);
        if (panelWasHidden) FadeIn(TaskPanel(), 16, 240);  // the task card enters
        if (info.Finished() && fxDoneTasks_.insert(info.id).second)
        {
            if (info.state == lycan::TaskState::Failed) PlaySfx(L"error");
            else if (info.state == lycan::TaskState::Completed) { PlaySfx(L"done"); PulseTaskPanel(); }
            if (info.state == lycan::TaskState::Completed || info.state == lycan::TaskState::Failed) GlowMoon(row, info.state == lycan::TaskState::Failed);
            UpdateTaskbar(&info);
        }
        else UpdateTaskbar(nullptr);
        UpdateTaskSummary();
        if (compact_) UpdateCompactDone();

        if (info.Finished() && info.state == lycan::TaskState::Failed && info.tempSpace) HandleTempSpaceTask(info);  // no room for the scratch file: ask where else
        if (info.Finished() && info.kind == lycan::TaskKind::Extract)
        {
            TaskMeta* meta = nullptr;
            {
                std::lock_guard lk(metaM_);
                auto m = taskMeta_.find(info.id);
                if (m != taskMeta_.end() && !m->second.handled) { m->second.handled = true; meta = &m->second; }
            }
            if (meta) FinishExtractTask(info, *meta);
        }
        if (info.Finished() && info.kind == lycan::TaskKind::Compress) FinishCompressTask(info);
        if (info.Finished() && info.kind == lycan::TaskKind::Scan && scanShown_.insert(info.id).second) FinishScanTask(info);
        if (info.Finished() && info.kind == lycan::TaskKind::Repack && repackShown_.insert(info.id).second) FinishRepackTask(info);
        if (info.Finished() && info.kind == lycan::TaskKind::Update)
        {
            TaskMeta meta;
            bool have = false;
            {
                std::lock_guard lk(metaM_);
                auto m = taskMeta_.find(info.id);
                if (m != taskMeta_.end() && !m->second.handled) { m->second.handled = true; meta = m->second; have = true; }
            }
            if (have) FinishUpdateTask(info, meta);
        }
        CheckCompactAutoClose();
    }

    // What is done after an extraction task ends: handling a wrong password, opening the folder when done, deleting the original archive
    void MainWindow::FinishExtractTask(lycan::TaskInfo const& info, TaskMeta& meta)
    {
        if (info.state == lycan::TaskState::Failed && info.extract.wrongPassword)
        {
            // password rejected: forget it, the next operation asks again and verifies
            if (current_ && currentPath_ == meta.archive) { password_.clear(); passwordVerified_ = false; }
            ShowMessage(lycan::Tr(L"main.wrong_password"), lycan::Tr(L"main.archive_could_not_be_opened_with"));
            return;
        }
        if (!meta.openFile.empty())
        {
            // temporary extraction done -> open with the default program. If there is no associated program, open the "Choose an app" dialog instead
            if (info.state == lycan::TaskState::Completed && info.extract.ok())
            {
                if (!meta.editEntry.empty()) { StartEditSession(meta); return; }  // "edit": the program is started and the file watched
                if (_wgetenv(L"LYCAN_TEST_NO_OPEN")) return;  // for tests: do not really start the associated program
                // working directory = the file's folder: programs often load their files by relative path
                const std::wstring workDir = meta.openFile.parent_path().wstring();
                SHELLEXECUTEINFOW sei{sizeof(sei)};
                sei.fMask = SEE_MASK_NOCLOSEPROCESS;  // remember the process: its temp folder must outlive it (see OpenTempDir.h)
                sei.lpVerb = nullptr;  // the default verb, as in Explorer (fonts have only "preview", no "open")
                sei.lpFile = meta.openFile.c_str();
                sei.lpDirectory = workDir.c_str();
                sei.nShow = SW_SHOWNORMAL;
                if (ShellExecuteExW(&sei)) lycan_app::TrackLaunch(lycan_app::OpenTempOwnerDir(meta.openFile), sei.hProcess);
                else
                {
                    ShellExecuteW(nullptr, L"openas", meta.openFile.c_str(), nullptr, workDir.c_str(), SW_SHOWNORMAL);
                    lycan_app::TrackLaunch(lycan_app::OpenTempOwnerDir(meta.openFile), nullptr);
                }
            }
            return;
        }
        if (info.state != lycan::TaskState::Completed || !info.extract.ok()) return;

        auto row = taskById_.find(info.id);
        if (settings_.openFolderAfterExtract && !info.extract.outputDir.empty())
            ShellExecuteW(nullptr, L"open", info.extract.outputDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

        // The original archive can be deleted with a button on the card (never automatically). Only offered when the
        // whole archive was extracted without skipping or rejecting anything, and not for volumes (one part only).
        if (meta.wholeArchive && info.extract.skipped.empty() && info.extract.rejected.empty() && !IsVolumePath(meta.archive))
        {
            meta.deletable = true;
            if (row != taskById_.end()) get_self<implementation::TaskRow>(row->second)->SetDeletable(true, hstring{lycan::Tr(L"main.delete_original_archive")});
        }
    }

    // Taskbar progress and the notification when done. finished not empty = this update is because a task just ended.
    // The progress is the total of all running tasks; only when the whole batch (all tasks) ends a summary notification is sent, so there is no pop-up for each one.
    void MainWindow::UpdateTaskbar(lycan::TaskInfo const* finished)
    {
        using PS = lycan_app::ProgressState;
        if (finished && finished->state != lycan::TaskState::Cancelled)
        {
            const bool ok = finished->state == lycan::TaskState::Completed &&
                            (finished->kind != lycan::TaskKind::Extract || finished->extract.ok()) &&
                            (finished->kind != lycan::TaskKind::Compress || finished->compress.ok()) &&
                            (finished->kind != lycan::TaskKind::Update || finished->update.ok());
            ++(ok ? batchOk_ : batchBad_);
            batchTitle_ = finished->title;
            batchKind_ = finished->kind;
            batchError_ = ok ? L"" : !finished->error.empty() ? finished->error.Text()
                          : finished->extract.wrongPassword ? lycan::Tr(L"main.wrong_password") : lycan::Tr(L"main.some_items_could_not_be_processed");
        }

        int active = 0, paused = 0;
        unsigned long long done = 0, total = 0;
        for (auto const& t : queue_.Snapshot())
        {
            if (t.Finished()) continue;
            ++active;
            if (t.state == lycan::TaskState::Paused) ++paused;
            if (t.total > 0) { done += t.done; total += t.total; }
        }

        if (active > 0)
        {
            const PS now = paused == active ? PS::Paused : total == 0 ? PS::Indeterminate : PS::Normal;
            if (now != lastTaskbarState_) { lastTaskbarState_ = now; lycan::diag::Info(L"taskbar progress: {} ({} task(s) running)", static_cast<int>(now), active); }
            notifier_.SetProgress(now, done, total);
            SetTitleProgress(total > 0 ? static_cast<int>(std::min<unsigned long long>(done * 100 / total, 100)) : -1);
            return;
        }
        SetTitleProgress(-1);
        lastTaskbarState_ = PS::None;

        if (batchOk_ + batchBad_ == 0)
        {
            if (notifier_.State() != PS::Error) notifier_.SetProgress(PS::None);  // cancelled: clear the progress bar
            return;
        }

        // the whole batch ended
        const int ok = batchOk_, bad = batchBad_;
        const bool single = ok + bad == 1;
        const wchar_t* kind = batchKind_ == lycan::TaskKind::Compress ? lycan::Tr(L"main.compression") : batchKind_ == lycan::TaskKind::Test ? lycan::Tr(L"main.integrity_test") : batchKind_ == lycan::TaskKind::Update ? lycan::Tr(L"main.change_archive") : batchKind_ == lycan::TaskKind::Scan ? lycan::Tr(L"main.scan_malware") : batchKind_ == lycan::TaskKind::Repack ? lycan::Tr(L"main.conversion") : lycan::Tr(L"main.extraction");
        std::wstring title, text;
        if (single)
        {
            title = bad ? lycan::Tf(L"main.op_failed", kind) : lycan::Tf(L"main.complete", kind);
            text = batchTitle_ + (bad ? lycan::Tf(L"fmt.colon_value", batchError_) : std::wstring());
        }
        else
        {
            title = lycan::Tr(L"app.tasks_finished");
            text = lycan::Tf(L"main.succeeded", ok) + (bad ? lycan::Tf(L"main.suffix_n_failed", bad) : std::wstring());
        }
        batchOk_ = batchBad_ = 0;

        notifier_.SetProgress(bad ? PS::Error : PS::None);
        const bool foreground = notifier_.AppIsForeground();
        if (!foreground) notifier_.FlashUntilForeground();
        const bool inBackground = sBackgroundActive && sBackgroundActive();  // the App sends one notification for everything
        if (settings_.notifications && !closing_ && !inBackground && (bad > 0 || !foreground)) notifier_.Notify(title, text, bad > 0);
    }

    // Summary next to the panel title: how many running / all done / with failures
    void MainWindow::UpdateTaskSummary()
    {
        int running = 0, failed = 0, total = 0;
        for (auto const& t : queue_.Snapshot())
        {
            ++total;
            if (!t.Finished()) ++running;
            else if (t.state == lycan::TaskState::Failed) ++failed;
        }
        std::wstring text;
        if (running) text = lycan::Tf(L"main.progress", running);
        else if (failed) text = lycan::Tf(L"main.n_failed", failed);
        else if (total) text = lycan::Tr(L"main.all_done");
        TaskSummary().Text(hstring{text});
        const auto bgVis = running > 0 ? Visibility::Visible : Visibility::Collapsed;  // only while something runs
        BackgroundButton().Visibility(bgVis);
        CompactBackgroundButton().Visibility(bgVis);
    }

    void MainWindow::OnTaskCollapseClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        bool collapse = TaskScroll().Visibility() == Visibility::Visible;
        TaskScroll().Visibility(collapse ? Visibility::Collapsed : Visibility::Visible);
        TaskCollapseGlyph().Glyph(collapse ? L"\uE70E" : L"\uE70D");  // expanded = arrow down; collapsed = arrow up
    }

    void MainWindow::OnClearFinishedClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        for (int i = static_cast<int>(taskRows_.Size()) - 1; i >= 0; --i)
        {
            auto row = taskRows_.GetAt(static_cast<uint32_t>(i)).as<LycanZip::TaskRow>();
            lycan::TaskInfo info;
            if (queue_.Get(row.Id(), info) && info.Finished())
            {
                taskById_.erase(row.Id());
                { std::lock_guard lk(metaM_); taskMeta_.erase(row.Id()); }
                taskRows_.RemoveAt(static_cast<uint32_t>(i));
            }
        }
        if (taskRows_.Size() == 0) TaskPanel().Visibility(Visibility::Collapsed);
        UpdateTaskSummary();
    }

    uint64_t TagId(Windows::Foundation::IInspectable const& sender)
    {
        return unbox_value<uint64_t>(sender.as<FrameworkElement>().Tag());
    }

    void MainWindow::OnTaskPauseClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { queue_.Pause(ActiveTaskFor(TagId(s))); }
    void MainWindow::OnTaskResumeClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { queue_.Resume(ActiveTaskFor(TagId(s))); }
    void MainWindow::OnTaskCancelClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&) { queue_.Cancel(ActiveTaskFor(TagId(s))); }

    void MainWindow::OnTaskOpenFolderClick(Windows::Foundation::IInspectable const& s, RoutedEventArgs const&)
    {
        auto path = unbox_value<hstring>(s.as<FrameworkElement>().Tag());
        if (!path.empty()) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}
