#include "pch.h"
// The main window: the settings page (all tabs except Passwords), the scratch folder, the context menu and Windows 11 menu, file types, the check for a new version.
#include "MainWindowShared.h"

namespace winrt::LycanZip::implementation
{
    // ---------------------------------------------------------------- Settings

    // After a DataTemplate (list row, task row) is instantiated, translate the button text written into it
    void MainWindow::OnTemplateLoaded(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        if (auto d = sender.try_as<DependencyObject>()) lycan_app::TranslateTemplateRoot(d);
    }


    // Shows a notice when the chosen language differs from the one loaded now (it takes effect after a restart)
    void MainWindow::UpdateLangNote()
    {
        const bool pending = lycan::i18n::Resolve(settings_.language, lycan::i18n::DefaultLangDir()) != lycan::i18n::Current();
        LangRestartNote().Visibility(pending ? Visibility::Visible : Visibility::Collapsed);
    }

    void MainWindow::LoadSettingsToUi()
    {
        loadingSettings_ = true;
        UpdateShellItemsUi();
        ThemeCombo().SelectedIndex(static_cast<int>(settings_.theme));
        {
            // the menu: follow the system, then the languages of lycan::kLanguages (the first item is in the XAML)
            if (LangCombo().Items().Size() == 1)
                for (const auto& l : lycan::kLanguages) {
                    ComboBoxItem item;
                    item.Content(box_value(hstring{lycan::Tr(l.nameId)}));
                    LangCombo().Items().Append(item);
                }
            const int found = lycan::LanguageIndex(settings_.language);
            LangCombo().SelectedIndex(found < 0 ? 0 : found + 1);
            UpdateLangNote();
        }
        AccentGrid().SelectedIndex(static_cast<int>(settings_.accent));
        BackgroundCombo().SelectedIndex(static_cast<int>(settings_.background));
        ConflictCombo().SelectedIndex(static_cast<int>(settings_.conflict));
        ExtractModeCombo().SelectedIndex(static_cast<int>(settings_.extractMode));
        CloseCompactToggle().IsOn(settings_.closeCompactWhenDone);
        RevealToggle().IsOn(settings_.revealAfterCompress);
        OpenFolderToggle().IsOn(settings_.openFolderAfterExtract);
        BombCheckToggle().IsOn(settings_.bombCheck);
        SkipMacMetaToggle().IsOn(settings_.skipMacMetadata);
        ShellIconsToggle().IsOn(settings_.shellIcons);
        KeepDamagedToggle().IsOn(settings_.keepDamaged);
        ConfirmDeleteToggle().IsOn(settings_.confirmDeleteArchive);
        ConfirmDeleteEntriesToggle().IsOn(settings_.confirmDeleteEntries);
        ConfirmWriteBackToggle().IsOn(settings_.confirmWriteBack);
        EditorPathBox().Text(hstring{settings_.editorPath});
        KeepBackupToggle().IsOn(settings_.keepBackupOnModify);
        DragVirtualToggle().IsOn(settings_.dragOutVirtual);
        SkipThumbsToggle().IsOn(settings_.skipThumbsDb);
        HideMacMetaToggle().IsOn(settings_.hideMacMetadata);
        WarnExecToggle().IsOn(settings_.warnExecutable);
        LockOpenToggle().IsOn(settings_.lockOpenArchive);
        UpdateCheckToggle().IsOn(settings_.checkUpdates);
        ExecScopeCombo().SelectedIndex(static_cast<int>(settings_.execScope));
        TestAfterToggle().IsOn(settings_.testAfterCompress);
        SumsArchiveToggle().IsOn(settings_.sumsArchive);
        SumsFilesToggle().IsOn(settings_.sumsFiles);
        DefaultFormatCombo().SelectedIndex(IndexOf(kFormatOrder, settings_.defaultFormat));
        DefaultProfileCombo().SelectedIndex(IndexOf(kProfileOrder, settings_.defaultProfile));
        // details, list, then the icon view in its four sizes (one choice, like the View menu)
        ViewStyleCombo().SelectedIndex(settings_.viewMode == lycan::ViewMode::Icons ? 2 + static_cast<int>(std::min(settings_.thumbSize, 3u)) : static_cast<int>(settings_.viewMode));
        ViewStyleCombo().IsEnabled(!settings_.rememberViewMode);  // when "remember the last choice" is on, the default view is the last choice
        RememberViewToggle().IsOn(settings_.rememberViewMode);
        ThumbsToggle().IsOn(settings_.iconThumbnails);
        ViewModeCombo().SelectedIndex(settings_.browseFlat ? 1 : 0);
        OpenPolicyCombo().SelectedIndex(static_cast<int>(settings_.openIn));
        DensityCombo().SelectedIndex(settings_.compactRows ? 0 : 1);
        FolderTreeToggle().IsOn(settings_.showFolderTree);
        PreviewPaneToggle().IsOn(settings_.previewPane);
        DblPreviewCombo().SelectedIndex(settings_.doubleClickPreview ? 0 : 1);
        ComicReadingToggle().IsOn(settings_.comicReading);
        AutoTryPasswordsToggle().IsOn(settings_.autoTryPasswords);
        RunPasswordsToggle().IsOn(settings_.rememberRunPasswords);
        RememberPasswordDefaultToggle().IsOn(settings_.rememberPasswordDefault);
        OpenPreviewKeyCombo().SelectedIndex(static_cast<int>(settings_.openPreviewKey));
        DblAudioCombo().SelectedIndex(settings_.doubleClickAudioPreview ? 0 : 1);
        {
            constexpr unsigned kPrefetchSteps[] = {0, 128, 256, 512, 1024, 2048};
            int sel = 2;
            for (int i = 0; i < 6; ++i) if (settings_.prefetchMB <= kPrefetchSteps[i]) { sel = i; break; }
            if (settings_.prefetchMB > 2048) sel = 5;
            PrefetchCombo().SelectedIndex(sel);
        }
        EnlargeToggle().IsOn(settings_.previewEnlargeSmall);
        AnimToggle().IsOn(settings_.animations);
        SoundToggle().IsOn(settings_.soundEffects);
        SoundSetCombo().SelectedIndex(static_cast<int>(settings_.soundSet));
        NotifyToggle().IsOn(settings_.notifications);
        DebugLogToggle().IsOn(settings_.debugLog);
        LogPathsToggle().IsOn(settings_.logFullPaths);
        ParallelBox().Value(settings_.parallelTasks);
        UpdateTempCard();
        TaskPriorityCombo().SelectedIndex(static_cast<int>(settings_.taskPriority));
        loadingSettings_ = false;
    }

    void MainWindow::ReadSettingsFromUi()
    {
        if (loadingSettings_) return;
        auto idx = [](ComboBox const& c, int max) { return std::clamp(c.SelectedIndex(), 0, max); };
        settings_.theme = static_cast<lycan::ThemeMode>(idx(ThemeCombo(), 3));
        {
            const int sel = idx(LangCombo(), lycan::kLanguageCount);   // 0 = follow the system
            settings_.language = sel == 0 ? L"auto" : lycan::kLanguages[sel - 1].code;
        }
        UpdateLangNote();
        settings_.conflict = static_cast<lycan::ConflictPolicy>(idx(ConflictCombo(), 3));
        settings_.extractMode = static_cast<lycan::ExtractMode>(idx(ExtractModeCombo(), 2));
        settings_.closeCompactWhenDone = CloseCompactToggle().IsOn();
        settings_.revealAfterCompress = RevealToggle().IsOn();
        settings_.openFolderAfterExtract = OpenFolderToggle().IsOn();
        settings_.bombCheck = BombCheckToggle().IsOn();
        settings_.skipMacMetadata = SkipMacMetaToggle().IsOn();
        const bool shellIconsChanged = settings_.shellIcons != ShellIconsToggle().IsOn();
        settings_.shellIcons = ShellIconsToggle().IsOn();
        if (shellIconsChanged)
        {
            if (entries_) for (auto const& it : entries_) ApplyShellIcon(it.as<LycanZip::EntryRow>());
            for (auto const& [id, tn] : treeNodes_) if (tn.Content()) ApplyShellIcon(tn.Content().as<LycanZip::EntryRow>());
            RefreshShellIcons();
        }
        settings_.keepDamaged = KeepDamagedToggle().IsOn();
        settings_.confirmDeleteArchive = ConfirmDeleteToggle().IsOn();
        settings_.confirmDeleteEntries = ConfirmDeleteEntriesToggle().IsOn();
        settings_.confirmWriteBack = ConfirmWriteBackToggle().IsOn();
        settings_.keepBackupOnModify = KeepBackupToggle().IsOn();
        settings_.dragOutVirtual = DragVirtualToggle().IsOn();
        settings_.skipThumbsDb = SkipThumbsToggle().IsOn();
        const bool hideMetaChanged = settings_.hideMacMetadata != HideMacMetaToggle().IsOn();
        settings_.hideMacMetadata = HideMacMetaToggle().IsOn();
        settings_.warnExecutable = WarnExecToggle().IsOn();
        settings_.checkUpdates = UpdateCheckToggle().IsOn();
        {
            const bool was = settings_.lockOpenArchive;
            settings_.lockOpenArchive = LockOpenToggle().IsOn();
            if (was != settings_.lockOpenArchive)
            {
                if (settings_.lockOpenArchive && current_ && updateTask_ == 0) LockArchiveFile(currentPath_);
                else if (!settings_.lockOpenArchive) UnlockArchiveFile();
            }
        }
        settings_.execScope = static_cast<lycan::OpenScope>(idx(ExecScopeCombo(), 1));
        settings_.testAfterCompress = TestAfterToggle().IsOn();
        settings_.sumsArchive = SumsArchiveToggle().IsOn();
        settings_.sumsFiles = SumsFilesToggle().IsOn();
        settings_.defaultFormat = kFormatOrder[idx(DefaultFormatCombo(), 5)];   // (no ISO among the defaults)
        settings_.defaultProfile = kProfileOrder[idx(DefaultProfileCombo(), 3)];
        const int viewChoice = idx(ViewStyleCombo(), 5);   // 0 details, 1 list, 2..5 icons small .. huge
        const lycan::ViewMode chosenView = viewChoice < 2 ? static_cast<lycan::ViewMode>(viewChoice) : lycan::ViewMode::Icons;
        const unsigned chosenSize = viewChoice >= 2 ? static_cast<unsigned>(viewChoice - 2) : settings_.thumbSize;
        const bool thumbChanged = settings_.iconThumbnails != ThumbsToggle().IsOn() || settings_.thumbSize != chosenSize;
        const bool rememberChanged = settings_.rememberViewMode != RememberViewToggle().IsOn();
        settings_.rememberViewMode = RememberViewToggle().IsOn();
        settings_.iconThumbnails = ThumbsToggle().IsOn();
        settings_.thumbSize = chosenSize;
        bool viewChanged = settings_.viewMode != chosenView || settings_.browseFlat != (idx(ViewModeCombo(), 1) == 1) || settings_.showFolderTree != FolderTreeToggle().IsOn();
        settings_.viewMode = chosenView;
        settings_.background = static_cast<lycan::BackgroundStyle>(idx(BackgroundCombo(), 2));
        settings_.browseFlat = idx(ViewModeCombo(), 1) == 1;
        settings_.openIn = static_cast<lycan::OpenPolicy>(idx(OpenPolicyCombo(), 1));
        settings_.compactRows = idx(DensityCombo(), 1) == 0;
        settings_.showFolderTree = FolderTreeToggle().IsOn();
        settings_.previewPane = PreviewPaneToggle().IsOn();
        settings_.doubleClickPreview = idx(DblPreviewCombo(), 1) == 0;
        settings_.comicReading = ComicReadingToggle().IsOn();
        settings_.autoTryPasswords = AutoTryPasswordsToggle().IsOn();
        settings_.rememberRunPasswords = RunPasswordsToggle().IsOn();
        settings_.rememberPasswordDefault = RememberPasswordDefaultToggle().IsOn();
        settings_.openPreviewKey = static_cast<lycan::PreviewKey>(idx(OpenPreviewKeyCombo(), 2));
        settings_.doubleClickAudioPreview = idx(DblAudioCombo(), 1) == 0;
        {
            constexpr unsigned kPrefetchSteps[] = {0, 128, 256, 512, 1024, 2048};
            settings_.prefetchMB = kPrefetchSteps[idx(PrefetchCombo(), 5)];
        }
        settings_.previewEnlargeSmall = EnlargeToggle().IsOn();
        settings_.animations = AnimToggle().IsOn();
        const lycan::SoundSet oldSoundSet = settings_.soundSet;
        settings_.soundEffects = SoundToggle().IsOn();
        settings_.soundSet = static_cast<lycan::SoundSet>(idx(SoundSetCombo(), 1));
        if (settings_.soundSet != oldSoundSet && settings_.soundEffects) PlaySfx(L"done");  // hear the other set
        settings_.notifications = NotifyToggle().IsOn();
        {
            const bool dbgChanged = settings_.debugLog != DebugLogToggle().IsOn();
            settings_.debugLog = DebugLogToggle().IsOn();
            settings_.logFullPaths = LogPathsToggle().IsOn();
            if (dbgChanged) lycan::diag::Configure(settings_.debugLog, settings_.logFullPaths);  // (written even when it is switched off: the end of the detailed part)
            if (dbgChanged) lycan::diag::Write(lycan::diag::Level::Warn, settings_.debugLog ? L"the detailed log was switched on" : L"the detailed log was switched off");
        }
        settings_.taskPriority = static_cast<lycan::TaskPriority>(idx(TaskPriorityCombo(), 2));
        double n = ParallelBox().Value();
        settings_.parallelTasks = std::isnan(n) ? 1u : static_cast<unsigned>(std::clamp(n, 1.0, 4.0));
        SaveSettings();
        ApplySettings();
        if (rememberChanged) ViewStyleCombo().IsEnabled(!settings_.rememberViewMode);
        if (thumbChanged && tree_ && !viewChanged) { UpdateViewMenuChecks(); RefreshList(true); }
        if (viewChanged) ApplyViewSettings();  // only when the default view itself changed is the view of the archive open now overridden (not reset because another setting changed)
        if (hideMetaChanged) RebuildTree();
    }

    void MainWindow::SaveSettings()
    {
        if (!lycan::SaveSettings(settingsPath_, settings_))
            AboutSettingsPath().Text(hstring{lycan::Tf(L"main.cannot_write_settings_file", settingsPath_.wstring())});
        StampSettings();
        lycan_app::SettingsHub::Get().Publish(this, settings_);  // the other windows get the newest settings at once
    }

    // Another window saved the settings: replace the working copy of this window and apply (only the look and global behaviour, not the view state of the archive that is open)
    void MainWindow::ApplyExternalSettings(lycan::Settings const& s)
    {
        if (s == settings_) return;
        settings_ = s;
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
        LoadColWidths();
        UpdateListColumns();
        StampSettings();  // the file was just written by the other side: no need to reload it again on activation
    }

    int MainWindow::UnfinishedTaskCount()
    {
        int n = 0;
        for (auto const& t : queue_.Snapshot())
            if (!t.Finished()) ++n;
        return n;
    }

    MainWindow::TaskStats MainWindow::CollectTaskStats()
    {
        TaskStats s;
        for (auto const& t : queue_.Snapshot())
        {
            if (!t.Finished())
            {
                ++s.running;
                if (t.total > 0) { s.done += t.done; s.total += t.total; }
            }
            else
            {
                const bool ok = t.state != lycan::TaskState::Failed &&
                                (t.state != lycan::TaskState::Completed ||
                                 ((t.kind != lycan::TaskKind::Extract || t.extract.ok()) && (t.kind != lycan::TaskKind::Compress || t.compress.ok()) && (t.kind != lycan::TaskKind::Update || t.update.ok())));
                if (!ok) ++s.failed;
            }
        }
        if (busyOps_ > 0 || dialogOpen_ || quickRunning_ || !pendingPaths_.empty()) s.needsUi = 1;
        return s;
    }

