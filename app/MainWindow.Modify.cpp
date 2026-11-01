#include "pch.h"
// The main window: changing an archive (add, delete, rename, new folder) and editing a file of the archive.
#include "MainWindowShared.h"

namespace winrt::LycanZip::implementation
{
    // ---------------------------------------------------------------- Changing the archive (the design notes "Changing an archive")
    //
    // add / delete / rename / new folder run as one task of the queue (TaskKind::Update): the archive is rewritten in a
    // temporary file next to it and swapped in (update.h). While it runs the window keeps showing the old content; when it is
    // done the archive is opened again and the view comes back to where it was.

    namespace
    {
        constexpr uint64_t kUpdateWarnBytes = 64ull << 20;  // a solid block that is unpacked and packed again beyond this: ask first

        // A name for one item (no folders in it): what Windows can extract, no ".", no "..".
        bool ValidEntryName(std::wstring const& n)
        {
            if (n.empty() || n.size() > 255 || n == L"." || n == L"..") return false;
            for (wchar_t c : n)
                if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c)) return false;
            return true;
        }

        FileStamp StampOfFile(fs::path const& p)
        {
            FileStamp s;
            std::error_code ec;
            const auto t = fs::last_write_time(p, ec);
            if (ec) return s;
            s.time = t.time_since_epoch().count();
            s.size = fs::file_size(p, ec);
            s.ok = !ec;
            return s;
        }

        std::wstring TrimName(std::wstring s)
        {
            const size_t b = s.find_first_not_of(L" \t"), e = s.find_last_not_of(L" \t");
            return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
        }

        bool PathIsUnder(std::wstring const& path, std::wstring const& folder)
        {
            return path.size() > folder.size() && path.compare(0, folder.size(), folder) == 0 && path[folder.size()] == L'/';
        }
    }

    std::wstring MainWindow::CurrentFolderPath() const
    {
        return tree_ && curNode_ < tree_->NodeCount() ? tree_->Node(curNode_).path : std::wstring();
    }

    // Can the shown archive be changed right now? With explain: says why not.
    bool MainWindow::CanModifyNow(bool explain)
    {
        if (!current_ || !tree_) return false;
        if (updateTask_ != 0)
        {
            if (explain) ShowStatusNote(lycan::Tr(L"main.archive_being_changed_please_wait"));
            return false;
        }
        if (!current_->Caps().canModify)
        {
            if (explain)
                ShowMessage(lycan::Tr(L"main.archive_cannot_be_changed_directly"),
                            hstring{lycan::Tf(L"main.cannot_be_changed_place_only_zip", current_->FormatName())});
            return false;
        }
        const DWORD attr = GetFileAttributesW(currentPath_.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_READONLY))
        {
            if (explain) ShowMessage(lycan::Tr(L"main.archive_read_only"), lycan::Tr(L"main.file_set_read_only_cannot_be"));
            return false;
        }
        return true;
    }

    lycan::UpdateOptions MainWindow::MakeUpdateOptions(lycan::UpdateMode mode)
    {
        lycan::UpdateOptions uo;
        uo.mode = mode;
        uo.compress.password = password_;
        uo.compress.tempDir = lycan_app::ScratchOverride();
        uo.compress.skipThumbsDb = settings_.skipThumbsDb;
        uo.keepBackup = settings_.keepBackupOnModify;
        uo.nameCodepage = currentCp_;
        return uo;
    }

    // The window sits still while the archive is replaced; afterwards it opens the new file, in the same folder and view
    fire_and_forget MainWindow::RunUpdateAsync(std::vector<lycan::UpdateOp> ops, TaskMeta meta, lycan::UpdateMode mode, bool upgrade)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true)) co_return;
        if (current_->Caps().encrypted && !co_await EnsurePasswordAsync()) co_return;
        if (!CanModifyNow(false)) co_return;  // the archive changed while the password was asked
        const fs::path archive = currentPath_;
        // a tar whose names have no encoding mark is written again with every name in UTF-8 (a zip keeps the untouched ones): ask once
        if (!upgrade && current_->NameEncoding().applicable && tarNamesOkFor_ != archive)
        {
            lycan::ArchiveType type;
            if (lycan::WritableTypeOf(*current_, type) && type == lycan::ArchiveType::Tar)
            {
                if (!co_await ConfirmTarNamesAsync()) co_return;
                if (!current_ || currentPath_ != archive) co_return;
                tarNamesOkFor_ = archive;
            }
        }
        lycan::diag::Info(L"update of {}: {} operation(s)", lycan::diag::Name(archive), ops.size());
        lycan::UpdateOptions uo = MakeUpdateOptions(mode);
        uo.rewriteNames = upgrade;   // "upgrade the archive": the names again in UTF-8, the data as it is
        uo.verify = upgrade;         // ... replaced only when the new archive passes the test

        // what it costs (a solid 7z block that loses an item is packed again); a bad operation shows up here before anything runs
        uint64_t recompress = 0;
        std::wstring error;
        SetBusy(true);
        co_await resume_background();
        try { recompress = lycan::EstimateUpdate(archive, ops, uo).recompressBytes; }
        catch (lycan::Error const& e) { error = e.message().Text(); }
        catch (std::exception const& e) { error = FromUtf8(e.what()); }
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!error.empty())
        {
            ShowMessage(lycan::Tr(L"main.archive_could_not_be_changed"), hstring{error});
            co_return;
        }
        if (!current_ || currentPath_ != archive || updateTask_ != 0) co_return;
        if (recompress >= kUpdateWarnBytes)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.will_take_while"), L"");
            TextBlock tb;
            tb.Text(hstring{lycan::Tf(L"main.about_solid_archive_has_be", FormatSize(recompress))});
            tb.TextWrapping(TextWrapping::Wrap);
            tb.MaxWidth(440);
            dlg.Content(tb);
            dlg.PrimaryButtonText(lycan::Tr(L"main.resume"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Primary);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
            if (!current_ || currentPath_ != archive || updateTask_ != 0) co_return;
        }

        // the previews show items by index: they would go on showing the old archive
        lycan_app::CloseImagePreview(this);
        lycan_app::CloseAudioPreview(this);
        meta.archive = archive;
        meta.isUpdate = true;
        // the view comes back as it was
        meta.keep.valid = true;
        meta.keep.flat = flat_;
        meta.keep.mode = viewMode_;
        meta.keep.sort = sortKey_;
        meta.keep.asc = sortAsc_;
        meta.keep.tree = treeWanted_;
        meta.keep.query = query_;
        {
            std::lock_guard lk(metaM_);
            UnlockArchiveFile();  // the archive is replaced by a new file: nobody may hold the old one (taken again when it is shown again)
            const uint64_t id = queue_.EnqueueUpdate(archive, std::move(ops), uo);
            updateTask_ = id;
            taskMeta_[id] = std::move(meta);
        }
    }

    void MainWindow::FinishUpdateTask(lycan::TaskInfo const& info, TaskMeta& meta)
    {
        if (updateTask_ == info.id) updateTask_ = 0;
        if (current_ && archiveLock_ == INVALID_HANDLE_VALUE) LockArchiveFile(currentPath_);  // nothing was swapped in (failed, cancelled, no change)
        const bool changed = info.state == lycan::TaskState::Completed || (info.state == lycan::TaskState::Failed && info.update.changed);
        if (info.state == lycan::TaskState::Cancelled) return;
        if (!changed)
        {
            if (info.update.errors.empty() && info.error.empty())
            {
                if (info.update.unchanged || info.update.notAdded)
                    ShowMessage(lycan::Tr(L"main.nothing_update"), lycan::Tr(L"main.files_archive_up_date_none_files"));
                return;
            }
            ShowMessage(lycan::Tr(L"main.archive_could_not_be_changed"), hstring{info.error.empty() ? info.update.errors[0].Text() : info.error.Text()});
            return;
        }
        if (!info.update.errors.empty())
            ShowMessage(lycan::Tr(L"main.some_files_were_not_added"), hstring{lycan::Tf(L"main.archive_was_changed_but_file_s", info.update.errors.size(), info.update.errors[0].Text())});
        if (!current_ || currentPath_ != meta.archive) return;  // this window moved on to another archive

        // the summary shows in the status area once the new archive is on screen
        std::wstring note;
        auto part = [&](uint32_t n, const wchar_t* text) { if (n) note += (note.empty() ? std::wstring{} : std::wstring{lycan::Tr(L"fmt.list_sep")}) + lycan::Tf(text, n); };
        part(info.update.added + info.update.replaced, L"main.status_added");
        part(info.update.deleted, L"main.deleted");
        part(info.update.renamed, L"main.renamed");
        part(info.update.dirsCreated, L"main.new_folder_s");
        part(info.update.unchanged, L"main.unchanged");
        openNote_ = meta.upgrade ? std::wstring(lycan::Tr(L"main.archive_upgraded")) : note.empty() ? std::wstring() : lycan::Tf(L"main.archive_changed", note);
        if (info.update.storedFiles) openNote_ += lycan::Tf(L"main.already_compressed_file_s_stored", info.update.storedFiles);
        for (auto& e : edits_)
            if (e.archive == meta.archive) e.archiveStamp = StampOfFile(e.archive);  // our own change is not "somebody else's"
        restoreFor_ = meta.archive;
        restoreFolder_ = meta.restoreFolder;
        restoreSelect_ = meta.selectPaths;
        keptView_ = meta.keep;
        if (meta.upgrade) currentCp_ = 0;   // the names are marked as UTF-8 now
        OpenArchiveAsync(currentPath_, currentCp_, password_);
    }

    // Adding to an archive that has items of the same name: what is done with the files of the same name
    Windows::Foundation::IAsyncOperation<int32_t> MainWindow::ChooseAddModeAsync(int clashes)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(lycan::Tr(L"main.items_with_same_name"), L"");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(460);
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.archive_already_has_items_with", clashes)});
        tb.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(tb);
        RadioButtons rb;
        struct Opt { const wchar_t* title; const wchar_t* text; };
        const Opt opts[] = {{L"main.replace_add", L"main.files_same_name_always_replaced"},
                            {L"main.update_add", L"main.file_replaced_only_if_one_on"},
                            {L"main.update_existing_only", L"main.only_files_archive_newer_on_disk"}};
        for (auto const& o : opts)
        {
            StackPanel line;
            TextBlock t;
            t.Text(lycan::Tr(o.title));
            line.Children().Append(t);
            line.Children().Append(Caption(lycan::Tr(o.text), 0.7));
            RadioButton r;
            r.Content(line);
            rb.Items().Append(r);
        }
        rb.SelectedIndex(settings_.addMode == lycan::AddMode::Replace ? 0 : settings_.addMode == lycan::AddMode::UpdateAdd ? 1 : settings_.addMode == lycan::AddMode::Freshen ? 2 : 0);
        sp.Children().Append(rb);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.add"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return -1;
        const int idx = std::clamp(rb.SelectedIndex(), 0, 2);
        settings_.addMode = static_cast<lycan::AddMode>(idx);
        SaveSettings();
        co_return idx;
    }

    // Making an archive whose file is there already
    Windows::Foundation::IAsyncOperation<int32_t> MainWindow::ChooseExistingOutputAsync(fs::path output, bool canUpdate)
    {
        auto strong = get_strong();
        PlaySfx(L"conflict");
        auto dlg = MakeDialog(lycan::Tr(L"main.file_already_exists"), L"\uE8C8");
        StackPanel sp;
        sp.Spacing(12);
        sp.MinWidth(480);
        {
            StackPanel where;
            where.Spacing(2);
            TextBlock n;
            n.Text(hstring{output.filename().wstring()});
            n.FontSize(16);
            n.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
            n.TextTrimming(TextTrimming::CharacterEllipsis);
            where.Children().Append(n);
            where.Children().Append(Caption(output.parent_path().wstring(), 0.65));
            sp.Children().Append(where);
        }
        // the choices, in this order (the index of the radio button -> what it means): 0 = add to it, 1 = a new name, 2 = replace it
        std::vector<int> kinds;
        RadioButtons rb;
        auto addOption = [&](int kind, const wchar_t* title, std::wstring const& text) {
            StackPanel line;
            TextBlock t;
            t.Text(title);
            line.Children().Append(t);
            line.Children().Append(Caption(text, 0.7));
            RadioButton r;
            r.Content(line);
            rb.Items().Append(r);
            kinds.push_back(kind);
        };
        if (canUpdate) addOption(3, lycan::Tr(L"main.add_to_this_archive"), lycan::Tr(L"main.add_these_files_what_not"));
        addOption(1, lycan::Tr(L"main.save_as_new_file"), lycan::Tf(L"main.saved_as", UniquePath(output).filename().wstring()));
        addOption(2, lycan::Tr(L"main.replace"), lycan::Tr(L"main.old_archive_replaced_by_one"));
        int sel = 0;
        const int wanted = settings_.existingOutput == lycan::ExistingOutput::Update ? 3 : settings_.existingOutput == lycan::ExistingOutput::Overwrite ? 2 : 1;
        for (size_t i = 0; i < kinds.size(); ++i) if (kinds[i] == wanted) sel = static_cast<int>(i);
        rb.SelectedIndex(sel);
        sp.Children().Append(rb);
        ComboBox modes;
        for (auto const* t : {L"main.replace_add_files_same_name_always", L"main.update_add_replace_only_if_newer", L"main.update_existing_only_only_files",
                              L"main.synchronize_make_archive_match"})
            modes.Items().Append(box_value(hstring{lycan::Tr(t)}));
        modes.SelectedIndex(static_cast<int>(settings_.addMode));
        Microsoft::UI::Xaml::Automation::AutomationProperties::SetName(modes, lycan::Tr(L"main.how_treat_files_same_name"));
        modes.HorizontalAlignment(HorizontalAlignment::Stretch);
        modes.Margin(ThicknessHelper::FromLengths(28, 0, 0, 0));
        auto showModes = [&] { modes.Visibility(canUpdate && rb.SelectedIndex() >= 0 && kinds[static_cast<size_t>(rb.SelectedIndex())] == 3 ? Visibility::Visible : Visibility::Collapsed); };
        showModes();
        rb.SelectionChanged([&](auto&&, auto&&) { showModes(); });
        sp.Children().Append(modes);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return 0;
        const int kind = kinds[static_cast<size_t>(std::clamp(rb.SelectedIndex(), 0, static_cast<int>(kinds.size()) - 1))];
        settings_.existingOutput = kind == 3 ? lycan::ExistingOutput::Update : kind == 2 ? lycan::ExistingOutput::Overwrite : lycan::ExistingOutput::Rename;
        if (kind == 3) settings_.addMode = static_cast<lycan::AddMode>(std::clamp(modes.SelectedIndex(), 0, 3));
        SaveSettings();
        co_return kind == 3 ? 3 + std::clamp(modes.SelectedIndex(), 0, 3) : kind;
    }

    // Synchronize deletes things: what goes, before it is done
    Windows::Foundation::IAsyncOperation<bool> MainWindow::ConfirmSyncAsync(fs::path archive, std::vector<lycan::UpdateOp> ops, lycan::UpdateOptions uo)
    {
        auto strong = get_strong();
        lycan::UpdateReport rep;
        std::wstring error;
        SetBusy(true);
        co_await resume_background();
        try { rep = lycan::PreviewUpdate(archive, ops, uo); }
        catch (lycan::Error const& e) { error = e.message().Text(); }
        catch (std::exception const& e) { error = FromUtf8(e.what()); }
        co_await resume_ui{dispatcher_};
        SetBusy(false);
        if (!error.empty())
        {
            ShowMessage(lycan::Tr(L"main.cannot_synchronize"), hstring{error});
            co_return false;
        }
        if (rep.deleted == 0) co_return true;   // nothing goes out: no question
        auto dlg = MakeDialog(lycan::Tr(L"main.synchronizing_deletes_items_from"), L"\uE7BA");
        StackPanel sp;
        sp.Spacing(10);
        sp.MinWidth(460);
        TextBlock tb;
        tb.Text(hstring{lycan::Tf(L"main.items_archive_no_longer_exist", rep.deleted, rep.added, rep.replaced, rep.unchanged)});
        tb.TextWrapping(TextWrapping::Wrap);
        sp.Children().Append(tb);
        std::wstring names;
        for (size_t i = 0; i < rep.deletedPaths.size() && i < 30; ++i) names += rep.deletedPaths[i] + L"\n";
        if (rep.deletedPaths.size() > 30) names += lycan::Tf(L"main.more", rep.deletedPaths.size() - 30);
        TextBox list;
        list.Text(hstring{names});
        list.IsReadOnly(true);
        list.AcceptsReturn(true);
        list.TextWrapping(TextWrapping::NoWrap);
        list.MaxHeight(220);
        ScrollViewer::SetVerticalScrollBarVisibility(list, ScrollBarVisibility::Auto);
        sp.Children().Append(list);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.synchronize"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Close);
        co_return co_await ShowDialogAsync(dlg) == ContentDialogResult::Primary;
    }

    // Files and folders (from Explorer or a picker) into the folder `into` of the archive ("" = top level)
    fire_and_forget MainWindow::AddPathsToArchive(std::vector<fs::path> paths, std::wstring into)
    {
        auto strong = get_strong();
        if (paths.empty() || !CanModifyNow(true)) co_return;
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        int clashes = 0;
        for (auto const& p : paths)
        {
            std::wstring name = p.has_filename() ? p.filename().wstring() : p.parent_path().filename().wstring();
            if (name.empty()) continue;
            const std::wstring target = into.empty() ? name : into + L"/" + name;
            if (tree_->Find(target) != lycan::kNoNode) ++clashes;
            meta.selectPaths.push_back(target);
        }
        lycan::UpdateMode mode = lycan::UpdateMode::AddReplace;
        if (clashes)
        {
            const int32_t chosen = co_await ChooseAddModeAsync(clashes);
            if (chosen < 0) co_return;
            mode = static_cast<lycan::UpdateMode>(chosen);
            if (!CanModifyNow(false)) co_return;
        }
        RunUpdateAsync({lycan::UpdateOp::Add(into, std::move(paths))}, std::move(meta), mode);
    }

    fire_and_forget MainWindow::AddFromPickerAsync(bool folder, std::wstring into)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true)) co_return;
        std::vector<fs::path> add;
        if (const wchar_t* t = _wgetenv(L"LYCAN_TEST_ADD_PATHS"))  // tests: no picker, '|' separates the paths
        {
            std::wstring all = t;
            size_t pos = 0;
            while (pos <= all.size())
            {
                size_t end = all.find(L'|', pos);
                if (end == std::wstring::npos) end = all.size();
                if (end > pos) add.emplace_back(all.substr(pos, end - pos));
                pos = end + 1;
            }
        }
        else if (folder)
        {
            const fs::path dir{std::wstring(co_await lycan_app::PickFolderAsync(AppWindow().Id(), lycan_app::kPurposeSourceFolder, {}, lycan::Tr(L"main.choose_folder_add")))};
            if (!dir.empty()) add.push_back(dir);
        }
        else
        {
            auto files = co_await lycan_app::PickFilesAsync(AppWindow().Id(), lycan_app::kPurposeSourceFiles, true, false);
            for (auto const& f : files) add.emplace_back(std::wstring(f));
        }
        if (add.empty()) co_return;
        AddPathsToArchive(std::move(add), std::move(into));
    }

    void MainWindow::OnEditorPathLostFocus(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (loadingSettings_) return;
        std::wstring p = TrimName(std::wstring{EditorPathBox().Text()});
        if (p.size() > 260) p.clear();
        if (p.size() >= 2 && p.front() == L'"' && p.back() == L'"') p = p.substr(1, p.size() - 2);  // pasted with its quotes
        if (p == settings_.editorPath) return;
        settings_.editorPath = p;
        SaveSettings();
    }

    void MainWindow::OnAddFilesClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { AddFromPickerAsync(false, CurrentFolderPath()); }
    void MainWindow::OnAddFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { AddFromPickerAsync(true, CurrentFolderPath()); }
    void MainWindow::OnNewFolderClick(Windows::Foundation::IInspectable const&, RoutedEventArgs const&) { NewFolderAsync(CurrentFolderPath()); }

    // One name for a folder or an item: the question stays open until the name is acceptable
    Windows::Foundation::IAsyncOperation<hstring> MainWindow::AskEntryNameAsync(hstring title, hstring initial)
    {
        auto strong = get_strong();
        auto dlg = MakeDialog(title, L"");
        TextBox box;
        box.Text(initial);
        box.MaxLength(255);
        Automation::AutomationProperties::SetName(box, lycan::Tr(L"main.name"));
        TextBlock bad;
        bad.Text(lycan::Tr(L"main.name_cannot_be_empty_contain"));
        bad.Foreground(Microsoft::UI::Xaml::Media::SolidColorBrush{lycan_app::StatusColor(Windows::UI::Color{255, 0xFF, 0x6B, 0x6B})});
        bad.TextWrapping(TextWrapping::Wrap);
        bad.Visibility(Visibility::Collapsed);
        StackPanel sp;
        sp.Spacing(8);
        sp.MinWidth(360);
        sp.Children().Append(box);
        sp.Children().Append(bad);
        dlg.Content(sp);
        dlg.PrimaryButtonText(lycan::Tr(L"main.ok"));
        dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
        dlg.DefaultButton(ContentDialogButton::Primary);
        dlg.PrimaryButtonClick([box, bad](ContentDialog const&, ContentDialogButtonClickEventArgs const& a) {
            if (!ValidEntryName(TrimName(std::wstring{box.Text()}))) { bad.Visibility(Visibility::Visible); a.Cancel(true); }
        });
        auto r = co_await ShowDialogAsync(dlg);
        co_return r == ContentDialogResult::Primary ? hstring{TrimName(std::wstring{box.Text()})} : hstring{};
    }

    fire_and_forget MainWindow::NewFolderAsync(std::wstring into)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true)) co_return;
        std::wstring name{co_await AskEntryNameAsync(lycan::Tr(L"main.new_folder_name"), lycan::Tr(L"main.new_folder_name"))};
        if (name.empty() || !CanModifyNow(false)) co_return;
        const std::wstring path = into.empty() ? name : into + L"/" + name;
        if (tree_->Find(path) != lycan::kNoNode)
        {
            ShowMessage(lycan::Tr(L"main.name_already_exists"), hstring{lycan::Tf(L"main.there_already_item_called_here", name)});
            co_return;
        }
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        meta.selectPaths.push_back(path);
        RunUpdateAsync({lycan::UpdateOp::MakeDir(path)}, std::move(meta));
    }

    fire_and_forget MainWindow::RenameNodeAsync(uint32_t node)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true) || node >= tree_->NodeCount() || node == tree_->Root()) co_return;
        const std::wstring oldPath = tree_->Node(node).path, oldName = tree_->Node(node).name, parent = tree_->ParentPath(node);
        std::wstring name{co_await AskEntryNameAsync(lycan::Tr(L"main.rename"), hstring{oldName})};
        if (name.empty() || name == oldName || !CanModifyNow(false)) co_return;
        const std::wstring path = parent.empty() ? name : parent + L"/" + name;
        if (tree_->Find(path) != lycan::kNoNode)
        {
            ShowMessage(lycan::Tr(L"main.name_already_exists"), hstring{lycan::Tf(L"main.there_already_item_called_here", name)});
            co_return;
        }
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        if (meta.restoreFolder == oldPath || PathIsUnder(meta.restoreFolder, oldPath)) meta.restoreFolder = path + meta.restoreFolder.substr(oldPath.size());  // the folder shown is the one renamed
        meta.selectPaths.push_back(path);
        RunUpdateAsync({lycan::UpdateOp::Rename(oldPath, path)}, std::move(meta));
    }

    fire_and_forget MainWindow::DeleteNodesAsync(std::vector<uint32_t> nodes)
    {
        auto strong = get_strong();
        if (nodes.empty() || !CanModifyNow(true)) co_return;
        // a folder takes what is below it: leave out what is selected inside a selected folder
        std::vector<std::wstring> paths;
        for (uint32_t n : nodes)
        {
            if (n == tree_->Root() || n >= tree_->NodeCount()) continue;
            bool covered = false;
            for (uint32_t m : nodes)
                if (m != n && tree_->Node(m).isDir && PathIsUnder(tree_->Node(n).path, tree_->Node(m).path)) covered = true;
            if (!covered) paths.push_back(tree_->Node(n).path);
        }
        if (paths.empty()) co_return;
        const std::wstring firstName = tree_->Node(nodes[0]).name;
        if (settings_.confirmDeleteEntries)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.delete_from_archive"), L"");
            StackPanel sp;
            sp.Spacing(10);
            sp.MaxWidth(440);
            TextBlock t1;
            t1.Text(hstring{paths.size() == 1 ? lycan::Tf(L"main.delete_one_from_archive", firstName) : lycan::Tf(L"main.delete_these_items_from_archive", paths.size())});
            t1.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t1);
            TextBlock t2;
            t2.Text(settings_.keepBackupOnModify ? lycan::Tr(L"main.items_taken_out_archive_itself_not")
                                                 : lycan::Tr(L"main.items_taken_out_archive_itself_not_moved_recycle_bin_cannot_be"));
            t2.Opacity(0.8);
            t2.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t2);
            CheckBox again;
            again.Content(box_value(hstring{lycan::Tr(L"main.delete_without_asking_from_now_on")}));
            sp.Children().Append(again);
            dlg.Content(sp);
            dlg.PrimaryButtonText(lycan::Tr(L"main.delete"));
            dlg.CloseButtonText(lycan::Tr(L"main.cancel"));
            dlg.DefaultButton(ContentDialogButton::Close);
            if (co_await ShowDialogAsync(dlg) != ContentDialogResult::Primary) co_return;
            if (again.IsChecked().Value())
            {
                settings_.confirmDeleteEntries = false;
                loadingSettings_ = true;
                ConfirmDeleteEntriesToggle().IsOn(false);
                loadingSettings_ = false;
                SaveSettings();
            }
            if (!CanModifyNow(false) || !tree_) co_return;
        }
        std::vector<lycan::UpdateOp> ops;
        for (auto const& p : paths) ops.push_back(lycan::UpdateOp::Delete(p));
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        RunUpdateAsync(std::move(ops), std::move(meta));
    }

    // ---------------------------------------------------------------- Editing a file of the archive
    //
    // "Edit": the file is extracted to a temporary folder, opened in a program (Notepad for text, the program that is
    // associated with the type for everything else, or the editor chosen in the settings) and watched. When it has been
    // saved and is quiet again, the user is asked whether to put it back into the archive (a Replace operation of
    // RunUpdateAsync). The program may be one that is shared by many windows (Notepad, VS Code), so its process is not what
    // is watched but the file: its last write time and size.

    fire_and_forget MainWindow::EditEntryAsync(uint32_t node)
    {
        auto strong = get_strong();
        if (!CanModifyNow(true) || node >= tree_->NodeCount()) co_return;
        auto const& n = tree_->Node(node);
        if (n.isDir || n.entryIndex == lycan::kNoEntry) co_return;
        const uint32_t entryIndex = n.entryIndex;
        const std::wstring name = n.name, entryPath = n.path;
        auto safe = lycan::SanitizeEntryPath(name);
        if (safe.rejected)
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), hstring{lycan::Tf(L"main.file_s_name_unsafe_was_rejected", safe.reason.Text())});
            co_return;
        }
        // one session per item: editing it again goes back to the file that is being edited
        for (auto const& e : edits_)
            if (e.archive == currentPath_ && e.entryPath == entryPath && fs::exists(e.file))
            {
                ShellExecuteW(nullptr, L"open", e.file.c_str(), nullptr, e.file.parent_path().c_str(), SW_SHOWNORMAL);
                co_return;
            }
        if (!co_await EnsurePasswordAsync() || !CanModifyNow(false)) co_return;

        static std::atomic<uint32_t> seq{0};
        if (lycan_app::OpenTempRoot().empty())
        {
            ShowMessage(lycan::Tr(L"main.cannot_open"), lycan::Tr(L"main.temporary_folder_not_found"));
            co_return;
        }
        const fs::path dir = lycan_app::OpenTempRoot() /
                             (std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++seq));
        lycan::ExtractOptions eo;
        eo.overwrite = lycan::OverwriteMode::Overwrite;
        eo.stripPrefix = tree_->ParentPath(node).empty() ? std::wstring() : tree_->ParentPath(node) + L"/";
        if (!password_.empty()) eo.password = [pw = password_](std::wstring& out) { out = pw; return true; };

        TaskMeta meta;
        meta.archive = currentPath_;
        meta.openFile = dir / safe.relative;
        meta.editEntry = entryPath;
        std::lock_guard lk(metaM_);
        const uint64_t id = queue_.EnqueueExtract(currentPath_, dir, eo, MakeOpenOptions(), {entryIndex});
        taskMeta_[id] = std::move(meta);
    }

    // The extraction is done: start the program and the watch
    void MainWindow::StartEditSession(TaskMeta const& meta)
    {
        if (!fs::exists(meta.openFile)) return;
        EditSession s;
        static uint64_t nextId = 1;
        s.id = nextId++;
        s.archive = meta.archive;
        s.entryPath = meta.editEntry;
        s.file = meta.openFile;
        s.archiveStamp = StampOfFile(meta.archive);
        s.baseline = s.lastSeen = StampOfFile(meta.openFile);
        edits_.push_back(s);

        if (!_wgetenv(L"LYCAN_TEST_NO_OPEN"))
        {
            std::wstring program = TrimName(settings_.editorPath);
            std::error_code ec;
            if (!program.empty() && !fs::exists(program, ec) && program.find_first_of(L"\\/") != std::wstring::npos) program.clear();  // not there any more
            const std::wstring workDir = s.file.parent_path().wstring();
            if (program.empty() && lycan::IsTextEditName(s.file.filename().wstring())) program = L"notepad.exe";
            bool started = false;
            if (!program.empty())
            {
                const std::wstring arg = L"\"" + s.file.wstring() + L"\"";
                started = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", program.c_str(), arg.c_str(), workDir.c_str(), SW_SHOWNORMAL)) > 32;
            }
            if (!started)
            {
                started = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, nullptr, s.file.c_str(), nullptr, workDir.c_str(), SW_SHOWNORMAL)) > 32;
                if (!started) started = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"openas", s.file.c_str(), nullptr, workDir.c_str(), SW_SHOWNORMAL)) > 32;
            }
        }
        ShowStatusNote(lycan::Tf(L"main.editing_after_you_save_you_asked", s.file.filename().wstring()));
        if (!editTimer_)
        {
            editTimer_ = dispatcher_.CreateTimer();
            editTimer_.Interval(std::chrono::milliseconds(1000));
            editTimer_.Tick([weak = get_weak()](auto&&, auto&&) { if (auto self = weak.get()) self->PollEdits(); });
        }
        editTimer_.Start();
    }

    // Once a second: a file that was saved and has not changed for a second is ready to go back
    void MainWindow::PollEdits()
    {
        for (size_t i = 0; i < edits_.size();)
        {
            EditSession& e = edits_[i];
            if (e.prompting) { ++i; continue; }
            const FileStamp now = StampOfFile(e.file);
            if (!now.ok)
            {
                // an editor that saves by writing a new file and renaming it leaves a gap; a file that stays away is gone
                if (++e.missing > 600) { edits_.erase(edits_.begin() + static_cast<std::ptrdiff_t>(i)); continue; }
                ++i;
                continue;
            }
            e.missing = 0;
            if (now == e.baseline) e.stable = 0;
            else if (now == e.lastSeen) { if (++e.stable >= 1) PromptWriteBack(e.id); }
            else e.stable = 0;
            e.lastSeen = now;
            ++i;
        }
        if (edits_.empty() && editTimer_) editTimer_.Stop();
    }

    bool MainWindow::HasUnsavedEdits() const
    {
        for (auto const& e : edits_)
        {
            const FileStamp now = StampOfFile(e.file);
            if (now.ok && !(now == e.baseline)) return true;
        }
        return false;
    }

    fire_and_forget MainWindow::PromptWriteBack(uint64_t id)
    {
        auto strong = get_strong();
        auto find = [this, id]() -> EditSession* {
            for (auto& e : edits_) if (e.id == id) return &e;
            return nullptr;
        };
        EditSession* s = find();
        if (!s || s->prompting) co_return;
        // not now: another question is open, a change is running, or the window shows something else (it asks later)
        if (!current_ || currentPath_ != s->archive || updateTask_ != 0 || dialogOpen_ || view_ != View::Archive || !tree_) co_return;
        s->prompting = true;
        const std::wstring name = s->file.filename().wstring();
        const std::wstring entryPath = s->entryPath;
        const fs::path file = s->file;

        // somebody else changed the archive while the file was being edited: putting it back could undo their change
        if (!(StampOfFile(s->archive) == s->archiveStamp))
        {
            s->prompting = false;
            s->baseline = StampOfFile(file);
            ShowMessage(lycan::Tr(L"main.archive_was_changed"), hstring{lycan::Tf(L"main.while_was_being_edited_another", name, file.wstring())});
            co_return;
        }
        const uint32_t node = tree_->Find(entryPath);
        if (node == lycan::kNoNode || tree_->Node(node).isDir)
        {
            s->prompting = false;
            s->baseline = StampOfFile(file);
            ShowMessage(lycan::Tr(L"main.file_no_longer_archive"), hstring{lycan::Tf(L"main.no_longer_archive_deleted_renamed", name, file.wstring())});
            co_return;
        }
        if (settings_.confirmWriteBack)
        {
            auto dlg = MakeDialog(lycan::Tr(L"main.put_back_into_archive"), L"");
            StackPanel sp;
            sp.Spacing(10);
            sp.MaxWidth(440);
            TextBlock t1;
            t1.Text(hstring{lycan::Tf(L"main.was_changed_put_back_into_archive", name)});
            t1.TextWrapping(TextWrapping::Wrap);
            sp.Children().Append(t1);
            CheckBox again;
            again.Content(box_value(hstring{lycan::Tr(L"main.put_back_without_asking_from_now")}));
            sp.Children().Append(again);
            dlg.Content(sp);
            dlg.PrimaryButtonText(lycan::Tr(L"main.put_back"));
            dlg.CloseButtonText(lycan::Tr(L"main.not_now"));
            dlg.DefaultButton(ContentDialogButton::Primary);
            const auto r = co_await ShowDialogAsync(dlg);
            s = find();
            if (!s) co_return;
            if (r != ContentDialogResult::Primary)
            {
                s->baseline = StampOfFile(file);  // asked again only when it changes again
                s->prompting = false;
                co_return;
            }
            if (again.IsChecked().Value())
            {
                settings_.confirmWriteBack = false;
                loadingSettings_ = true;
                ConfirmWriteBackToggle().IsOn(false);
                loadingSettings_ = false;
                SaveSettings();
            }
        }
        s = find();
        if (!s) co_return;
        s->baseline = StampOfFile(file);  // this is the content that goes in
        s->prompting = false;
        if (!current_ || currentPath_ != s->archive || tree_->Find(entryPath) == lycan::kNoNode) co_return;
        TaskMeta meta;
        meta.restoreFolder = CurrentFolderPath();
        meta.selectPaths.push_back(entryPath);
        RunUpdateAsync({lycan::UpdateOp::Replace(entryPath, file)}, std::move(meta));
    }

    // Closing the window while a saved file has not gone back: say so
    fire_and_forget MainWindow::AskCloseWithEditsAsync()
    {
        auto strong = get_strong();
        if (askingClose_ || dialogOpen_) co_return;
        askingClose_ = true;
        auto dlg = MakeDialog(lycan::Tr(L"main.changes_not_yet_put_back_into"), L"");
        TextBlock tb;
        tb.Text(lycan::Tr(L"main.file_you_edited_has_not_been"));
        tb.TextWrapping(TextWrapping::Wrap);
        tb.MaxWidth(420);
        dlg.Content(tb);
        dlg.PrimaryButtonText(lycan::Tr(L"main.close_without_putting_back"));
        dlg.CloseButtonText(lycan::Tr(L"main.back"));
        dlg.DefaultButton(ContentDialogButton::Close);
        const auto r = co_await ShowDialogAsync(dlg);
        askingClose_ = false;
        if (r == ContentDialogResult::Primary)
        {
            closeConfirmed_ = true;
            Close();
        }
    }

    // Search / list everything result -> jump to the folder of that item and select it
    void MainWindow::ShowNodeInFolder(uint32_t node)
    {
        if (!tree_ || node >= tree_->NodeCount()) return;
        flat_ = false;
        syncingUi_ = true;
        FlatToggle().IsChecked(false);
        syncingUi_ = false;
        NavigateTo(tree_->Node(node).parent, true);
        for (auto const& it : entries_)
        {
            auto row = it.as<LycanZip::EntryRow>();
            if (row.NodeId() == node)
            {
                ActiveList().SelectedItems().Clear();
                ActiveList().SelectedItems().Append(row);
                ActiveList().ScrollIntoView(row);
                break;
            }
        }
    }

    void MainWindow::ShowProperties(std::vector<uint32_t> const& nodes)
    {
        if (!tree_ || nodes.size() != 1) return;
        auto const& n = tree_->Node(nodes[0]);
        std::wstring parent = tree_->ParentPath(nodes[0]);
        std::wstring t = lycan::Tf(L"main.path", n.path);
        if (n.isDir)
        {
            t += lycan::Tr(L"main.type_folder");
            t += lycan::Tf(L"main.contains_total", n.fileCount, FormatSize(n.totalSize));
        }
        else
        {
            t += lycan::Tr(L"main.type_file");
            t += lycan::Tf(L"main.info_size", FormatSize(n.size), n.size);
            std::wstring packed = lycan::Tf(L"main.info_packed", FormatSize(n.packedSize));
            if (n.size > 0 && n.packedSize > 0)
            {
                packed += lycan::Tf(L"fmt.percent_paren", std::lround(100.0 * static_cast<double>(n.packedSize) / static_cast<double>(n.size)));
            }
            t += packed + L"\n";
        }
        if (n.mtimeFileTime) t += lycan::Tf(L"main.info_modified", FormatTime(n.mtimeFileTime));
        if (n.isEncrypted) t += lycan::Tr(L"main.info_encrypted");
        if (!n.isDir && current_ && n.entryIndex != lycan::kNoEntry && n.entryIndex < current_->Entries().size() && current_->Entries()[n.entryIndex].hasCrc)
        {
            wchar_t crc[16];
            swprintf_s(crc, L"%08x", current_->Entries()[n.entryIndex].crc);
            t += lycan::Tf(L"main.crc_32_stored_archive", crc);  // no need to unpack for this one; the other hash values are in the menu
        }
        // what the archive says besides the columns of the list (only what the format has)
        if (current_ && n.entryIndex != lycan::kNoEntry && n.entryIndex < current_->Entries().size())
        {
            const auto tech = current_->EntryTechInfo(n.entryIndex);
            std::wstring extra;
            if (tech.ctimeFileTime) extra += lycan::Tf(L"main.info_created", FormatTime(tech.ctimeFileTime));
            if (tech.atimeFileTime) extra += lycan::Tf(L"main.info_accessed", FormatTime(tech.atimeFileTime));
            if (!tech.method.empty()) extra += lycan::Tf(L"main.info_method", tech.method);
            if (tech.hasUnixMode)
            {
                std::wstring bits;
                for (int i = 0; i < 9; ++i) bits += (tech.unixMode & (0400u >> i)) ? L"rwxrwxrwx"[i] : L'-';
                wchar_t octal[16];
                swprintf_s(octal, L"%04o", tech.unixMode);
                extra += lycan::Tf(L"main.info_permissions", bits + L"  (" + octal + L")");
            }
            if (!tech.hostOs.empty()) extra += lycan::Tf(L"main.info_host_os", tech.hostOs);
            if (!tech.characteristics.empty()) extra += lycan::Tf(L"main.info_characteristics", tech.characteristics);
            if (!tech.linkTarget.empty()) extra += lycan::Tf(L"main.info_link", tech.linkTarget);
            if (tech.hasBlock) extra += lycan::Tf(L"main.info_block", tech.block);
            if (tech.hasOffset) extra += lycan::Tf(L"main.info_offset", tech.offset);
            if (!tech.comment.empty()) extra += lycan::Tf(L"main.info_comment", tech.comment);
            if (!extra.empty()) t += L"\n" + std::wstring{lycan::Tr(L"main.info_technical")} + extra;
        }
        ShowMessage(hstring{n.name}, hstring{t});
    }
}