    // Background work: the window disappears, the tasks (in worker processes) keep running
    void MainWindow::HideForBackground()
    {
        lycan_app::CloseImagePreview(this);
        lycan_app::CloseAudioPreview(this);
        if (previewPane_) previewPane_->CloseLarge();
        AppWindow().Hide();
    }

    // --preview-images / --preview-audio: the window never shows; the preview window is the whole UI
    void MainWindow::SetPreviewOnly()
    {
        previewOnly_ = true;
        AppWindow().Hide();
    }

    // The preview could not be shown straight away (password, damaged file, nothing to preview): show this window and
    // open the archive the normal way so the user sees what is going on.
    void MainWindow::LeavePreviewOnly(fs::path const& path, std::wstring const& note)
    {
        previewOnly_ = false;
        openNote_ = note;
        AppWindow().Show();
        ActivateWindow();
        OpenArchiveAsync(path, 0, L"");
    }

    fire_and_forget MainWindow::PreviewOnlyAsync(fs::path path, bool audio)
    {
        auto strong = get_strong();
        std::shared_ptr<lycan::ArchiveSource> src;
        std::vector<lycan_app::PreviewItem> images;
        std::vector<lycan_app::AudioItem> tracks;
        bool encrypted = false;
        co_await resume_background();
        try
        {
            lycan::OpenOptions oo;
            oo.tempDir = lycan_app::ScratchOverride();
            src = lycan::ArchiveSource::Open(path, oo);
            encrypted = src->Caps().encrypted;
            // every level of the archive, in path order (the same as the list's "show everything" view)
            lycan::ArchiveTree tree(src->Entries(), true);  // the junk macOS adds is never previewed
            for (uint32_t id : tree.Descendants(tree.Root(), lycan::SortKey::Path, true, false))
            {
                auto const& n = tree.Node(id);
                if (n.isDir || n.entryIndex == lycan::kNoEntry) continue;
                if (audio) { if (lycan_app::IsPreviewableAudioName(n.name)) tracks.push_back({n.entryIndex, n.name, n.path, n.size}); }
                else if (lycan_app::CanPreviewImage(n.name)) images.push_back({n.entryIndex, n.name, n.path, n.size, 0, n.packedSize, n.mtimeFileTime});
            }
        }
        catch (...) { src.reset(); }  // password, damaged, no room...: the normal window explains it
        co_await resume_ui{dispatcher_};
        if (closing_) co_return;

        if (!src || encrypted) { LeavePreviewOnly(path, L""); co_return; }
        if (images.empty() && tracks.empty())
        {
            LeavePreviewOnly(path, audio ? lycan::Tr(L"main.there_no_audio_preview_archive") : lycan::Tr(L"main.there_no_images_preview_archive"));
            co_return;
        }

        auto onClosed = [weak = get_weak()] {
            if (auto self = weak.get())
                self->dispatcher_.TryEnqueue([weak] { if (auto s = weak.get()) if (s->previewOnly_ && !s->closing_) s->Close(); });  // the last window: the process ends
        };
        if (audio)
        {
            lycan_app::AudioRequest req;
            req.owner = this;
            req.archive = path;
            req.open.tempDir = lycan_app::ScratchOverride();
            req.items = std::move(tracks);
            req.onClosed = onClosed;
            req.loopMode = static_cast<int>(settings_.audioLoop);
            req.onLoopMode = [weak = get_weak()](int m) { if (auto s = weak.get()) { s->settings_.audioLoop = static_cast<lycan::AudioLoop>(m); s->SaveSettings(); } };
            lycan_app::ShowAudioPreview(std::move(req));
        }
        else
        {
            lycan_app::PreviewRequest req;
            req.owner = this;
            req.archive = path;
            req.open.tempDir = lycan_app::ScratchOverride();
            req.prefetchBytes = static_cast<uint64_t>(settings_.prefetchMB) << 20;
            req.enlargeSmall = settings_.previewEnlargeSmall;
            req.animations = AnimOn();
            req.thumbReads = src->Caps().randomAccess && !src->Caps().solid;
            req.items = std::move(images);   // every picture of the archive in path order: the book of the reading mode already
            // a comic book archive (.cbz .cb7 .cbr .cbt) opens in the reading mode (setting)
            req.reading = settings_.comicReading && lycan::IconFamilyForPath(path.wstring()) == lycan::IconFamily::Comic;
            req.readingStore = ReadingStorePath();
            req.onClosed = onClosed;  // no main window to hand files to: "open with the associated program" (Enter) is unavailable
            lycan_app::ShowImagePreview(std::move(req));
        }
    }

    void MainWindow::ShowFromBackground()
    {
        AppWindow().Show();
        ActivateWindow();
    }

    void MainWindow::OnBackgroundClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (sEnterBackground) sEnterBackground();
    }

    // ---------------------------------------------------------------- temporary files

    void MainWindow::UpdateTempCard()
    {
        const bool custom = !settings_.tempDir.empty();
        TempDirText().Text(hstring{custom ? settings_.tempDir : std::wstring{lycan::Tr(L"main.system_temporary_folder")}});
        TempResetButton().IsEnabled(custom);
        RefreshTempUsage();
    }

    fire_and_forget MainWindow::RefreshTempUsage()
    {
        auto strong = get_strong();
        co_await resume_background();
        const uint64_t used = lycan_app::OpenTempUsage();
        co_await resume_ui{dispatcher_};
        TempUsageText().Text(hstring{lycan::Tf(L"main.use_opened_dragged_files", FormatSize(used))});
    }

    fire_and_forget MainWindow::OnTempChooseClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        const fs::path picked{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeTempDir,
                                                                               settings_.tempDir.empty() ? fs::path{} : fs::path{settings_.tempDir}, lycan::Tr(L"main.choose_temporary_folder")))};
        if (picked.empty()) co_return;
        if (lycan::IsSlowLocation(picked))
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.location_slow"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.network_removable_drive_temporary", picked.wstring())});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(420);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"main.use_anyway"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Close);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        }
        settings_.tempDir = picked.wstring();
        SaveSettings();
        ApplySettings();
        UpdateTempCard();
    }

    void MainWindow::OnTempResetClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        settings_.tempDir.clear();
        SaveSettings();
        ApplySettings();
        UpdateTempCard();
    }

    fire_and_forget MainWindow::OnTempClearClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        co_await resume_background();
        const uint64_t freed = lycan_app::ClearOpenTemp();
        co_await resume_ui{dispatcher_};
        RefreshTempUsage();
        ShowMessage(lycan::Tr(L"main.temporary_files_cleared"), hstring{lycan::Tf(L"main.freed_files_use_not_deleted", FormatSize(freed))});
    }

    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskTempLocationAsync(uint64_t need, uint64_t have, std::wstring dir)
    {
        auto strong = get_strong();
        co_await resume_ui{dispatcher_};
        // local disks with room, other than the one that is full
        const std::wstring fullRoot = fs::path(dir).root_path().wstring();
        const uint64_t want = need ? need + need / 20 : (2ull << 30);
        std::vector<fs::path> bases;
        auto dlg = MakeDialog(lycan::Tr(L"main.not_enough_room_temporary_files"), L"");
        StackPanel sp;
        sp.Spacing(12);
        TextBlock info;
        info.Text(hstring{need ? lycan::Tf(L"main.about_intermediate_data_has_be_put", FormatSize(need), dir, FormatSize(have))
                               : lycan::Tf(L"main.temporary_location_full_left", dir, FormatSize(have))});
        info.TextWrapping(TextWrapping::Wrap);
        info.MaxWidth(460);
        sp.Children().Append(info);
        RadioButtons options;
        options.Header(box_value(lycan::Tr(L"main.where_use_instead_local_disks_only")));
        for (auto const& v : lycan::LocalFixedVolumes())
        {
            if (v.root.wstring() == fullRoot || v.freeBytes < want) continue;
            bases.push_back(v.root);
            TextBlock t;
            t.Text(hstring{lycan::Tf(L"main.free", v.root.wstring(), FormatSize(v.freeBytes))});
            options.Items().Append(t);
        }
        TextBlock none;
        none.Text(lycan::Tr(L"main.no_other_local_disk_has_enough"));
        none.TextWrapping(TextWrapping::Wrap);
        none.MaxWidth(460);
        none.Opacity(0.8);
        none.Visibility(bases.empty() ? Visibility::Visible : Visibility::Collapsed);
        sp.Children().Append(none);
        if (!bases.empty()) options.SelectedIndex(0);
        options.Visibility(bases.empty() ? Visibility::Collapsed : Visibility::Visible);
        sp.Children().Append(options);
        TextBlock slow;
        slow.Text(lycan::Tr(L"main.network_removable_drive_temporary_files_will_be_slow"));
        slow.Opacity(0.8);
        slow.Visibility(Visibility::Collapsed);
        HyperlinkButton other;
        other.Content(box_value(lycan::Tr(L"main.choose_another_folder")));
        other.Padding(ThicknessHelper::FromUniformLength(0));
        CheckBox always;
        always.Content(box_value(lycan::Tr(L"main.always_use_temporary_location")));
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.use_location_try_again"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        dlg.IsPrimaryButtonEnabled(!bases.empty());
        const auto owner = AppWindow().Id();
        other.Click([&, owner](auto&&, auto&&) -> fire_and_forget {
            const fs::path p{std::wstring(co_await lycan_app::PickFolderAsync(owner, lycan_app::kPurposeTempDir, {}, lycan::Tr(L"main.choose_temporary_folder")))};
            if (p.empty()) co_return;
            bases.push_back(p);
            TextBlock t;
            t.Text(hstring{lycan::Tf(L"main.chosen", p.wstring())});
            options.Visibility(Visibility::Visible);
            options.Items().Append(t);
            options.SelectedIndex(static_cast<int>(options.Items().Size()) - 1);
            none.Visibility(Visibility::Collapsed);
            slow.Visibility(lycan::IsSlowLocation(p) ? Visibility::Visible : Visibility::Collapsed);
            dlg.IsPrimaryButtonEnabled(true);
        });
        sp.Children().Append(other);
        sp.Children().Append(slow);
        sp.Children().Append(always);
        const auto r = co_await ShowDialogAsync(dlg);
        if (r != ContentDialogResult::Primary) co_return hstring{};
        const int i = options.SelectedIndex();
        if (i < 0 || static_cast<size_t>(i) >= bases.size()) co_return hstring{};
        const fs::path base = bases[static_cast<size_t>(i)];
        if (always.IsChecked().Value())
        {
            settings_.tempDir = base.wstring();
            SaveSettings();
            ApplySettings();
            UpdateTempCard();
        }
        else lycan_app::SetSessionTempBase(base);  // for the rest of this run
        co_return hstring{base.wstring()};
    }

    // a task failed because the scratch folder was too small: ask where else, then run it again there
    fire_and_forget MainWindow::HandleTempSpaceTask(lycan::TaskInfo info)
    {
        auto strong = get_strong();
        if (!tempAsked_.insert(info.id).second) co_return;
        const hstring base = co_await AskTempLocationAsync(info.tempNeed, info.tempHave, info.tempDir);
        if (base.empty()) co_return;
        const uint64_t nid = queue_.Retry(info.id, fs::path{std::wstring{base}} / L"LycanZip");
        if (!nid) co_return;
        {
            std::lock_guard lk(metaM_);  // what happens after the task (test the new archive, open the folder...) follows to the new one
            if (auto c = compressMeta_.find(info.id); c != compressMeta_.end()) compressMeta_[nid] = c->second;
            if (auto t = taskMeta_.find(info.id); t != taskMeta_.end()) { auto m = t->second; m.handled = false; taskMeta_[nid] = std::move(m); }
        }
        RemoveTaskRow(info.id);
    }

    void MainWindow::RemoveTaskRow(uint64_t id)
    {
        for (uint32_t i = 0; i < taskRows_.Size(); ++i)
            if (taskRows_.GetAt(i).as<LycanZip::TaskRow>().Id() == id)
            {
                taskRows_.RemoveAt(i);
                break;
            }
        taskById_.erase(id);
        {
            std::lock_guard lk(metaM_);
            taskMeta_.erase(id);
            compressMeta_.erase(id);
        }
        if (taskRows_.Size() == 0) TaskPanel().Visibility(Visibility::Collapsed);
        UpdateTaskSummary();
    }

    fire_and_forget MainWindow::AskCloseWithTasksAsync(int count)
    {
        auto strong = get_strong();
        if (askingClose_ || dialogOpen_) co_return;  // already asking, or another dialog is not answered yet: handle that first
        askingClose_ = true;
        auto dlg = MakeDialog(lycan::Tr(L"main.tasks_still_running"), L"\uE7BA");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.still_unfinished_closing_now", count)});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.cancel_tasks_close"));
        dlg.CloseButtonText(lycan::Tr(L"main.keep_waiting"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const auto r = co_await ShowDialogAsync(dlg);
        askingClose_ = false;
        if (r == ContentDialogResult::Primary)
        {
            closeConfirmed_ = true;
            queue_.CancelAll();
            Close();
        }
    }

    void MainWindow::NotifyStateChanged(bool force)
    {
        if (!sStateChanged) return;
        const uint64_t now = GetTickCount64();
        if (!force && now - stateTick_ < 1000) return;
        stateTick_ = now;
        sStateChanged();
    }

    void MainWindow::AppendCrashState(std::vector<fs::path>& archives, std::vector<lycan::CrashTask>& tasks) const
    {
        if (current_) archives.push_back(currentPath_);
        else if (openPending_ > 0 && !openingPath_.empty()) archives.push_back(openingPath_);
        for (auto const& t : queue_.Snapshot())
        {
            if (t.Finished()) continue;
            lycan::CrashTask ct;
            ct.kind = t.kind == lycan::TaskKind::Compress ? L"Compress" : t.kind == lycan::TaskKind::Test ? L"Test" : t.kind == lycan::TaskKind::Update ? L"Update" : t.kind == lycan::TaskKind::Scan ? L"Scan" : t.kind == lycan::TaskKind::Repack ? L"Repack" : L"Extract";
            ct.state = t.state == lycan::TaskState::Running ? L"Running" : t.state == lycan::TaskState::Paused ? L"Paused" : L"Queued";
            ct.title = t.title;
            ct.target = t.target;
            tasks.push_back(std::move(ct));
        }
    }

    // The last crash: first remove the compression temporary files that are clearly ours and mark the report as seen (however the user answers, do not ask at every start), then explain
    void MainWindow::ShowCrashRecovery(std::vector<lycan::CrashReport> reports)
    {
        int cleaned = 0;
        for (auto& r : reports)
        {
            cleaned += lycan::CleanupCrashLeftovers(r);
            lycan::MarkCrashReportSeen(r);
        }
        ShowCrashRecoveryAsync(std::move(reports), cleaned);
    }

    fire_and_forget MainWindow::ShowCrashRecoveryAsync(std::vector<lycan::CrashReport> reports, int cleaned)
    {
        auto strong = get_strong();
        if (reports.empty() || dialogOpen_) co_return;
        auto const& r = reports.front();  // the newest one
        auto kindName = [](std::wstring const& k) -> std::wstring {
            return k == L"Compress" ? lycan::Tr(L"main.compression") : k == L"Test" ? lycan::Tr(L"main.integrity_test") : lycan::Tr(L"main.extraction");
        };
        constexpr size_t kShow = 5;
        std::wstring body = lycan::Tf(L"main.lycanzip_closed_unexpectedly_at", r.time);
        if (!r.archives.empty())
        {
            body += L"\n\n" + lycan::Tf(L"main.open_at_time", r.archives.size());
            for (size_t i = 0; i < r.archives.size() && i < kShow; ++i) body += L"\n• " + r.archives[i].filename().wstring();
            if (r.archives.size() > kShow) body += L"\n…";
        }
        if (!r.tasks.empty())
        {
            body += L"\n\n" + lycan::Tf(L"main.not_finished", r.tasks.size());
            bool anyExtract = false;
            for (size_t i = 0; i < r.tasks.size(); ++i)
            {
                if (r.tasks[i].kind == L"Extract") anyExtract = true;
                if (i < kShow) body += L"\n• " + kindName(r.tasks[i].kind) + L" " + r.tasks[i].title;
            }
            if (r.tasks.size() > kShow) body += L"\n…";
            if (anyExtract) body += L"\n" + std::wstring(lycan::Tr(L"main.destination_extraction_may_contain"));
        }
        if (cleaned > 0) body += L"\n\n" + lycan::Tf(L"main.removed_unfinished_compression", cleaned);
        const fs::path crashDir = r.file.parent_path();
        body += L"\n\n" + lycan::Tf(L"main.crash_report_including_memory_dump", crashDir.wstring());

        auto dlg = MakeDialog(lycan::Tr(L"main.lycanzip_closed_unexpectedly_last"), L"\uE7BA");
        TextBlock tb;
        tb.Text(hstring{body});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(520);
        dlg.Content(tb);
        std::vector<fs::path> reopen;
        for (auto const& a : r.archives)
        {
            std::error_code ec;
            if (reopen.size() < kShow && fs::is_regular_file(a, ec)) reopen.push_back(a);
        }
        if (!reopen.empty()) dlg.PrimaryButtonText(hstring{lycan::Tf(L"main.reopen", reopen.size())});
        dlg.SecondaryButtonText(lycan::Tr(L"main.open_report_folder"));
        dlg.CloseButtonText(lycan::Tr(L"main.skip"));
        dlg.DefaultButton(reopen.empty() ? ContentDialogButton::Close : ContentDialogButton::Primary);
        const auto res = co_await ShowDialogAsync(dlg);
        if (res == ContentDialogResult::Primary)
        {
            if (sOpenInNewWindow)
                for (auto const& a : reopen) sOpenInNewWindow(a);
        }
        else if (res == ContentDialogResult::Secondary)
            ShellExecuteW(nullptr, L"open", crashDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::StampSettings()
    {
        std::error_code ec;
        auto t = fs::last_write_time(settingsPath_, ec);
        settingsStamp_ = ec ? fs::file_time_type{} : t;
    }

    // Several windows: when another window saved the settings file (theme, animations, notifications ...), reload and apply it when this window is activated.
    // The state of the archive view (view mode, sort, thumbnails) is not touched, only the global look / behaviour settings are updated.
    void MainWindow::ReloadSettingsIfChanged()
    {
        std::error_code ec;
        auto t = fs::last_write_time(settingsPath_, ec);
        if (ec || t == settingsStamp_) return;
        settingsStamp_ = t;
        auto fresh = lycan::LoadSettings(settingsPath_);
        if (fresh == settings_) return;
        settings_ = fresh;
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
    }

    bool MainWindow::IsOpenableArchive(fs::path const& p) { return IsArchivePath(p); }

    // The same archive (case-insensitive / any way of writing the path): this window shows it or is opening it
    bool MainWindow::ShowsArchive(fs::path const& p) const
    {
        auto same = [&](fs::path const& a) {
            if (a.empty()) return false;
            std::error_code ec;
            return fs::equivalent(a, p, ec) && !ec;
        };
        return (current_ && same(currentPath_)) || (openPending_ > 0 && same(openingPath_));
    }

    bool MainWindow::IsPristine() const
    {
        return view_ == View::Home && !current_ && !compact_ && openPending_ == 0 && busyOps_ == 0 && !dialogOpen_ && !cPageActive_ &&
               taskById_.empty() && pendingPaths_.empty() && !quickRunning_ && !closing_;
    }

    bool MainWindow::ReplaceOnOpen() const
    {
        return settings_.openIn == lycan::OpenPolicy::Replace && taskById_.empty() && pendingPaths_.empty() && !dialogOpen_ && !cPageActive_ &&
               !quickRunning_ && !closing_ && openPending_ == 0 && busyOps_ == 0;
    }

    Windows::Foundation::IAsyncOperation<int> MainWindow::AskOpenBatchAsync(int total, int safeCount, int maxCount)
    {
        auto strong = get_strong();
        if (dialogOpen_) co_return safeCount;  // another dialog is not answered yet: open only the safe number, do not stack dialogs
        const int all = (std::min)(total, maxCount);
        auto dlg = MakeDialog(lycan::Tr(L"main.opening_several_archives"), L"\uE8A7");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.received_archives_at_once_each", total)});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(hstring{all < total ? lycan::Tf(L"main.open_first", all) : lycan::Tf(L"main.open_all", all)});
        dlg.SecondaryButtonText(hstring{lycan::Tf(L"main.open_only_first", safeCount)});
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Secondary);
        const auto r = co_await ShowDialogAsync(dlg);
        co_return r == ContentDialogResult::Primary ? all : r == ContentDialogResult::Secondary ? safeCount : 0;
    }

    void MainWindow::OpenFromUi(fs::path p)
    {
        if (!ReplaceOnOpen() && !IsPristine() && sOpenInNewWindow) { sOpenInNewWindow(std::move(p)); return; }
        OpenArchiveChecked(std::move(p));
    }

    // A later volume of a split archive (name.part2.rar, name.002 ...) shows only part of the archive: every time, the user is
    // asked whether to open the first volume instead (never automatic). Without the first volume next to it there is nothing to
    // offer: the volume opens, with a note.
    fire_and_forget MainWindow::OpenArchiveChecked(fs::path path)
    {
        auto strong = get_strong();
        const lycan::VolumeInfo vol = lycan::DetectVolume(path);
        if (vol.IsLater())
        {
            std::error_code ec;
            if (fs::exists(vol.first, ec))
            {
                OpGuard guard{this};
                auto dlg = MakeDialog(lycan::Tr(L"main.later_volume_split_archive"), L"\uE8B7");
                TextBlock tb;
                tb.Text(hstring{lycan::Tf(L"main.volume_split_archive_one_volume", path.filename().wstring(), vol.index, vol.first.filename().wstring())});
                tb.TextWrapping(TextWrapping::Wrap);
                tb.MaxWidth(440);
                dlg.Content(tb);
                dlg.PrimaryButtonText(lycan::Tr(L"main.open_first_volume"));
                dlg.SecondaryButtonText(lycan::Tr(L"main.open_volume_anyway"));
                dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
                dlg.DefaultButton(ContentDialogButton::Primary);
                const auto r = co_await ShowDialogAsync(dlg);
                if (r == ContentDialogResult::Primary) path = vol.first;
                else if (r != ContentDialogResult::Secondary) { CheckCompactAutoClose(); co_return; }
            }
            else openNote_ = lycan::Tf(L"main.first_volume_was_not_found_only", vol.first.filename().wstring());
        }
        OpenArchiveAsync(std::move(path), 0, L"");
    }

    void MainWindow::ApplySettings()
    {
        const bool contrast = settings_.theme == lycan::ThemeMode::ContrastBlack;
        RootGrid().RequestedTheme(settings_.theme == lycan::ThemeMode::Light  ? ElementTheme::Light
                                  : settings_.theme == lycan::ThemeMode::Dark || contrast ? ElementTheme::Dark
                                                                              : ElementTheme::Default);
        if (contrast != lycan_app::ContrastThemeOn() || contrast)
        {
            const bool changed = contrast != lycan_app::ContrastThemeOn();
            lycan_app::SetContrastTheme(contrast);
            if (!contrast) ApplyAccent();
            if (changed)
            {   // the controls ask for their brushes again when the theme changes
                RootGrid().RequestedTheme(ElementTheme::Light);
                RootGrid().RequestedTheme(contrast ? ElementTheme::Dark : (settings_.theme == lycan::ThemeMode::Light ? ElementTheme::Light : settings_.theme == lycan::ThemeMode::Dark ? ElementTheme::Dark : ElementTheme::Default));
            }
        }
        queue_.SetMaxParallel(settings_.parallelTasks);
        queue_.SetDefaultPriority(settings_.taskPriority);
        lycan::diag::Configure(settings_.debugLog, settings_.logFullPaths);
        lycan_app::SetCustomTempBase(settings_.tempDir);
        ApplyDensity();
        UpdateBackdrop();
        UpdateExtractButton();
        UpdateCompactOptions();
        syncingUi_ = true;
        PaneToggle().IsChecked(settings_.previewPane);
        syncingUi_ = false;
        UpdateTreeVisibility();
    }

    // ---- Accent colour: overrides the SystemAccentColor resources at Application level, so buttons / selection / progress etc. change together
    namespace
    {
        struct AccentDef { const wchar_t* name; uint8_t r, g, b; };
        constexpr AccentDef kAccentDefs[] = {{L"main.moonlight", 0x4C, 0x8D, 0xF6}, {L"main.blood_moon", 0xD6, 0x45, 0x50}, {L"main.amber_eyes", 0xE0, 0x93, 0x1F},
                                             {L"main.aurora", 0x1F, 0xB5, 0xA0}, {L"main.forest", 0x3E, 0x9B, 0x5A},
                                             {L"main.night", 0xA9, 0xB6, 0xCC}};   // Night: a silver accent (the background is near black, see UpdateBackdrop)
        constexpr int kAccentCount = 7;  // including "follow the system"

        Windows::UI::Color Mix(AccentDef const& a, double toward, double t)  // toward: 255 = white, 0 = black
        {
            auto m = [&](uint8_t c) { return static_cast<uint8_t>(c + (toward - c) * t + 0.5); };
            return Windows::UI::Color{255, m(a.r), m(a.g), m(a.b)};
        }
    }

    namespace
    {
        struct AccentPalette { Windows::UI::Color fill, text; };

        // Change the colour of the brush itself: the controls share one brush instance, so after Color changes all places that use it follow at once (no reload needed)
        void SetBrushColor(ResourceDictionary const& dict, const wchar_t* key, Windows::UI::Color c)
        {
            auto k = box_value(hstring{key});
            if (!dict.HasKey(k)) return;
            if (auto b = dict.Lookup(k).try_as<Microsoft::UI::Xaml::Media::SolidColorBrush>()) b.Color(c);
        }

        void ApplyPalette(ResourceDictionary const& dict, AccentPalette const& dark, AccentPalette const& light)
        {
            for (auto const& kv : dict.ThemeDictionaries())
            {
                auto name = unbox_value_or<hstring>(kv.Key(), L"");
                auto sub = kv.Value().try_as<ResourceDictionary>();
                if (!sub) continue;
                const AccentPalette* pal = nullptr;
                if (name == L"Light") pal = &light;
                else if (name == L"Default" || name == L"Dark") pal = &dark;  // high contrast (HighContrast) keeps the system setting
                if (!pal) continue;
                auto alpha = [](Windows::UI::Color c, uint8_t a) { c.A = a; return c; };
                SetBrushColor(sub, L"AccentFillColorDefaultBrush", pal->fill);
                SetBrushColor(sub, L"AccentFillColorSecondaryBrush", alpha(pal->fill, 0xE6));
                SetBrushColor(sub, L"AccentFillColorTertiaryBrush", alpha(pal->fill, 0xCC));
                SetBrushColor(sub, L"AccentFillColorSelectedTextBackgroundBrush", pal->fill);
                SetBrushColor(sub, L"AccentTextFillColorPrimaryBrush", pal->text);
                SetBrushColor(sub, L"AccentTextFillColorSecondaryBrush", pal->text);
                SetBrushColor(sub, L"AccentTextFillColorTertiaryBrush", alpha(pal->text, 0xCC));
                SetBrushColor(sub, L"SystemControlHighlightAccentBrush", pal->fill);
                SetBrushColor(sub, L"SystemControlForegroundAccentBrush", pal->text);
            }
            for (auto const& m : dict.MergedDictionaries()) ApplyPalette(m, dark, light);
        }
    }

    void MainWindow::ApplyAccent()
    {
        AccentPalette dark, light;
        int idx = static_cast<int>(settings_.accent);
        if (idx >= 0 && idx < kAccentCount - 1)
        {
            auto const& a = kAccentDefs[idx];
            // As in Windows: the fill of the dark theme uses Light2, the light theme Dark1; accent text uses Light3 / Dark2
            dark = {Mix(a, 255, 0.22), Mix(a, 255, 0.50)};
            light = {Mix(a, 0, 0.20), Mix(a, 0, 0.40)};
        }
        else  // follow the system: use the accent colour family of Windows
        {
            Windows::UI::ViewManagement::UISettings ui;
            using Windows::UI::ViewManagement::UIColorType;
            dark = {ui.GetColorValue(UIColorType::AccentLight2), ui.GetColorValue(UIColorType::AccentLight3)};
            light = {ui.GetColorValue(UIColorType::AccentDark1), ui.GetColorValue(UIColorType::AccentDark2)};
        }
        ApplyPalette(Application::Current().Resources(), dark, light);
        if (lycan_app::ContrastThemeOn()) lycan_app::SetContrastTheme(true);   // the accent does not replace the contrast colours
        UpdateBackdrop();
    }

    // ---- Background: the gradient changes with the accent theme and dark / light; the start page also has the halo, stars and wolf shadow (side silhouette)
    namespace
    {
        Windows::UI::Color MixColor(Windows::UI::Color c, double toward, double t, uint8_t alpha = 255)
        {
            auto m = [&](uint8_t v) { return static_cast<uint8_t>(v + (toward - v) * t + 0.5); };
            return Windows::UI::Color{alpha, m(c.R), m(c.G), m(c.B)};
        }
        Windows::UI::Color WithAlpha(Windows::UI::Color c, uint8_t a) { c.A = a; return c; }
    }

    static Windows::UI::Color AccentBaseColor(lycan::AccentTheme t)
    {
        int idx = static_cast<int>(t);
        if (idx >= 0 && idx < kAccentCount - 1) return Windows::UI::Color{255, kAccentDefs[idx].r, kAccentDefs[idx].g, kAccentDefs[idx].b};
        return Windows::UI::ViewManagement::UISettings{}.GetColorValue(Windows::UI::ViewManagement::UIColorType::Accent);
    }

    void MainWindow::UpdateBackdrop()
    {
        using namespace Microsoft::UI::Xaml::Media;
        const bool highContrast = lycan_app::HighContrast();
        if (highContrast != contrastBackdrop_)
        {   // a contrast look is solid: no Mica behind the window, plain black under the page
            contrastBackdrop_ = highContrast;
            if (highContrast) SystemBackdrop(nullptr); else SystemBackdrop(Microsoft::UI::Xaml::Media::MicaBackdrop{});
            RootGrid().Background(highContrast && lycan_app::ContrastThemeOn() ? Brush{SolidColorBrush{Windows::UI::Color{255, 0, 0, 0}}} : Brush{nullptr});
        }
        const bool off = settings_.background == lycan::BackgroundStyle::Off || highContrast;
        BackdropLayer().Visibility(off ? Visibility::Collapsed : Visibility::Visible);
        if (off) return;

        const bool dark = RootGrid().ActualTheme() != ElementTheme::Light;
        const auto base = AccentBaseColor(settings_.accent);
        Windows::UI::Color top, bottom, glow;
        if (dark && settings_.accent == lycan::AccentTheme::Night)
        {
            top = Windows::UI::Color{0xF4, 0x0B, 0x0D, 0x12};      // black night: almost fully black, with only a trace of cold colour
            bottom = Windows::UI::Color{0xF4, 0x03, 0x04, 0x06};
            glow = WithAlpha(MixColor(base, 255, 0.10), 0x26);
        }
        else if (dark)
        {
            top = MixColor(base, 0, 0.74, 0xF2);       // deep night: the accent colour darkened
            bottom = MixColor(base, 0, 0.94, 0xF2);
            glow = WithAlpha(MixColor(base, 255, 0.10), 0x58);
        }
        else
        {
            top = MixColor(base, 255, 0.93, 0xEE);     // morning light: the accent colour so pale it is almost white
            bottom = MixColor(base, 255, 0.80, 0xEE);
            glow = WithAlpha(MixColor(base, 255, 0.35), 0x50);
        }
        LinearGradientBrush fill;
        fill.StartPoint({0.2, 0.0});
        fill.EndPoint({0.8, 1.0});
        GradientStop s0, s1;
        s0.Color(top);
        s0.Offset(0.0);
        s1.Color(bottom);
        s1.Offset(1.0);
        fill.GradientStops().Append(s0);
        fill.GradientStops().Append(s1);
        BgFill().Fill(fill);

        // Glow of the accent colour at the top right
        RadialGradientBrush glowBrush;
        glowBrush.Center({0.85, 0.05});
        glowBrush.GradientOrigin({0.85, 0.05});
        glowBrush.RadiusX(0.75);
        glowBrush.RadiusY(0.75);
        GradientStop g0, g1;
        g0.Color(glow);
        g0.Offset(0.0);
        g1.Color(WithAlpha(glow, 0));
        g1.Offset(1.0);
        glowBrush.GradientStops().Append(g0);
        glowBrush.GradientStops().Append(g1);
        BgGlow().Fill(glowBrush);

        // Start page scene
        const bool scene = settings_.background == lycan::BackgroundStyle::Full && view_ == View::Home && !compact_ && !loading_;
        HomeScene().Visibility(scene ? Visibility::Visible : Visibility::Collapsed);
        const bool strip = settings_.background == lycan::BackgroundStyle::Full && view_ == View::Archive && !compact_ && !loading_;
        ArchiveScene().Visibility(strip ? Visibility::Visible : Visibility::Collapsed);
        if (strip)
        {
            try { UpdateArchiveStrip(dark, base); }
            catch (winrt::hresult_error const& e) { lycan::diag::Warn(L"archive strip: {} (0x{:08x})", std::wstring(e.message()), static_cast<uint32_t>(e.code().value)); }
        }
        if (!scene) return;

        const Windows::UI::Color moon = dark ? Windows::UI::Color{255, 214, 228, 255} : WithAlpha(MixColor(base, 255, 0.55), 255);
        RadialGradientBrush halo;
        halo.Center({0.5, 0.5});
        halo.GradientOrigin({0.5, 0.5});
        halo.RadiusX(0.5);
        halo.RadiusY(0.5);
        GradientStop h0, h1, h2;
        h0.Color(WithAlpha(moon, dark ? 0x46 : 0x55));
        h0.Offset(0.0);
        h1.Color(WithAlpha(moon, dark ? 0x1C : 0x26));
        h1.Offset(0.55);
        h2.Color(WithAlpha(moon, 0));
        h2.Offset(1.0);
        halo.GradientStops().Append(h0);
        halo.GradientStops().Append(h1);
        halo.GradientStops().Append(h2);
        MoonHalo().Fill(halo);

        // Milky way band: a very flat ellipse + radial gradient, with fully transparent edges (the end points of a rectangle would show hard edges)
        RadialGradientBrush band;
        band.Center({0.5, 0.5});
        band.GradientOrigin({0.5, 0.5});
        band.RadiusX(0.5);
        band.RadiusY(0.5);
        GradientStop b0, b1, b2;
        b0.Color(WithAlpha(MixColor(base, 255, 0.7), dark ? 0x26 : 0));
        b0.Offset(0.0);
        b1.Color(WithAlpha(MixColor(base, 255, 0.7), dark ? 0x10 : 0));
        b1.Offset(0.55);
        b2.Color(WithAlpha(MixColor(base, 255, 0.7), 0));
        b2.Offset(1.0);
        band.GradientStops().Append(b0);
        band.GradientStops().Append(b1);
        band.GradientStops().Append(b2);
        MilkyBand().Fill(band);
        MilkyBand().Visibility(dark ? Visibility::Visible : Visibility::Collapsed);

        // Wolf shadow: in the dark theme a darker silhouette with a rim light in the accent colour; in the light theme a faint blue-grey
        WolfShade().Fill(SolidColorBrush{dark ? WithAlpha(MixColor(base, 0, 0.96), 0xE0) : WithAlpha(MixColor(base, 0, 0.55), 0x22)});
        WolfShade().Stroke(SolidColorBrush{dark ? WithAlpha(MixColor(base, 255, 0.15), 0x55) : WithAlpha(base, 0x30)});
        WolfShade().StrokeThickness(dark ? 3.5 : 2.5);
        WolfShade().Opacity(dark ? 0.95 : 1.0);

        // Starry sky: few stars, in different sizes, with a soft glow (many small dots of the same size look like a dirty screen).
        // Positions are sampled to keep a distance from each other so they do not clump together; a few are four-pointed flashes, the rest are soft round dots.
        if (!starsBuilt_)
        {
            starsBuilt_ = true;
            uint32_t seed = 7721005;
            auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
            std::vector<std::pair<double, double>> pts;
            for (int tries = 0; tries < 600 && pts.size() < 24; ++tries)
            {
                double x = rnd(), y = rnd() * 0.74;
                bool ok = true;
                for (auto const& q : pts) if (std::hypot(x - q.first, (y - q.second) * 1.4) < 0.11) { ok = false; break; }
                if (ok) pts.push_back({x, y});
            }
            auto addStar = [&](double size, bool sparkle, double opacity) -> double
            {
                Grid g;
                g.Width(size * (sparkle ? 2.0 : 1.0));
                g.Height(size * (sparkle ? 2.0 : 1.0));
                // soft glow
                RadialGradientBrush glow;
                glow.Center({0.5, 0.5});
                glow.GradientOrigin({0.5, 0.5});
                glow.RadiusX(0.5);
                glow.RadiusY(0.5);
                GradientStop g0, g1;
                g0.Color(Windows::UI::Color{static_cast<uint8_t>(sparkle ? 0x70 : 0xB0), 214, 228, 255});
                g0.Offset(0.0);
                g1.Color(Windows::UI::Color{0, 214, 228, 255});
                g1.Offset(1.0);
                glow.GradientStops().Append(g0);
                glow.GradientStops().Append(g1);
                Microsoft::UI::Xaml::Shapes::Ellipse halo;
                halo.Fill(glow);
                g.Children().Append(halo);
                if (sparkle)
                {
                    // four-pointed star
                    Microsoft::UI::Xaml::Shapes::Path star;
                    const double r = size, k = size * 0.14;
                    // The coordinates of a Path start at (0,0) and negative coordinates are cut off: shift the whole shape by +r and set the element size to 2r, then it is centred
                    auto n = [](double v) { return std::to_wstring(v); };
                    std::wstring d = L"M" + n(r) + L",0 L" + n(r + k) + L"," + n(r - k) + L" L" + n(2 * r) + L"," + n(r) + L" L" + n(r + k) + L"," + n(r + k) +
                                     L" L" + n(r) + L"," + n(2 * r) + L" L" + n(r - k) + L"," + n(r + k) + L" L0," + n(r) + L" L" + n(r - k) + L"," + n(r - k) + L" Z";
                    star.Width(2 * r);
                    star.Height(2 * r);
                    star.Data(Microsoft::UI::Xaml::Markup::XamlBindingHelper::ConvertValue(xaml_typename<Media::Geometry>(), box_value(hstring{d})).as<Media::Geometry>());
                    star.Fill(SolidColorBrush{Windows::UI::Color{0xF2, 245, 249, 255}});
                    star.HorizontalAlignment(HorizontalAlignment::Center);
                    star.VerticalAlignment(VerticalAlignment::Center);
                    g.Children().Append(star);
                }
                g.Opacity(opacity);
                Stars().Children().Append(g);
                return g.Width();
            };
            // the small soft stars (random places)
            for (size_t i = 0; i < pts.size(); ++i)
            {
                const double size = 5 + rnd() * 5;
                starFrac_.push_back(pts[i]);
                starSize_.push_back(addStar(size, false, 0.35 + rnd() * 0.45));
            }
            // The big four-pointed stars draw Canis Major (the Greater Dog; Sirius is its brightest star), as in the usual
            // picture: the dog stands on its hind legs, the head at the upper right, the tail at the lower left. The
            // places are the stars of that picture (x 55..381, y 100..441), scaled to 0..1; the size follows the brightness.
            struct Pt { double x, y, size; };
            static const Pt kCanisMajor[] = {
                {320, 100, 14},   // head
                {250, 133, 20},   // the star behind the head
                {318, 185, 34},   // Sirius
                {282, 207, 11},
                {316, 247, 15},   // the raised foreleg
                {381, 260, 14},   // its paw
                {183, 262, 13},   // back
                {113, 284, 20},   // hip
                {55, 308, 14},    // tail
                {285, 303, 11},
                {150, 345, 15},   // hind leg
                {148, 394, 11},
                {264, 441, 14},   // hind foot
            };
            for (auto const& c : kCanisMajor)
                conStars_.push_back({(c.x - 55) / (381.0 - 55), (c.y - 100) / (441.0 - 100), addStar(c.size, true, 0.95)});
        }
        Stars().Visibility(dark ? Visibility::Visible : Visibility::Collapsed);  // the light theme has no stars
        LayoutHomeScene();
        LayoutArchiveStrip();
        UpdateToolbarText();
    }

    // The strip at the top of the archive view: some soft stars and the wolf, both fade out downwards (a few dozen pixels, so cheap)
    void MainWindow::UpdateArchiveStrip(bool dark, Windows::UI::Color base)
    {
        using namespace Microsoft::UI::Xaml::Media;
        if (!archiveStarsBuilt_)
        {
            archiveStarsBuilt_ = true;
            uint32_t seed = 4410217;
            auto rnd = [&seed]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.0; };
            for (int i = 0; i < 12; ++i)
            {
                // the same four-pointed stars as the home page, small, with a soft glow
                const double r = 4 + rnd() * 4;
                Grid g;
                g.Width(2 * r);
                g.Height(2 * r);
                RadialGradientBrush glow;
                glow.Center({0.5, 0.5});
                glow.GradientOrigin({0.5, 0.5});
                glow.RadiusX(0.5);
                glow.RadiusY(0.5);
                GradientStop g0, g1;
                g0.Color(Windows::UI::Color{0x70, 214, 228, 255});
                g0.Offset(0.0);
                g1.Color(Windows::UI::Color{0, 214, 228, 255});
                g1.Offset(1.0);
                glow.GradientStops().Append(g0);
                glow.GradientStops().Append(g1);
                Microsoft::UI::Xaml::Shapes::Ellipse halo;
                halo.Fill(glow);
                g.Children().Append(halo);
                Microsoft::UI::Xaml::Shapes::Path star;
                const double k = r * 0.14;
                auto n = [](double v) { return std::to_wstring(v); };
                const std::wstring d = L"M" + n(r) + L",0 L" + n(r + k) + L"," + n(r - k) + L" L" + n(2 * r) + L"," + n(r) + L" L" + n(r + k) + L"," + n(r + k) +
                                       L" L" + n(r) + L"," + n(2 * r) + L" L" + n(r - k) + L"," + n(r + k) + L" L0," + n(r) + L" L" + n(r - k) + L"," + n(r - k) + L" Z";
                star.Width(2 * r);
                star.Height(2 * r);
                star.Data(Microsoft::UI::Xaml::Markup::XamlBindingHelper::ConvertValue(xaml_typename<Media::Geometry>(), box_value(hstring{d})).as<Media::Geometry>());
                star.Fill(SolidColorBrush{Windows::UI::Color{0xF2, 245, 249, 255}});
                star.HorizontalAlignment(HorizontalAlignment::Center);
                star.VerticalAlignment(VerticalAlignment::Center);
                g.Children().Append(star);
                ArchiveStars().Children().Append(g);
                archiveStarFrac_.push_back({(i + 0.15 + rnd() * 0.7) / 12.0, rnd()});  // x spread over the width, y 0..1 of the strip
                archiveStarSize_.push_back(2 * r);
            }
        }
        ArchiveStars().Visibility(dark ? Visibility::Visible : Visibility::Collapsed);  // no stars in the light theme

        // the wolf: the fill fades out from the top to the bottom
        LinearGradientBrush fill;
        fill.StartPoint({0.5, 0.0});
        fill.EndPoint({0.5, 1.0});
        GradientStop a, b;
        a.Color(dark ? WithAlpha(MixColor(base, 0, 0.9), 0x70) : WithAlpha(MixColor(base, 0, 0.55), 0x30));
        a.Offset(0.0);
        b.Color(dark ? WithAlpha(MixColor(base, 0, 0.9), 0) : WithAlpha(MixColor(base, 0, 0.55), 0));
        b.Offset(1.0);
        fill.GradientStops().Append(a);
        fill.GradientStops().Append(b);
        WolfStrip().Fill(fill);
        WolfStrip().Stroke(nullptr);
        LayoutArchiveStrip();
    }

    // The buttons of the first row of the archive view: when the window is too narrow the texts of the less important ones go first
    // (icons stay; the names for the screen reader and the tips stay), then the text of "Extract here" as well.
    void MainWindow::UpdateToolbarText()
    {
        if (view_ != View::Archive) return;
        const double available = ArchivePanel().ActualWidth();
        if (available < 1) return;
        auto texts = [&](bool secondary, bool primary) {
            const Visibility v2 = secondary ? Visibility::Visible : Visibility::Collapsed;
            ExtractToText().Visibility(v2);
            AddText().Visibility(v2);
            PreviewText().Visibility(v2);
            ViewText().Visibility(v2);
            ExtractHereText().Visibility(primary ? Visibility::Visible : Visibility::Collapsed);
        };
        auto fits = [&] {
            ArchiveToolRow().Measure(Windows::Foundation::Size{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()});
            return ArchiveToolRow().DesiredSize().Width <= available;
        };
        texts(true, true);
        bool iconsOnly = false;
        if (!fits()) { texts(false, true); iconsOnly = true; }
        if (iconsOnly && !fits()) texts(false, false);
        if (iconsOnly != toolbarIconsOnly_ || iconsOnly)
        {
            toolbarIconsOnly_ = iconsOnly;
            // the tips say what the icons mean
            auto tip = [&](Control const& b, wchar_t const* id) { ToolTipService::SetToolTip(b, iconsOnly ? box_value(hstring{lycan::Tr(id)}) : nullptr); };
            tip(ExtractToButton(), L"main.extract");
            tip(PreviewButton(), L"main.preview");
            tip(ViewButton(), L"main.view");
            tip(ExtractHereButton(), L"main.extract_here");
        }
    }

    void MainWindow::LayoutArchiveStrip()
    {
        if (ArchiveScene().Visibility() != Visibility::Visible) return;
        const double w = RootGrid().ActualWidth();
        if (w < 1) return;
        const double wolfLeft = w - 28 - 96 * 1.073;   // the wolf is 1.073 times as wide as high
        for (uint32_t i = 0; i < ArchiveStars().Children().Size() && i < archiveStarFrac_.size(); ++i)
        {
            auto star = ArchiveStars().Children().GetAt(i).as<UIElement>();
            const double x = archiveStarFrac_[i].first * w, y = 4 + archiveStarFrac_[i].second * 100;
            Canvas::SetLeft(star, x - archiveStarSize_[i] / 2);
            Canvas::SetTop(star, y - archiveStarSize_[i] / 2);
            star.Opacity((1.0 - y / 120.0) * 0.9);                       // fades out downwards
            star.Visibility(x > wolfLeft - 10 ? Visibility::Collapsed : Visibility::Visible);  // none over the wolf
        }
    }

    // Lays out the halo, stars and wolf shadow by the window size
    void MainWindow::LayoutHomeScene()
    {
        if (HomeScene().Visibility() != Visibility::Visible) return;
        const double w = RootGrid().ActualWidth(), h = RootGrid().ActualHeight();
        if (w < 1 || h < 1) return;
        const double wolfH = h * 0.62;
        WolfShade().Height(wolfH);
        WolfShade().Margin(ThicknessHelper::FromLengths(0, 0, w * 0.02, h * 0.04));
        const double halo = std::max(w, h) * 0.78;
        MoonHalo().Width(halo);
        MoonHalo().Height(halo);
        // the center of the halo points at the wolf's head (right side, a little up)
        MoonHalo().Margin(ThicknessHelper::FromLengths(w * 0.74 - halo / 2, h * 0.52 - halo / 2, 0, 0));
        // Where the wolf is (its shape is about 1.065 times as wide as high, see WolfShade): no star is drawn over it
        const double wolfRight = w * 0.98, wolfBottom = h * 0.96;
        const double wolfLeft = wolfRight - wolfH * 1.065, wolfTop = wolfBottom - wolfH;
        const double pad = std::max(w, h) * 0.015;
        auto onWolf = [&](double x, double y, double r) {
            return x + r > wolfLeft - pad && x - r < wolfRight && y + r > wolfTop - pad && y - r < wolfBottom;
        };
        for (uint32_t i = 0; i < Stars().Children().Size() && i < starFrac_.size() && i < starSize_.size(); ++i)
        {
            auto star = Stars().Children().GetAt(i).as<UIElement>();
            const double x = starFrac_[i].first * w, y = starFrac_[i].second * h;
            Canvas::SetLeft(star, x - starSize_[i] / 2);
            Canvas::SetTop(star, y - starSize_[i] / 2);
            star.Visibility(onWolf(x, y, starSize_[i] / 2) ? Visibility::Collapsed : Visibility::Visible);
        }
        // Canis Major: in the upper left, away from the wolf (it shrinks when the window is narrow)
        {
            double boxH = h * 0.40;
            const double ratio = (381.0 - 55) / (441.0 - 100);
            const double left = w * 0.05, top = h * 0.13;
            if (left + boxH * ratio > wolfLeft - pad) boxH = std::max(h * 0.18, std::min(boxH, wolfTop - pad - top));  // beside the wolf's head: stay above it
            boxH = std::min(boxH, (w * 0.5 - left) / ratio);                                                          // and never wider than half of the window
            const double boxW = boxH * ratio;
            const size_t first = starFrac_.size();
            for (size_t k = 0; k < conStars_.size() && first + k < Stars().Children().Size(); ++k)
            {
                auto star = Stars().Children().GetAt(static_cast<uint32_t>(first + k)).as<UIElement>();
                Canvas::SetLeft(star, left + conStars_[k].rx * boxW - conStars_[k].size / 2);
                Canvas::SetTop(star, top + conStars_[k].ry * boxH - conStars_[k].size / 2);
            }
        }
        // A faint milky way band: one soft bright band at an angle (dark theme only)
        // The element size must not exceed the layout cell: if it does, XAML clips to the cell and after the rotation it becomes a hard edge
        MilkyBand().Width(w * 0.98);
        MilkyBand().Height(h * 0.40);
        MilkyBand().Margin(ThicknessHelper::FromLengths(0, h * 0.12, 0, 0));
    }

    void MainWindow::BuildAccentPicker()
    {
        auto nameOf = [](int i) { return i < kAccentCount - 1 ? hstring{lycan::Tr(kAccentDefs[i].name)} : hstring{lycan::Tr(L"main.follow_system")}; };
        // the cells of the grid all get the width of the first one: make it as wide as the longest name (in German "Follow system" is long)
        double widest = 0;
        for (int i = 0; i < kAccentCount; ++i)
        {
            TextBlock probe;
            probe.Text(nameOf(i));
            probe.Measure(Windows::Foundation::Size{10000, 10000});
            widest = (std::max)(widest, static_cast<double>(probe.DesiredSize().Width));
        }
        for (int i = 0; i < kAccentCount; ++i)
        {
            StackPanel panel;
            panel.Orientation(Orientation::Horizontal);
            panel.Spacing(8);
            panel.Padding(ThicknessHelper::FromLengths(6, 4, 16, 4));
            panel.MinWidth(std::ceil(6 + 22 + 8 + widest + 16 + 4));  // padding, dot, spacing, name, padding, rounding
            Microsoft::UI::Xaml::Shapes::Ellipse dot;
            dot.Width(22);
            dot.Height(22);
            if (i < kAccentCount - 1)
            {
                auto const& a = kAccentDefs[i];
                dot.Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, a.r, a.g, a.b}});
            }
            else
            {
                dot.Fill(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, 120, 120, 128}});
                dot.StrokeThickness(2);
                dot.Stroke(Microsoft::UI::Xaml::Media::SolidColorBrush{Windows::UI::Color{255, 220, 220, 230}});
            }
            TextBlock label;
            label.Text(nameOf(i));
            label.VerticalAlignment(VerticalAlignment::Center);
            panel.Children().Append(dot);
            panel.Children().Append(label);
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(panel, label.Text());
            AccentGrid().Items().Append(panel);
        }
    }

    void MainWindow::OnAccentChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        if (loadingSettings_) return;
        int i = AccentGrid().SelectedIndex();
        if (i < 0) return;
        settings_.accent = static_cast<lycan::AccentTheme>(i);
        SaveSettings();
        ApplyAccent();
    }

    // When the default view setting changes, apply it to the archive that is open now too
    void MainWindow::ApplyViewSettings()
    {
        if (!tree_ || loadingSettings_) return;
        bool changed = flat_ != settings_.browseFlat || viewMode_ != settings_.viewMode;
        flat_ = settings_.browseFlat;
        viewMode_ = settings_.viewMode;
        treeWanted_ = settings_.showFolderTree;
        syncingUi_ = true;
        FlatToggle().IsChecked(flat_);
        TreeToggle().IsChecked(treeWanted_);
        syncingUi_ = false;
        UpdateTreeVisibility();
        if (changed) RefreshList();
    }

    void MainWindow::OnSettingComboChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&) { ReadSettingsFromUi(); }
    void MainWindow::OnSettingToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { ReadSettingsFromUi(); }
    void MainWindow::OnSettingNumberChanged(NumberBox const&, NumberBoxValueChangedEventArgs const&) { ReadSettingsFromUi(); }

    void MainWindow::OnSettingsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (view_ == View::Settings) { OnSettingsBackClick(nullptr, nullptr); return; }
        viewBeforeSettings_ = view_;
        std::wstring engine = lycan::Tr(L"main.compression_engine_7_zip");
        try
        {
            auto dll = lycan::SevenZipDllPath();
            auto v = FileVersionOf(dll);
            engine += (v.empty() ? L"" : L" " + v) + lycan::Tf(L"fmt.paren", dll.wstring());
        }
        catch (...) { engine += lycan::Tr(L"main.7z_dll_not_found"); }
        AboutVersion().Text(L"LycanZip " LYCAN_VERSION_WSTRING);
        AboutEngine().Text(hstring{engine});
        AboutSettingsPath().Text(hstring{lycan::Tf(L"main.settings_file", settingsPath_.wstring())});
        loadingSettings_ = true;
        ShellMenuToggle().IsOn(lycan::IsShellMenuRegistered(ShellOptions()));
        FileAssocToggle().IsOn(lycan::IsFileAssociationsRegistered(AssocOptions()));
        loadingSettings_ = false;
        RebuildShellPresetRows();
        RefreshDataLocationUi();
        RefreshFileAssocStatus();
        RefreshWin11MenuAsync();
        ShowView(View::Settings);
    }

    // Tests (LYCAN_TEST_REG_ROOT=Software\\LycanZipTest_x) keep the menu and file association registrations under a key of their own
    static std::wstring TestRegistryRoot()
    {
        wchar_t buf[256];
        const DWORD n = GetEnvironmentVariableW(L"LYCAN_TEST_REG_ROOT", buf, static_cast<DWORD>(std::size(buf)));
        return n > 0 && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
    }

    lycan::FileAssocOptions MainWindow::AssocOptions()
    {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        lycan::FileAssocOptions o;
        o.exe = std::wstring(exe, n);
        if (const std::wstring root = TestRegistryRoot(); !root.empty()) { o.classesKey = root + L"\\Classes"; o.softwareKey = root + L"\\Software"; o.notifyShell = false; }
        return o;
    }

    // Shows which program opens a few common extensions now (read-only query)
    // The Store version (a package with an identity): the manifest declares the Windows 11 menu and the file types, so they are always on and cannot be
    // switched; the registry of a packaged program is virtualized, so the classic menu cannot be registered from it either. (Tests: LYCAN_TEST_STORE_UI=1)
    void MainWindow::ApplyStoreChannelUi()
    {
        if (!IsStoreUi())
        {
            // another copy while the Store version is installed: the menu and the file types are the Store version's, these switches stay off
            if (!lycan::IsStorePackageInstalled()) { OtherCopyNote().Visibility(Visibility::Collapsed); return; }
            OtherCopyNoteText().Text(lycan::Tr(L"main.store_version_provides_menu"));
            OtherCopyNote().Visibility(Visibility::Visible);
            const bool was = loadingSettings_;
            loadingSettings_ = true;
            for (auto t : {ShellMenuToggle(), FileAssocToggle(), Win11MenuToggle()}) { t.IsOn(false); t.IsEnabled(false); }
            loadingSettings_ = was;
            return;
        }
        const std::wstring other = OtherCopyLocation();
        OtherCopyNoteText().Text(hstring{lycan::Tf(L"main.other_copy_note", other)});
        OtherCopyNote().Visibility(other.empty() ? Visibility::Collapsed : Visibility::Visible);
        StoreExplorerNote().Visibility(Visibility::Visible);
        UpdatesHeader().Visibility(Visibility::Collapsed);   // the Store updates the program: no check for new versions here
        UpdatesCard().Visibility(Visibility::Collapsed);
        // The menus and the file types are part of the package (Windows adds them at install and removes them at uninstall): nothing to register,
        // so the cards that register are hidden. The menu switch stays: it is a setting that the menu DLL reads (shell_menu), and the DLL then shows
        // nothing (the new Windows 11 menu, Show more options, the classic menu of Windows 10, the right-drag menu). Explorer reads it live.
        FileAssocCard().Visibility(Visibility::Collapsed);
        Win11MenuCard().Visibility(Visibility::Collapsed);
        RestartExplorerCard().Visibility(Visibility::Collapsed);
        ShellMenuHint().Text(lycan::Tr(L"main.store_menu_switch_hint"));
        const bool was = loadingSettings_;
        loadingSettings_ = true;
        ShellMenuToggle().IsEnabled(true);
        ShellMenuToggle().IsOn(settings_.shellMenu);
        loadingSettings_ = was;
        OpenDefaultAppsButton().IsEnabled(true);
    }

    bool MainWindow::IsStoreUi()
    {
        return lycan::DetectChannel() == lycan::InstallChannel::Store || GetEnvironmentVariableW(L"LYCAN_TEST_STORE_UI", nullptr, 0) > 0;
    }

    // A copy that is not the Store version, while the Store version is installed: remove its own right-click menu and file associations (they are
    // LycanZip's keys under HKCU; the Store version's are in its package), so that File Explorer shows LycanZip once. Returns true when the Store
    // version is installed. The Windows 11 menu package of such a copy is removed by CheckInstalledCopiesAsync (the package manager is slow).
    bool MainWindow::StepAsideForStore()
    {
        if (lycan::GetDataLocation().packaged || !lycan::IsStorePackageInstalled()) return false;
        std::wstring error;
        if (lycan::IsShellMenuRegistered(ShellOptions()))
        {
            if (lycan::UnregisterShellMenu(ShellOptions(), &error)) lycan::diag::Info(L"the Store version is installed: the right-click menu of this copy was removed");
            else lycan::diag::Warn(L"the Store version is installed, the right-click menu of this copy could not be removed: {}", error);
        }
        if (lycan::IsFileAssociationsRegistered(AssocOptions()))
        {
            if (lycan::UnregisterFileAssociations(AssocOptions(), &error)) lycan::diag::Info(L"the Store version is installed: the file associations of this copy were removed");
            else lycan::diag::Warn(L"the Store version is installed, the file associations of this copy could not be removed: {}", error);
        }
        return true;
    }

    // The Store version: where another copy is that still has the classic right-click menu or the file associations registered (empty: none). A package
    // reads the real HKCU but writes to a private copy of it: what an earlier Store version registered itself (pointing into WindowsApps) is only in
    // that copy, and is removed here instead of being reported.
    std::wstring MainWindow::OtherCopyLocation()
    {
        auto inPackage = [](fs::path const& p) {
            std::wstring s = p.wstring();
            std::transform(s.begin(), s.end(), s.begin(), ::towlower);
            return s.find(L"\\windowsapps\\") != std::wstring::npos;
        };
        std::wstring where;
        if (lycan::IsShellMenuRegistered(ShellOptions()))
        {
            const fs::path dll = lycan::RegisteredShellDll(ShellOptions());
            if (inPackage(dll)) lycan::UnregisterShellMenu(ShellOptions());
            else
            {
                // an installed copy keeps the DLL outside the folder of the program and writes where the program is next to it (lycan::EnsureShellHome)
                std::ifstream f(dll.parent_path() / L"LycanZip.exepath", std::ios::binary);
                std::string utf8((std::istreambuf_iterator<char>(f)), {});
                while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' ')) utf8.pop_back();
                where = utf8.empty() ? dll.parent_path().wstring() : fs::path(std::wstring{winrt::to_hstring(utf8)}).parent_path().wstring();
            }
        }
        if (lycan::IsFileAssociationsRegistered(AssocOptions()))
        {
            const fs::path exe = lycan::RegisteredAssocExe(AssocOptions());
            if (inPackage(exe)) lycan::UnregisterFileAssociations(AssocOptions());
            else if (where.empty()) where = exe.parent_path().wstring();
        }
        return where;
    }

    winrt::Windows::Foundation::IAsyncAction MainWindow::TwoCopiesDialogAsync(std::wstring text)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.two_copies_title"), L"");
        TextBlock tb;
        tb.Text(hstring{text});
        tb.TextWrapping(TextWrapping::Wrap);
        tb.IsTextSelectionEnabled(true);
        tb.MaxWidth(460);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.open_installed_apps"));
        dlg.CloseButtonText(lycan::Tr(L"main.ok"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary)
            ShellExecuteW(nullptr, L"open", L"ms-settings:appsfeatures", nullptr, nullptr, SW_SHOWNORMAL);
    }

    // The program was started on its own: two copies on this PC (said once), else the registrations that point to another folder
    winrt::fire_and_forget MainWindow::CheckInstalledCopiesAsync()
    {
        auto strong = get_strong();
        if (GetEnvironmentVariableW(L"LYCAN_NO_STALE_CHECK", nullptr, 0) > 0) co_return;  // tests: the real registrations belong to other builds
        const lycan::DataLocation loc = lycan::GetDataLocation();
        if (loc.packaged)
        {
            const std::wstring other = OtherCopyLocation();
            if (other.empty() || settings_.twoCopiesNoticeShown) co_return;
            settings_.twoCopiesNoticeShown = true;
            SaveSettings();
            co_await TwoCopiesDialogAsync(lycan::Tf(L"main.two_copies_other_has_menu", other));
            co_return;
        }
        if (!StepAsideForStore()) { CheckStaleRegistrationsAsync(); co_return; }
        if (TestRegistryRoot().empty())   // the Windows 11 menu package of a copy like this one (the package is not part of the test registry key)
        {
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            const lycan_app::Win11MenuRegistration w11 = lycan_app::QueryWin11MenuRegistration();
            if (w11.found)
            {
                const hstring failure = co_await lycan_app::UnregisterWin11MenuAsync();
                if (failure.empty()) lycan::diag::Info(L"the Store version is installed: the Windows 11 menu package of this copy was removed");
                else lycan::diag::Warn(L"the Store version is installed, the Windows 11 menu package could not be removed: {}", std::wstring{failure});
            }
            co_await ui;
        }
        RefreshFileAssocStatus();
        if (settings_.twoCopiesNoticeShown) co_return;
        settings_.twoCopiesNoticeShown = true;
        SaveSettings();
        co_await TwoCopiesDialogAsync(lycan::Tf(L"main.two_copies_store_owns", loc.exeDir.wstring()));
    }

    void MainWindow::RefreshFileAssocStatus()
    {
        // the Store version: the file types are part of the package, always registered (its HKCU view does not show them)
        bool registered = IsStoreUi() || lycan::IsFileAssociationsRegistered(AssocOptions());
        std::wstring text;
        for (auto ext : {L".zip", L".7z", L".rar", L".tar.gz"})
        {
            std::wstring e = ext;
            if (e == L".tar.gz") e = L".gz";  // the association follows the last extension
            std::wstring name = lycan::CurrentDefaultAppName(e.c_str());
            if (!text.empty()) text += L"\n";
            text += L"• " + std::wstring(ext) + std::wstring{lycan::Tr(L"fmt.gap")} + (name.empty() ? lycan::Tr(L"main.not_set") : name);
            if (lycan::IsLycanZipDefaultFor(e.c_str())) text += lycan::Tr(L"main.already_lycanzip");
        }
        if (!registered) text += lycan::Tr(L"main.not_registered_yet_turn_on_switch");
        AssocStatus().Text(hstring{text});
        OpenDefaultAppsButton().IsEnabled(registered);
        ApplyStoreChannelUi();
    }

    void MainWindow::OnFileAssocToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        bool want = FileAssocToggle().IsOn();
        std::wstring error;
        bool ok = want ? lycan::RegisterFileAssociations(AssocOptions(), &error) : lycan::UnregisterFileAssociations(AssocOptions(), &error);
        if (!ok)
        {
            loadingSettings_ = true;
            FileAssocToggle().IsOn(!want);
            loadingSettings_ = false;
            ShowMessage(lycan::Tr(L"main.cannot_update_file_associations"), hstring{error});
        }
        RefreshFileAssocStatus();
        if (ok && want)
            ShowMessage(lycan::Tr(L"main.registered"), lycan::Tr(L"main.lycanzip_can_now_handle_archives"));
    }

    void MainWindow::OnOpenDefaultAppsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!lycan::OpenDefaultAppsSettings()) ShowMessage(lycan::Tr(L"main.cannot_open_settings"), lycan::Tr(L"main.please_open_settings_apps_default"));
    }

    lycan::ShellRegOptions MainWindow::ShellOptions()
    {
        wchar_t exe[MAX_PATH * 2];
        DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        lycan::ShellRegOptions o;
        o.exe = std::wstring(exe, n);
        if (const std::wstring root = TestRegistryRoot(); !root.empty()) o.classesKey = root + L"\\Classes";
        return o;
    }

    // ---- Which entries the context menu shows (settings page, "Explorer" tab). The menu reads the setting itself, so
    // nothing is re-registered: a change shows the next time the menu opens.

    namespace
    {
        struct ShellItemLabel { const char* id; std::wstring label; };
        // The text of each entry (translated); the order is the order of the menu
        std::vector<ShellItemLabel> FileItems()
        {
            return {{"add", lycan::Tr(L"main.menu_add_to_archive")}, {"fmt1", lycan::Tr(L"main.compress_name_format")}, {"fmt2", lycan::Tr(L"main.compress_name_format")},
                    {"each", lycan::Tr(L"main.compress_each_separately_when")}, {"template", lycan::Tr(L"main.compress_with_preset")},
                    {"more", lycan::Tr(L"main.other_formats_tar_gz_tar_xz")}, {"encrypt", lycan::Tr(L"main.encrypted_archive")},
                    {"opencontent", lycan::Tr(L"main.open_contents_with_lycanzip_exe")},
                    {"hash", lycan::Tr(L"main.hash_values_menu_files_archives")}, {"hashsave", lycan::Tr(L"sums.menu_hash_save")}};
        }
        std::vector<ShellItemLabel> ArchiveItems()
        {
            return {{"open", lycan::Tr(L"main.open")}, {"here", lycan::Tr(L"main.menu_extract_here")}, {"folder", lycan::Tr(L"main.extract_folders_same_name")},
                    {"to", lycan::Tr(L"main.extract")}, {"recent", lycan::Tr(L"main.extract_recent_folder")},
                    {"merge", lycan::Tr(L"main.extract_all_into_one_folder_when")}, {"test", lycan::Tr(L"main.test_integrity")},
                    {"convert", lycan::Tr(L"main.convert_archive_format")},
                    {"images", lycan::Tr(L"main.image_preview")}, {"audio", lycan::Tr(L"main.audio_preview")}};
        }
        const lycan::ArchiveType kShellFormats[] = {lycan::ArchiveType::Zip, lycan::ArchiveType::SevenZip, lycan::ArchiveType::TarGz,
                                                    lycan::ArchiveType::TarXz, lycan::ArchiveType::TarBz2};
    }

    void MainWindow::BuildShellItemsUi()
    {
        auto panel = ShellItemsPanel();
        panel.Children().Clear();
        shellChecks_.clear();
        auto weak = get_weak();

        auto title = TextBlock{};
        title.Text(lycan::Tr(L"main.context_menu_entries"));
        title.Style(Application::Current().Resources().Lookup(box_value(L"BodyStrongTextBlockStyle")).as<Style>());
        title.Margin(ThicknessHelper::FromLengths(0, 16, 0, 0));
        panel.Children().Append(title);
        panel.Children().Append(Caption(lycan::Tr(L"main.choose_which_entries_shown_change"), 0.65));

        auto makeFormatCombo = [&](bool second) {
            ComboBox combo;
            combo.MinWidth(110);
            for (auto t : kShellFormats)
            {
                ComboBoxItem item;
                item.Content(box_value(hstring{t == lycan::ArchiveType::Zip ? L"ZIP" : lycan::FormatName(t)}));
                combo.Items().Append(item);
            }
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(combo, second ? lycan::Tr(L"main.format_second_compress_entry") : lycan::Tr(L"main.format_first_compress_entry"));
            combo.SelectionChanged([weak, second](auto&&, auto&&) {
                auto s = weak.get();
                if (!s || s->loadingSettings_) return;
                auto& combo = second ? s->shellFmt2_ : s->shellFmt1_;
                const int i = combo.SelectedIndex();
                if (i < 0 || i >= static_cast<int>(std::size(kShellFormats))) return;
                (second ? s->settings_.shellFormat2 : s->settings_.shellFormat1) = kShellFormats[i];
                s->SaveSettings();
            });
            return combo;
        };

        auto addGroup = [&](const wchar_t* heading, std::vector<ShellItemLabel> const& items) {
            panel.Children().Append(Caption(heading, 0.75));
            StackPanel rows;
            rows.Spacing(2);
            for (auto const& item : items)
            {
                const std::string id = item.id;
                Grid row;
                row.ColumnSpacing(12);
                ColumnDefinition c0, c1;
                c0.Width(GridLength{1, GridUnitType::Star});
                c1.Width(GridLength{0, GridUnitType::Auto});
                row.ColumnDefinitions().Append(c0);
                row.ColumnDefinitions().Append(c1);
                CheckBox box;
                const bool isFormat = id == "fmt1" || id == "fmt2";
                box.Content(box_value(hstring{item.label}));
                Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, id == "fmt1" ? lycan::Tr(L"main.compress_first_format") : id == "fmt2" ? lycan::Tr(L"main.compress_second_format") : item.label);
                box.Click([weak, id](IInspectable const& sender, RoutedEventArgs const&) {
                    auto s = weak.get();
                    if (!s || s->loadingSettings_) return;
                    s->settings_.shellItems = lycan::ShellItemsWith(s->settings_.shellItems, id, sender.as<CheckBox>().IsChecked().Value());
                    s->SaveSettings();
                });
                row.Children().Append(box);
                shellChecks_.emplace_back(id, box);
                if (isFormat)
                {
                    ComboBox combo = makeFormatCombo(id == "fmt2");
                    (id == "fmt2" ? shellFmt2_ : shellFmt1_) = combo;
                    Grid::SetColumn(combo, 1);
                    row.Children().Append(combo);
                }
                rows.Children().Append(row);
            }
            Border card;
            card.Style(RootGrid().Resources().Lookup(box_value(L"SettingCard")).as<Style>());
            card.Child(rows);
            panel.Children().Append(card);
        };
        addGroup(lycan::Tr(L"main.files_folders"), FileItems());
        addGroup(lycan::Tr(L"main.archives"), ArchiveItems());
        {
            panel.Children().Append(Caption(lycan::Tr(L"main.templates_compress_with_template"), 0.75));
            shellPresetRows_ = StackPanel{};
            shellPresetRows_.Spacing(2);
            Border card;
            card.Style(RootGrid().Resources().Lookup(box_value(L"SettingCard")).as<Style>());
            card.Child(shellPresetRows_);
            panel.Children().Append(card);
            RebuildShellPresetRows();
        }
        UpdateShellItemsUi();
    }

    // Is the template listed in the Explorer menu "Compress with a preset"? (the built-in ones: settings.ini; the others: presets.ini)
    bool MainWindow::PresetInMenu(std::wstring const& name, bool builtin)
    {
        if (builtin)
        {
            for (size_t i = 0; i < lycan::kBuiltinPresetCount; ++i)
                if (name == lycan::kBuiltinPresetNames[i]) return lycan::BuiltinPresetEnabled(settings_.shellPresets, i);
            return false;
        }
        for (auto const& u : cUser_) if (u.name == name) return u.inMenu;
        for (auto const& u : lycan::LoadPresets(lycan::DefaultPresetsPath())) if (u.name == name) return u.inMenu;
        return true;
    }

    void MainWindow::SetPresetInMenu(std::wstring const& name, bool builtin, bool on)
    {
        if (builtin)
        {
            for (size_t i = 0; i < lycan::kBuiltinPresetCount; ++i)
                if (name == lycan::kBuiltinPresetNames[i]) settings_.shellPresets = lycan::BuiltinPresetsWith(settings_.shellPresets, i, on);
            SaveSettings();
            return;
        }
        auto list = lycan::LoadPresets(lycan::DefaultPresetsPath());
        for (auto& u : list) if (u.name == name) u.inMenu = on;
        lycan::SavePresets(lycan::DefaultPresetsPath(), list);
        for (auto& u : cUser_) if (u.name == name) u.inMenu = on;  // the compress page keeps its own copy
        if (!cBase_.builtin && cBase_.name == name) cBase_.inMenu = on;
    }

    // The check boxes of the templates on the settings page (the list of the user's templates can change, so it is built again
    // whenever the page is opened)
    void MainWindow::RebuildShellPresetRows()
    {
        if (!shellPresetRows_) return;
        shellPresetRows_.Children().Clear();
        auto weak = get_weak();
        auto addRow = [&](std::wstring const& name, std::wstring const& label, bool builtin) {
            CheckBox box;
            box.Content(box_value(hstring{label}));
            Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(box, hstring{label});
            const bool was = loadingSettings_;
            loadingSettings_ = true;
            box.IsChecked(PresetInMenu(name, builtin));
            loadingSettings_ = was;
            box.Click([weak, name, builtin](IInspectable const& sender, RoutedEventArgs const&) {
                auto s = weak.get();
                if (!s || s->loadingSettings_) return;
                s->SetPresetInMenu(name, builtin, sender.as<CheckBox>().IsChecked().Value());
            });
            shellPresetRows_.Children().Append(box);
        };
        for (auto const* n : lycan::kBuiltinPresetNames) addRow(n, lycan::Tr(lycan::BuiltinPresetNameId(n)), true);
        for (auto const& u : lycan::LoadPresets(lycan::DefaultPresetsPath())) addRow(u.name, u.name, false);
    }

    void MainWindow::UpdateShellItemsUi()
    {
        const bool was = loadingSettings_;
        loadingSettings_ = true;  // setting the controls from code must not write back
        for (auto& [id, box] : shellChecks_) box.IsChecked(lycan::ShellItemEnabled(settings_.shellItems, id));
        auto select = [](ComboBox const& combo, lycan::ArchiveType t) {
            if (!combo) return;
            for (size_t i = 0; i < std::size(kShellFormats); ++i) if (kShellFormats[i] == t) combo.SelectedIndex(static_cast<int>(i));
        };
        select(shellFmt1_, settings_.shellFormat1);
        select(shellFmt2_, settings_.shellFormat2);
        loadingSettings_ = was;
    }

    void MainWindow::OnShellMenuToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        bool want = ShellMenuToggle().IsOn();
        if (IsStoreUi())   // the Store version: a setting that the menu DLL reads, no registration (ApplyStoreChannelUi)
        {
            settings_.shellMenu = want;
            SaveSettings();
            return;
        }
        std::wstring error;
        bool ok = want ? lycan::RegisterShellMenu(ShellOptions(), &error) : lycan::UnregisterShellMenu(ShellOptions(), &error);
        if (!ok)
        {
            loadingSettings_ = true;
            ShellMenuToggle().IsOn(!want);  // restore the state of the switch
            loadingSettings_ = false;
            ShowMessage(lycan::Tr(L"main.cannot_update_right_click_menu"), hstring{error});
        }
    }

    // ---- The welcome window: the first time the program is started on its own (see Welcome.h)
    bool MainWindow::MaybeShowWelcome()
    {
        const bool forced = GetEnvironmentVariableW(L"LYCAN_FORCE_WELCOME", nullptr, 0) > 0;
        if (!forced && (settings_.welcomeDone || GetEnvironmentVariableW(L"LYCAN_NO_WELCOME", nullptr, 0) > 0)) return false;
        if (lycan_app::WelcomeIsOpen()) return true;
        ShowWelcomeAsync();
        return true;
    }

    winrt::fire_and_forget MainWindow::ShowWelcomeAsync()
    {
        auto strong = get_strong();
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        const lycan_app::Win11MenuState state = lycan_app::QueryWin11Menu();
        co_await ui;
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        const bool available = lycan_app::Win11MenuPackageAvailable(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path()));

        lycan_app::WelcomeHost host;
        const lycan::Settings original = settings_;
        host.settings = settings_;
        for (int i = 0; i < kAccentCount - 1; ++i)
            host.accents.push_back({lycan::Tr(kAccentDefs[i].name), Windows::UI::Color{255, kAccentDefs[i].r, kAccentDefs[i].g, kAccentDefs[i].b}, false});
        host.accents.push_back({lycan::Tr(L"main.follow_system"), Windows::UI::Color{255, 120, 120, 128}, true});
        host.win11Possible = state != lycan_app::Win11MenuState::Unsupported && available;
        host.win11Note = state == lycan_app::Win11MenuState::Unsupported ? lycan::Tr(L"main.pc_not_running_windows_11_there") : lycan::Tr(L"main.version_has_no_signed_menu_package");
        if (lycan::GetDataLocation().packaged || GetEnvironmentVariableW(L"LYCAN_TEST_STORE_UI", nullptr, 0) > 0)
        {
            host.integrationNote = lycan::Tr(L"main.status_built_in");
            host.integrationOn = true;
            host.menuSwitchNote = lycan::Tr(L"main.store_menu_switch_hint");
        }
        else if (lycan::IsStorePackageInstalled()) host.integrationNote = lycan::Tr(L"main.store_version_provides_menu");
        auto weak = get_weak();
        host.preview = [weak](lycan::Settings const& s) {
            auto self = weak.get();
            if (!self) return;
            self->settings_.theme = s.theme;
            self->settings_.accent = s.accent;
            self->settings_.animations = s.animations;
            self->settings_.soundEffects = s.soundEffects;
            self->settings_.soundSet = s.soundSet;
            self->ApplySettings();
            self->ApplyAccent();
        };
        host.playSample = [weak] { if (auto self = weak.get()) self->PlaySfx(L"done"); };
        host.finish = [weak, original](lycan::Settings const& s, bool classic, bool win11, bool assoc) {
            if (auto self = weak.get()) self->ApplyWelcomeAsync(s, original, classic, win11, assoc);
        };
        host.skipped = [weak, original] {
            auto self = weak.get();
            if (!self) return;
            // nothing is kept of what was tried: back to the settings of before, and the window is not shown again
            self->settings_.theme = original.theme;
            self->settings_.accent = original.accent;
            self->settings_.animations = original.animations;
            self->settings_.soundEffects = original.soundEffects;
            self->settings_.soundSet = original.soundSet;
            self->settings_.welcomeDone = true;
            self->ApplySettings();
            self->ApplyAccent();
            self->SaveSettings();
            self->ShowMessage(lycan::Tr(L"main.setup_skipped"), lycan::Tr(L"main.you_can_change_everything_later"));
            self->CheckInstalledCopiesAsync();
        };
        lycan_app::ShowWelcome(std::move(host));
    }

    winrt::fire_and_forget MainWindow::ApplyWelcomeAsync(lycan::Settings s, lycan::Settings original, bool classicMenu, bool win11Menu, bool fileAssoc)
    {
        auto strong = get_strong();
        s.welcomeDone = true;
        settings_ = s;
        SaveSettings();
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
        std::wstring failed;
        std::wstring error;
        if (classicMenu && !lycan::IsShellMenuRegistered(ShellOptions()) && !lycan::RegisterShellMenu(ShellOptions(), &error)) failed += std::wstring{lycan::Tr(L"main.right_click_menu")} + lycan::Tf(L"fmt.colon_value", error) + L"\n";
        error.clear();
        if (fileAssoc && !lycan::IsFileAssociationsRegistered(AssocOptions()) && !lycan::RegisterFileAssociations(AssocOptions(), &error)) failed += std::wstring{lycan::Tr(L"main.file_associations")} + lycan::Tf(L"fmt.colon_value", error) + L"\n";
        if (win11Menu)
        {
            wchar_t exe[MAX_PATH * 2];
            const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
            hstring e = co_await lycan_app::RegisterWin11MenuAsync(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path()));
            if (!e.empty()) failed += std::wstring{lycan::Tr(L"main.windows_11_right_click_menu")} + lycan::Tf(L"fmt.colon_value", std::wstring{e}) + L"\n";
        }
        RefreshFileAssocStatus();
        RefreshWin11MenuAsync();
        loadingSettings_ = true;
        ShellMenuToggle().IsOn(lycan::IsShellMenuRegistered(ShellOptions()));
        FileAssocToggle().IsOn(lycan::IsFileAssociationsRegistered(AssocOptions()));
        loadingSettings_ = false;
        if (!failed.empty()) ShowMessage(lycan::Tr(L"main.some_things_could_not_be_set"), hstring{failed + lycan::Tr(L"main.you_can_try_again_on_settings")});
        else if (_wcsicmp(s.language.c_str(), original.language.c_str()) != 0) ShowMessage(lycan::Tr(L"main.language"), lycan::Tr(L"main.language_changes_after_lycanzip"));
        CheckInstalledCopiesAsync();   // was left out while the welcome window was up: two copies on this PC is said now, not at the next start (dialogs wait for each other)
    }

    // The Windows 11 menu toggle: the state comes from the package manager (asked on a background thread), and the toggle cannot be
    // switched on where it cannot work (not Windows 11, or no menu package next to the program); it can always be switched off
    winrt::fire_and_forget MainWindow::RefreshWin11MenuAsync()
    {
        auto strong = get_strong();
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        const lycan_app::Win11MenuState state = lycan_app::QueryWin11Menu();
        co_await ui;
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        const bool available = lycan_app::Win11MenuPackageAvailable(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path()));
        const bool registered = state == lycan_app::Win11MenuState::Registered;
        loadingSettings_ = true;
        Win11MenuToggle().IsOn(registered);
        loadingSettings_ = false;
        Win11MenuToggle().IsEnabled(registered || (state != lycan_app::Win11MenuState::Unsupported && available));
        // a card that can never be switched on (Windows 10, or a version without a signed menu package: the zip and installer versions) is not shown;
        // the FAQ of the web site says why. A package registered earlier keeps it, so that it can be switched off.
        Win11MenuCard().Visibility(registered || (state != lycan_app::Win11MenuState::Unsupported && available) ? Visibility::Visible : Visibility::Collapsed);
        Win11MenuStatus().Text(state == lycan_app::Win11MenuState::Unsupported ? lycan::Tr(L"main.pc_not_running_windows_11_there")
                               : registered ? lycan::Tr(L"main.status_on")
                               : available ? lycan::Tr(L"main.status_off")
                                           : lycan::Tr(L"main.version_has_no_signed_menu_package"));
        ApplyStoreChannelUi();
    }

    winrt::fire_and_forget MainWindow::OnWin11MenuToggled(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_ || win11MenuBusy_) co_return;
        auto strong = get_strong();
        const bool want = Win11MenuToggle().IsOn();
        win11MenuBusy_ = true;
        Win11MenuToggle().IsEnabled(false);
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        hstring error = want ? co_await lycan_app::RegisterWin11MenuAsync(lycan::EnsureShellHome(fs::path(std::wstring(exe, n)).parent_path())) : co_await lycan_app::UnregisterWin11MenuAsync();
        win11MenuBusy_ = false;
        if (!error.empty()) ShowMessage(want ? lycan::Tr(L"main.could_not_add_windows_11_context") : lycan::Tr(L"main.could_not_remove_windows_11"), error);
        else if (want) ShowMessage(lycan::Tr(L"main.added"), lycan::Tr(L"main.if_menu_does_not_show_up"));
        RefreshWin11MenuAsync();
    }

    static bool SamePath(fs::path const& a, fs::path const& b)
    {
        return !a.empty() && !b.empty() && _wcsicmp(a.lexically_normal().c_str(), b.lexically_normal().c_str()) == 0;
    }

    namespace
    {
        // Ends the processes of File Explorer of this session. Windows starts the shell again by itself after a moment; when it did not
        // within a few seconds it is started here (explorer.exe opens a folder window when a shell runs already, so only when none does).
        void RestartExplorerProcesses()
        {
            DWORD session = 0;
            ProcessIdToSessionId(GetCurrentProcessId(), &session);
            auto explorerPids = [&]() {
                std::vector<DWORD> pids;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                if (snap == INVALID_HANDLE_VALUE) return pids;
                PROCESSENTRY32W e{sizeof(e)};
                for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e))
                {
                    DWORD s = 0;
                    if (_wcsicmp(e.szExeFile, L"explorer.exe") == 0 && ProcessIdToSessionId(e.th32ProcessID, &s) && s == session) pids.push_back(e.th32ProcessID);
                }
                CloseHandle(snap);
                return pids;
            };
            for (DWORD pid : explorerPids())
                if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid))
                {
                    TerminateProcess(h, 1);
                    WaitForSingleObject(h, 3000);
                    CloseHandle(h);
                }
            for (int i = 0; i < 20; ++i)   // up to 5 seconds
            {
                Sleep(250);
                if (!explorerPids().empty()) return;
            }
            ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    // "Restart File Explorer": a loaded context menu DLL is only replaced when Explorer starts again (after an update, or after the menu was switched on / off). It closes
    // the folder windows and the taskbar goes away for a moment, so it is asked first.
    winrt::fire_and_forget MainWindow::RestartExplorerAsync()
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.restart_explorer_title"), L"");
        TextBlock tb;
        tb.Text(lycan::Tr(L"main.restart_explorer_question"));
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(460);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.restart_explorer_do"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        lycan::diag::Info(L"restarting File Explorer");
        winrt::apartment_context ui;
        co_await winrt::resume_background();
        RestartExplorerProcesses();
        co_await ui;
    }

    void MainWindow::OnRestartExplorerClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        RestartExplorerAsync();
    }

    // After the program was updated or moved: the context menus and the file associations that the user switched on have to point to this version.
    // - The same folder, an older menu DLL (a zip unpacked over the old one, winget, Scoop): the menu DLL has the version in its name, so the new
    //   one is a new file; the registration is pointed to it without a question (the user switched the menu on), the old DLLs are deleted when
    //   Explorer does not hold them any more, and a notice offers to restart Explorer.
    // - Another folder (a portable folder was moved or copied): ask, because it may be a copy that the user does not want to use.
    // Nothing is registered that the user has not switched on before.
    winrt::fire_and_forget MainWindow::CheckStaleRegistrationsAsync()
    {
        auto strong = get_strong();
        if (GetEnvironmentVariableW(L"LYCAN_NO_STALE_CHECK", nullptr, 0) > 0) co_return;  // tests: the real registrations point to another build
        wchar_t exeBuf[MAX_PATH * 2];
        const fs::path exe = std::wstring(exeBuf, GetModuleFileNameW(nullptr, exeBuf, static_cast<DWORD>(std::size(exeBuf))));
        const fs::path exeDir = exe.parent_path();
        // where the menu DLL and the Windows 11 package are (the folder of the exe, or outside of an installed copy). Nothing is copied there
        // unless a registration needs it (RegisterShellMenu and EnsureShellHome below): a copy that never switched the menu on leaves no files.
        const fs::path dir = lycan::ShellHome(exeDir);
        const fs::path dllNow = lycan::ShellDllPath(dir);

        // ---- the same folder: update the registrations
        bool classicUpdate = false, win11Update = false, blocked = false;
        std::vector<std::pair<hstring, hstring>> failures;   // title, reason
        fs::path registeredDll;
        if (lycan::IsShellMenuRegistered(ShellOptions()))
        {
            registeredDll = lycan::RegisteredShellDll(ShellOptions());
            classicUpdate = !SamePath(registeredDll, dllNow) && SamePath(registeredDll.parent_path(), dir);
        }
        if (classicUpdate)
        {
            std::wstring error;
            if (lycan::RegisterShellMenu(ShellOptions(), &error)) registeredDll = dllNow;
            else { failures.push_back({lycan::Tr(L"main.cannot_update_right_click_menu"), hstring{error}}); blocked = true; }
        }
        lycan_app::Win11MenuRegistration w11;
        if (TestRegistryRoot().empty())  // (the package is not part of the test registry key)
        {
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            w11 = lycan_app::QueryWin11MenuRegistration();
            co_await ui;
        }
        const bool w11Here = w11.found && SamePath(fs::path(w11.location), dir);
        if (w11Here && (w11.major != LYCAN_VERSION_MAJOR || w11.minor != LYCAN_VERSION_MINOR || w11.build != LYCAN_VERSION_PATCH))
        {
            lycan::EnsureShellHome(exeDir);
            if (!lycan_app::Win11MenuPackageAvailable(dir)) blocked = true;   // nothing to register the new version with: the old DLL has to stay
            else
            {
                hstring failure = co_await lycan_app::UnregisterWin11MenuAsync();
                if (failure.empty()) failure = co_await lycan_app::RegisterWin11MenuAsync(dir);
                if (failure.empty()) win11Update = true;
                else { failures.push_back({lycan::Tr(L"main.could_not_update_windows_11"), failure}); blocked = true; }
            }
        }
        if (!blocked)
        {
            const int removed = lycan::RemoveOtherShellDlls(dir, dllNow);
            if (removed) lycan::diag::Info(L"removed {} menu DLL(s) of older versions", removed);
        }
        for (auto const& f : failures) ShowMessage(f.first, f.second);
        if (failures.empty() && (classicUpdate || win11Update))
        {
            std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
            links.push_back({{lycan::Tr(L"main.restart_explorer"), {}}, [this]() { RestartExplorerAsync(); }});
            ShowNotice(UpdateStrip(), false, lycan::Tr(L"main.menu_updated"), lycan::Tr(L"main.menu_updated_restart_explorer"), links);
        }

        // ---- another folder: ask
        if (SamePath(fs::path(settings_.ignoredMenuPath), exe)) co_return;
        std::vector<std::wstring> stale;
        std::wstring nowUsed;   // where they point now: the other copy (or the old folder)
        bool classicStale = false, assocStale = false, win11Stale = false;
        if (!registeredDll.empty() && !SamePath(registeredDll.parent_path(), dir))
        {
            classicStale = true;
            stale.push_back(lycan::Tr(L"main.right_click_menu"));
            std::ifstream f(registeredDll.parent_path() / L"LycanZip.exepath", std::ios::binary);   // an installed copy: the DLL is outside the folder of the program
            std::string utf8((std::istreambuf_iterator<char>(f)), {});
            while (!utf8.empty() && (utf8.back() == '\r' || utf8.back() == '\n' || utf8.back() == ' ')) utf8.pop_back();
            nowUsed = utf8.empty() ? registeredDll.parent_path().wstring() : fs::path(std::wstring{winrt::to_hstring(utf8)}).parent_path().wstring();
        }
        if (lycan::IsFileAssociationsRegistered(AssocOptions()) && !SamePath(lycan::RegisteredAssocExe(AssocOptions()), exe))
        {
            assocStale = true;
            stale.push_back(lycan::Tr(L"main.file_associations"));
            if (nowUsed.empty()) nowUsed = lycan::RegisteredAssocExe(AssocOptions()).parent_path().wstring();
        }
        if (w11.found && !w11Here)
        {
            win11Stale = true;
            stale.push_back(lycan::Tr(L"main.windows_11_right_click_menu"));
            if (nowUsed.empty()) nowUsed = w11.location;
        }
        if (stale.empty()) co_return;

        std::wstring list;
        for (auto const& s : stale) list += (list.empty() ? std::wstring{} : std::wstring{lycan::Tr(L"fmt.list_sep")}) + s;
        auto dlg = MakeDialog(lycan::Tr(L"main.lycanzip_was_moved"), L"\uE946");
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.still_point_old_folder_so_right", list, exeDir.wstring(), nowUsed)});
        tb.IsTextSelectionEnabled(true);
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(460);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.use_current_folder"));
        dlg.SecondaryButtonText(lycan::Tr(L"main.do_not_remind_me_again"));
        dlg.CloseButtonText(lycan::Tr(L"main.later"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        const auto answer = co_await ShowDialogAsync(dlg);
        if (answer == ContentDialogResult::Secondary)
        {
            settings_.ignoredMenuPath = exe.wstring();
            SaveSettings();
            co_return;
        }
        if (answer != ContentDialogResult::Primary) co_return;

        std::wstring error;
        if (classicStale && !lycan::RegisterShellMenu(ShellOptions(), &error)) { ShowMessage(lycan::Tr(L"main.cannot_update_right_click_menu"), hstring{error}); co_return; }
        if (assocStale && !lycan::RegisterFileAssociations(AssocOptions(), &error)) { ShowMessage(lycan::Tr(L"main.cannot_update_file_associations"), hstring{error}); co_return; }
        if (win11Stale)
        {
            hstring failure = co_await lycan_app::UnregisterWin11MenuAsync();
            if (failure.empty() && lycan_app::Win11MenuPackageAvailable(dir)) failure = co_await lycan_app::RegisterWin11MenuAsync(dir);
            if (!failure.empty()) { ShowMessage(lycan::Tr(L"main.could_not_update_windows_11"), failure); co_return; }
        }
        ShowMessage(lycan::Tr(L"main.updated"), lycan::Tr(L"main.if_menu_does_not_show_up_yet_restart_file_explorer_sign_out"));
        RefreshWin11MenuAsync();
    }

    // Restore the default settings (the context menu and the file associations are not affected, they are system-level registry entries)
    fire_and_forget MainWindow::OnResetSettingsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.restore_default_settings_title"), L"\uE777");
        TextBlock tb;
        tb.Text(lycan::Tr(L"main.defaults_theme_accent_color"));
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.restore"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        settings_ = lycan::Settings{};
        SaveSettings();
        LoadSettingsToUi();
        ApplySettings();
        ApplyAccent();
        ApplyViewSettings();
    }

    // ---------------------------------------------------------------- Check for a new version

    // Asks latest.json (versioncheck.h). Automatic: only when the user turned it on, never for the Store version (the Store updates it), at
    // most once in seven days, silent when it fails. Manual (the button in About): always, and it says what happened.
    winrt::fire_and_forget MainWindow::CheckForUpdateAsync(bool manual)
    {
        auto strong = get_strong();
        const lycan::InstallChannel channel = lycan::DetectChannel();
        const long long now = static_cast<long long>(std::time(nullptr));
        if (!manual)
        {
            if (compact_ || !settings_.checkUpdates || channel == lycan::InstallChannel::Store) co_return;
            if (!lycan::UpdateCheckDue(settings_.lastUpdateCheck, now)) co_return;
        }
        else
        {
            UpdateStatusText().Text(lycan::Tr(L"main.update_checking"));
            UpdateCheckNowButton().IsEnabled(false);
        }
        std::wstring url;
        {
            wchar_t buf[512];
            const DWORD n = GetEnvironmentVariableW(L"LYCAN_UPDATE_URL", buf, static_cast<DWORD>(std::size(buf)));   // tests: a server on this computer
            if (n > 0 && n < std::size(buf)) url.assign(buf, n);
        }
        co_await resume_background();
        const auto latest = lycan::FetchLatest(url);
        auto dq = DispatcherQueue();
        auto weak = get_weak();
        dq.TryEnqueue([weak, latest, manual, channel, now]()
        {
            auto self = weak.get();
            if (!self) return;
            if (latest) { self->settings_.lastUpdateCheck = now; self->SaveSettings(); }
            if (manual) self->UpdateCheckNowButton().IsEnabled(true);
            const auto theirs = latest ? lycan::ParseVersion(latest->version) : std::nullopt;
            const lycan::Version mine{LYCAN_VERSION_MAJOR, LYCAN_VERSION_MINOR, LYCAN_VERSION_PATCH};
            if (!theirs)
            {
                lycan::diag::Info(L"update check: no answer");
                if (manual) self->UpdateStatusText().Text(lycan::Tr(L"main.update_check_failed"));
                return;
            }
            const bool newer = lycan::IsNewerVersion(*theirs, mine);
            lycan::diag::Info(L"update check: latest {}, {}", lycan::WideFromUtf8(latest->version), newer ? L"newer" : L"not newer");
            if (!newer)
            {
                if (manual) self->UpdateStatusText().Text(hstring{lycan::Tf(L"main.update_up_to_date", std::wstring(LYCAN_VERSION_WSTRING))});
                return;
            }
            if (!manual && self->settings_.skipUpdateVersion == latest->version) return;
            if (manual) self->UpdateStatusText().Text(hstring{lycan::Tf(L"main.update_available", lycan::WideFromUtf8(latest->version))});
            if (channel == lycan::InstallChannel::Store) return;   // the manual check says so above; the Store does the update
            self->ShowUpdateNotice(*latest);
            if (!manual && channel == lycan::InstallChannel::Installed) self->PrepareUpdateAsync();   // the installed copy has the update ready when the user looks
        });
    }

    // The installed copy, after the automatic check found a newer version: the signed feed is asked and the update is downloaded in the background
    // (nothing is installed; the notice then offers "Restart and update"). When anything fails, the notice with "Install now" stays and the user can try by hand.
    winrt::fire_and_forget MainWindow::PrepareUpdateAsync()
    {
        auto strong = get_strong();
#ifdef LYCAN_VELOPACK
        if (updateBusy_ || readyUpdater_) co_return;
        updateBusy_ = true;
        auto updater = std::make_shared<lycan::InstalledUpdater>(lycan::UpdateFeedBase(), lycan::GetUpdatePublicKey());
        std::wstring error;
        std::optional<lycan::UpdateOffer> offer;
        bool ok = false;
        {
            auto dq = DispatcherQueue();
            auto weak = get_weak();
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            offer = updater->Check(&error);
            if (offer)
            {
                std::atomic<int> shown{-10};
                ok = updater->Download([dq, weak, &shown](int percent) {
                    if (percent < shown.load() + 5 && percent < 100) return;   // not at every percent
                    shown = percent;
                    dq.TryEnqueue([weak, percent]() { if (auto self = weak.get()) self->ShowNotice(self->UpdateStrip(), false, hstring{lycan::Tf(L"main.update_downloading", percent)}, {}, {}); });
                }, &error);
            }
            co_await ui;
        }
        updateBusy_ = false;
        if (!offer || !ok)
        {
            lycan::diag::Info(L"update (background): {}", error.empty() ? std::wstring(L"no newer version in the feed") : error);
            if (offer) UpdateStrip().Visibility(Visibility::Collapsed);   // the progress notice goes; the user can try again from the About page
            co_return;
        }
        readyUpdater_ = updater;
        const std::wstring ver = lycan::WideFromUtf8(offer->version);
        lycan::diag::Info(L"update (background): {} is downloaded and checked", ver);
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        links.push_back({{lycan::Tr(L"main.update_restart_now"), {}}, [this]() { ApplyReadyUpdate(); }});
        ShowNotice(UpdateStrip(), false, hstring{lycan::Tf(L"main.update_ready", ver)}, hstring{lycan::Tr(L"main.update_ready_hint")}, links);
#else
        co_return;
#endif
    }

    void MainWindow::ApplyReadyUpdate()
    {
#ifdef LYCAN_VELOPACK
        if (!readyUpdater_) return;
        if (UnfinishedTaskCount() > 0)
        {
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_tasks_running"));
            return;
        }
        std::wstring error;
        if (!readyUpdater_->ApplyAfterExit(&error))
        {
            lycan::diag::Warn(L"update: {}", error);
            readyUpdater_.reset();
            UpdateStrip().Visibility(Visibility::Collapsed);
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_failed"));
            return;
        }
        lycan::diag::Info(L"update: the update program was started, closing");
        Application::Current().Exit();
#endif
    }

    void MainWindow::ShowUpdateNotice(lycan::LatestInfo const& info)
    {
        const lycan::InstallChannel channel = lycan::DetectChannel();
        const std::wstring ver = lycan::WideFromUtf8(info.version);
        auto openUrl = [](std::string const& u) { if (!u.empty()) ShellExecuteW(nullptr, L"open", lycan::WideFromUtf8(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> links;
        hstring message;
        if (channel == lycan::InstallChannel::Winget || channel == lycan::InstallChannel::Scoop)
        {
            const std::wstring cmd = channel == lycan::InstallChannel::Winget ? L"winget upgrade Aegisulf.LycanZip" : L"scoop update lycanzip";
            message = hstring{lycan::Tf(L"main.update_with_command", cmd)};
        }
        else if (channel == lycan::InstallChannel::Installed)
        {
            links.push_back({{lycan::Tr(L"main.update_install_now"), {}}, [this]() { InstallUpdateAsync(); }});
        }
        else
        {
            const std::string page = info.page.empty() ? std::string("https://aegisulf.com/lycanzip/download/") : info.page;
            links.push_back({{lycan::Tr(L"main.update_download"), {}}, [openUrl, page]() { openUrl(page); }});
        }
        if (!info.changelog.empty())
            links.push_back({{lycan::Tr(L"main.update_whats_new"), {}}, [openUrl, log = info.changelog]() { openUrl(log); }});
        links.push_back({{lycan::Tr(L"main.update_skip"), hstring{lycan::Tr(L"main.update_skip_tip")}}, [this, v = info.version]()
        {
            settings_.skipUpdateVersion = v;
            SaveSettings();
            UpdateStrip().Visibility(Visibility::Collapsed);
        }});
        ShowNotice(UpdateStrip(), false, hstring{lycan::Tf(L"main.update_available", ver)}, message, links);
    }

    // The installed copy: asks the signed feed (the signature and the hash of every package are checked, src/updater), asks the user, downloads, starts the
    // update program that waits for this process to end, and ends the process. Nothing is changed when anything fails.
    winrt::fire_and_forget MainWindow::InstallUpdateAsync()
    {
        auto strong = get_strong();
#ifdef LYCAN_VELOPACK
        if (readyUpdater_) { ApplyReadyUpdate(); co_return; }   // downloaded in the background already
        if (updateBusy_) co_return;
        if (UnfinishedTaskCount() > 0)
        {
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_tasks_running"));
            co_return;
        }
        updateBusy_ = true;
        auto updater = std::make_shared<lycan::InstalledUpdater>(lycan::UpdateFeedBase(), lycan::GetUpdatePublicKey());
        std::wstring error;
        std::optional<lycan::UpdateOffer> offer;
        {
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            offer = updater->Check(&error);
            co_await ui;
        }
        if (!offer)
        {
            lycan::diag::Info(L"update: {}", error.empty() ? std::wstring(L"no newer version in the feed") : error);
            updateBusy_ = false;
            ShowMessage(lycan::Tr(L"main.update_install_title"), error.empty() ? lycan::Tr(L"main.update_not_in_feed") : lycan::Tr(L"main.update_failed"));
            co_return;
        }
        {
            wchar_t size[64];
            swprintf_s(size, L"%.1f MB", static_cast<double>(offer->bytes) / (1024.0 * 1024.0));
            auto dlg = MakeDialog(lycan::Tr(L"main.update_install_title"), L"\uE896");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.update_install_question", lycan::WideFromUtf8(offer->version), std::wstring(size))});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"main.update_install_now"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Primary);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) { updateBusy_ = false; co_return; }
        }
        if (UnfinishedTaskCount() > 0)   // something was started while the question was open
        {
            updateBusy_ = false;
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_tasks_running"));
            co_return;
        }
        auto showProgress = [this](int percent) { ShowNotice(UpdateStrip(), false, hstring{lycan::Tf(L"main.update_downloading", percent)}, {}, {}); };
        showProgress(0);
        bool ok = false;
        {
            auto dq = DispatcherQueue();
            auto weak = get_weak();
            winrt::apartment_context ui;
            co_await winrt::resume_background();
            std::atomic<int> shown{0};
            ok = updater->Download([dq, weak, &shown](int percent) {
                if (percent < shown.load() + 5 && percent < 100) return;   // not at every percent
                shown = percent;
                dq.TryEnqueue([weak, percent]() { if (auto self = weak.get()) self->ShowNotice(self->UpdateStrip(), false, hstring{lycan::Tf(L"main.update_downloading", percent)}, {}, {}); });
            }, &error);
            co_await ui;
        }
        if (ok && UnfinishedTaskCount() == 0) ok = updater->ApplyAfterExit(&error);
        if (!ok)
        {
            lycan::diag::Warn(L"update: {}", error);
            updateBusy_ = false;
            UpdateStrip().Visibility(Visibility::Collapsed);
            ShowMessage(lycan::Tr(L"main.update_install_title"), lycan::Tr(L"main.update_failed"));
            co_return;
        }
        lycan::diag::Info(L"update: the update program was started, closing");
        Application::Current().Exit();
#else
        co_return;
#endif
    }

    void MainWindow::OnUpdateCheckNowClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        CheckForUpdateAsync(true);
    }

    // The links of the About page open in the browser of the user. Tests: LYCAN_TEST_URL_FILE=<file> appends the address to the file instead.
    static void OpenAboutLink(wchar_t const* url)
    {
        if (const wchar_t* f = _wgetenv(L"LYCAN_TEST_URL_FILE"))
        {
            std::ofstream(f, std::ios::app) << lycan::ToUtf8(url) << "\n";
            return;
        }
        ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    }

    void MainWindow::OnOpenWebsiteClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { OpenAboutLink(L"https://aegisulf.com/lycanzip/"); }
    void MainWindow::OnOpenSourceClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { OpenAboutLink(L"https://github.com/Aegisulf/LycanZip"); }

    void MainWindow::OnShowWelcomeClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (!lycan_app::WelcomeIsOpen()) ShowWelcomeAsync();
    }

    void MainWindow::OnOpenSettingsFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring args = L"/select,\"" + settingsPath_.wstring() + L"\"";
        std::error_code ec;
        if (fs::exists(settingsPath_, ec)) ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        else ShellExecuteW(nullptr, L"open", settingsPath_.parent_path().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // "Data location" in the About tab: portable (the settings next to the program) or in the user's folder; see datalocation.h
    void MainWindow::RefreshDataLocationUi()
    {
        const lycan::DataLocation loc = lycan::GetDataLocation();
        DataModeText().Text(loc.packaged ? lycan::Tr(L"main.msix_package_settings_templates")
                            : loc.portable ? lycan::Tr(L"main.portable_mode_settings_templates")
                                           : lycan::Tr(L"main.normal_mode_settings_templates"));
        DataFolderText().Text(hstring{lycan::Tf(L"main.folder_settings", settingsPath_.parent_path().wstring())});
        DataSwitchButton().Visibility(loc.packaged ? Visibility::Collapsed : Visibility::Visible);
        if (loc.packaged) { DataNoteText().Text(L""); return; }
        DataSwitchButton().Content(box_value(loc.portable ? lycan::Tr(L"main.switch_normal_mode_user_folder") : lycan::Tr(L"main.switch_portable_mode_program")));
        const bool blocked = !loc.portable && !lycan::DirWritable(loc.exeDir);
        DataSwitchButton().IsEnabled(!blocked);
        DataNoteText().Text(blocked ? hstring{lycan::Tf(L"main.program_folder_cannot_be_written", loc.exeDir.wstring())}
                                    : lycan::Tr(L"main.switching_copies_settings"));
    }

    fire_and_forget MainWindow::OnDataSwitchClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto strong = get_strong();
        const lycan::DataLocation loc = lycan::GetDataLocation();
        if (loc.packaged) co_return;
        if (UnfinishedTaskCount() > 0)
        {
            ShowMessage(lycan::Tr(L"main.tasks_still_running"), lycan::Tr(L"main.wait_tasks_end_cancel_them_before"));
            co_return;
        }
        const bool toPortable = !loc.portable;
        auto dlg = MakeDialog(lycan::Tr(L"main.switch_data_location"), L"\uE8B7");
        TextBlock tb;
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(460);
        tb.Text(hstring{(toPortable ? lycan::Tf(L"main.settings_templates_copied_program", loc.exeDir.wstring())
                                    : lycan::Tf(L"main.settings_templates_copied_user", loc.userDir.wstring())) +
                        L"\n" + lycan::Tr(L"main.if_new_place_has_settings_files")});
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.switch_restart"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
        if (UnfinishedTaskCount() > 0) co_return;  // a task started while the question was open
        SaveSettings();  // what is on the screen goes into the files that are copied
        const lycan::SwitchStatus st = lycan::SwitchDataLocation(loc.exeDir, loc.userDir, toPortable);
        lycan::diag::Write(lycan::diag::Level::Warn, std::wstring(L"data location: switch to ") + (toPortable ? L"the program folder" : L"the user folder") + L", result " + std::to_wstring(static_cast<int>(st)));
        if (st != lycan::SwitchStatus::Ok)
        {
            ShowMessage(lycan::Tr(L"main.could_not_switch"), st == lycan::SwitchStatus::NotWritable ? lycan::Tr(L"main.new_place_cannot_be_written")
                                                : st == lycan::SwitchStatus::CopyFailed ? lycan::Tr(L"main.copying_settings_failed")
                                                                                       : lycan::Tr(L"main.flag_file_portable_mode_lycanzip"));
            co_return;
        }
        // start the program again (it waits for this one to end) and end this one
        wchar_t exe[MAX_PATH * 2];
        const DWORD n = GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        std::wstring cmd = L"\"" + std::wstring(exe, n) + L"\" --wait-pid " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi))
        {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        Application::Current().Exit();
    }

    void MainWindow::OnOpenLogFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        std::error_code ec;
        const fs::path dir = lycan::diag::Dir();
        fs::create_directories(dir, ec);  // nothing may have been written yet
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // The version, Windows, the engine, the switches and the last lines of the log: one paste into a bug report
    void MainWindow::OnCopyDiagnosticsClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        Windows::ApplicationModel::DataTransfer::DataPackage dp;
        dp.SetText(hstring{lycan::diag::DiagnosticText(40)});
        Windows::ApplicationModel::DataTransfer::Clipboard::SetContent(dp);
        DiagCopiedNote().Visibility(Visibility::Visible);
        auto timer = dispatcher_.CreateTimer();
        timer.IsRepeating(false);
        timer.Interval(std::chrono::seconds(3));
        timer.Tick([weak = get_weak()](auto&& sender, auto&&) {
            if (auto self = weak.get()) self->DiagCopiedNote().Visibility(Visibility::Collapsed);
            sender.Stop();
        });
        timer.Start();
    }

    void MainWindow::OnSettingsBackClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ShowView(viewBeforeSettings_ == View::Archive && !current_ ? View::Home : viewBeforeSettings_);
    }
}
