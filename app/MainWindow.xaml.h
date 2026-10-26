#pragma once
#include "MainWindow.g.h"
#include <unordered_map>
#include <unordered_set>
#include "winrt/LycanZip.h"

#include "TaskExecutorFactory.h"
#include <atomic>
#include <set>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <string>

#include "ImagePreview.h"
#include "ShellNotify.h"
#include "lycan/archive_source.h"
#include "lycan/crashreport.h"
#include "lycan/exectype.h"
#include "lycan/versioncheck.h"
#include "lycan/fileassoc.h"
#include "lycan/launch.h"
#include "lycan/presets.h"
#include "lycan/settings.h"
#include "lycan/shellreg.h"
#include "lycan/texttype.h"

namespace lycan { class InstalledUpdater; }   // src/updater/installed_update.h (only with Velopack)
#include "lycan/volumes.h"
#include "lycan/taskqueue.h"
#include "lycan/tree.h"

namespace winrt::LycanZip::implementation
{
    // when a file was last written and how big it is (a watched file is "changed" when this is not what it was)
    struct FileStamp
    {
        uint64_t size = 0;
        int64_t time = 0;
        bool ok = false;
        bool operator==(FileStamp const&) const = default;
    };

    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();
        ~MainWindow();

        // Start request (command line / context menu / handed over by another instance). fromLaunch: the request this process was started with
        void HandleRequest(lycan::LaunchRequest const& request, bool fromLaunch);
        void ReportBadArguments(std::wstring const& error);

        // ---- Several windows (the App dispatches requests; see the design notes "Several windows") ----
        static bool IsOpenableArchive(std::filesystem::path const& p);
        // The user opens an archive in a window that is in use already: the App sets this callback so that a new window is opened
        static inline std::function<void(std::filesystem::path)> sOpenInNewWindow;
        bool IsPristine() const;           // start page, no archive / tasks / running flow: can be used right away to open the next archive
        bool ShowsArchive(std::filesystem::path const& p) const;  // this window shows (or is opening) this archive (to detect duplicates)
        void BringToFront() { ActivateWindow(); }
        void ApplyExternalSettings(lycan::Settings const& s);     // another window saved the settings (broadcast by SettingsHub): apply them at once

        // ---- Crash records and recovery (see lycan/crashreport.h, the design notes "Crash records and recovery") ----
        // The archives this window has open and the unfinished tasks (for the snapshot of the crash report)
        void AppendCrashState(std::vector<std::filesystem::path>& archives, std::vector<lycan::CrashTask>& tasks) const;
        // Tells the App to rebuild the snapshot when opening / task state changes (set by the App)
        static inline std::function<void()> sStateChanged;
        // The report left by the last crash: cleans up compression temporary files, marks it as seen, shows an explanation (one click reopens the archives of last time)
        void ShowCrashRecovery(std::vector<lycan::CrashReport> reports);
        void ReloadSettingsIfChanged();    // another window changed the settings file: reload and apply (called when the window is activated)
        // Too many archives handed over at once (Explorer multi-select): asks how many to open. Returns the number to open (0 = cancel)
        Windows::Foundation::IAsyncOperation<int> AskOpenBatchAsync(int total, int safeCount, int maxCount);
        // ---- Background work (App drives it; see the design notes "Background work") ----
        // The user clicked "continue in the background": App hides every window and keeps the process alive while tasks run.
        static inline std::function<void()> sEnterBackground;
        // Something needs the user (a prompt): App brings the windows back. Also used to ask whether the mode is active.
        static inline std::function<void()> sLeaveBackground;
        static inline std::function<bool()> sBackgroundActive;
        struct TaskStats { int running = 0; int failed = 0; int needsUi = 0; unsigned long long done = 0, total = 0; };
        TaskStats CollectTaskStats();      // unfinished / failed tasks and the combined progress (UI thread)
        void CancelAllTasks() { queue_.CancelAll(); }
        void HideForBackground();
        void SetPreviewOnly();  // this window only exists to show a preview: hide it (see PreviewOnlyAsync)
        void ShowFromBackground();
        void OnBackgroundClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        bool ReplaceOnOpen() const;  // the setting "open mode = replace the current window" is on, and this window can be replaced safely now (no tasks, no dialog)

        // ---- XAML events ----
        void OnDragLeave(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        void OnDragOver(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        fire_and_forget OnDrop(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::DragEventArgs const&);
        fire_and_forget OnOpenArchiveClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OnNewFromFilesClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OnNewFromFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnBackClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void BuildExtractToMenu();
        void OnExtractToSplitClick(Microsoft::UI::Xaml::Controls::SplitButton const&, Microsoft::UI::Xaml::Controls::SplitButtonClickEventArgs const&);
        void OnExtractHereSplitClick(Microsoft::UI::Xaml::Controls::SplitButton const&, Microsoft::UI::Xaml::Controls::SplitButtonClickEventArgs const&);
        void OnCompactOptionClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCompactDoneClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OnExtractHereClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OnExtractToClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OnTestClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnEncodingChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        fire_and_forget OnResetSettingsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenSettingsFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenWebsiteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenSourceClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnShowWelcomeClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void RefreshDataLocationUi();
        fire_and_forget OnDataSwitchClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenLogFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCopyDiagnosticsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTaskCollapseClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnClearFinishedClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSettingsTabChanged(Microsoft::UI::Xaml::Controls::SelectorBar const&, Microsoft::UI::Xaml::Controls::SelectorBarSelectionChangedEventArgs const&);
        // ---- temporary files (settings page + the "not enough room" question) ----
        fire_and_forget OnTempChooseClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTempResetClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OnTempClearClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTemplateLoaded(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTaskPauseClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTaskResumeClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTaskCancelClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTaskTestClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTaskDeleteClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTaskOpenFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // Settings page
        void OnSettingsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSettingsBackClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSettingComboChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnSettingToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAccentChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnSettingNumberChanged(Microsoft::UI::Xaml::Controls::NumberBox const&, Microsoft::UI::Xaml::Controls::NumberBoxValueChangedEventArgs const&);
        void OnShellMenuToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget OnWin11MenuToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget RefreshWin11MenuAsync();
        winrt::fire_and_forget CheckStaleRegistrationsAsync();
        winrt::fire_and_forget RestartExplorerAsync();   // "Restart File Explorer" (asks first)
        void OnRestartExplorerClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        winrt::fire_and_forget CheckForUpdateAsync(bool manual);      // versioncheck.h: asks the web site; manual = the button (ignores the setting, the schedule and "skip")
        void ShowUpdateNotice(lycan::LatestInfo const& info);
        winrt::fire_and_forget InstallUpdateAsync();                  // the installed copy: downloads the signed update, installs it, starts again (src/updater)
        winrt::fire_and_forget PrepareUpdateAsync();                  // the installed copy, after the automatic check: downloads the signed update in the background
        void ApplyReadyUpdate();                                      // the downloaded update: starts the update program and ends this process
        bool updateBusy_ = false;
        std::shared_ptr<lycan::InstalledUpdater> readyUpdater_;       // the update that was downloaded in the background
        void OnUpdateCheckNowClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        bool win11MenuBusy_ = false;
        void OnFileAssocToggled(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnOpenDefaultAppsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

        // Archive view: navigation, sorting, search, multi-select, context menu
        void OnTreeExpanding(Microsoft::UI::Xaml::Controls::TreeView const&, Microsoft::UI::Xaml::Controls::TreeViewExpandingEventArgs const&);
        void OnTreeItemInvoked(Microsoft::UI::Xaml::Controls::TreeView const&, Microsoft::UI::Xaml::Controls::TreeViewItemInvokedEventArgs const&);
        void OnCrumbClicked(Microsoft::UI::Xaml::Controls::BreadcrumbBar const&, Microsoft::UI::Xaml::Controls::BreadcrumbBarItemClickedEventArgs const&);
        void OnUpClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSortClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnSearchTextChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TextChangedEventArgs const&);
        void OnMacMetaItemClick(Windows::Foundation::IInspectable const& sender, Microsoft::UI::Xaml::RoutedEventArgs const& e);
        void RebuildTree();   // after the "show macOS metadata" choice changed
        void OnFlatToggleClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnViewMenuClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnTreeToggleClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnPreviewImagesClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnPreviewAudioClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnContainerChanging(Microsoft::UI::Xaml::Controls::ListViewBase const&, Microsoft::UI::Xaml::Controls::ContainerContentChangingEventArgs const&);
        void OnEntrySelectionChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const&);
        void OnSelectAllClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnEntryDoubleTapped(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::DoubleTappedRoutedEventArgs const&);
        void OnEntryKeyDown(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const&);
        void OnEntryContextRequested(Microsoft::UI::Xaml::UIElement const&, Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const&);
        void OnRootSizeChanged(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::SizeChangedEventArgs const&);
        // Dragging items of the archive out to Explorer / the desktop (XAML drag and drop + files provided on demand)
        void OnEntryDragStarting(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::DragItemsStartingEventArgs const&);
        // Folder tree: drag out and context menu (the list's folder menu is shared, see AddNodeMenuItems)
        void OnTreeDragStarting(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Controls::TreeViewDragItemsStartingEventArgs const&);
        void OnTreeDragCompleted(Microsoft::UI::Xaml::Controls::TreeView const&, Microsoft::UI::Xaml::Controls::TreeViewDragItemsCompletedEventArgs const&) { dragOutActive_ = false; }
        void OnTreeContextRequested(Microsoft::UI::Xaml::UIElement const&, Microsoft::UI::Xaml::Input::ContextRequestedEventArgs const&);
        enum class DragOutResult { Cancel, Done, Provider };
        DragOutResult BeginDragOut(std::vector<uint32_t> const& nodes, std::wstring const& strip, std::wstring const& wrap,
                                   Windows::ApplicationModel::DataTransfer::DataPackage const& data);
        void AddNodeMenuItems(Microsoft::UI::Xaml::Controls::MenuFlyout const& fly, std::vector<uint32_t> const& nodes, std::wstring const& strip);
        void AddArchiveMenuItems(Microsoft::UI::Xaml::Controls::MenuFlyout const& fly);
        fire_and_forget ExtractNodes(std::vector<uint32_t> nodes, std::wstring strip, bool askFolder);
        fire_and_forget StartExtractCore(std::filesystem::path dest, std::vector<uint32_t> indices, std::wstring strip, bool selectionOnly, std::optional<lycan::ExtractMode> modeArg);
        fire_and_forget DeleteCurrentArchiveAsync();
        // Changing the archive: add / delete / rename / new folder (one Update task each; the design notes)
        void OnAddFilesClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnAddFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnNewFolderClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget AddPathsToArchive(std::vector<std::filesystem::path> paths, std::wstring into);
        fire_and_forget AddFromPickerAsync(bool folder, std::wstring into);
        fire_and_forget NewFolderAsync(std::wstring into);
        fire_and_forget RenameNodeAsync(uint32_t node);
        fire_and_forget DeleteNodesAsync(std::vector<uint32_t> nodes);
        fire_and_forget EditEntryAsync(uint32_t node);
        fire_and_forget OnScanClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void FinishScanTask(lycan::TaskInfo const& info);
        // hash values: the dialog of a request (files on disk, or entries of the open archive)
        struct HashRequest
        {
            std::vector<std::filesystem::path> files;
            std::filesystem::path archive;
            std::vector<uint32_t> indices;
            std::filesystem::path root;      // a folder that was chosen: its files are in `files`, names are shown and saved relative to its parent
            std::filesystem::path sidecar;   // a checksum file next to the single file: its value is compared when the hash values are done
            bool virusTotal = false;         // "Look up on VirusTotal": only the SHA-256 is calculated, and the VirusTotal page of it is opened when it is done
        };
        struct VerifyState;
        // "Verify checksum files": the files next to each checksum file, or (archive given) the files inside the archive
        void OnVerifySumsClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget VerifySumsFromPickerAsync();
        fire_and_forget ShowVerifyDialogAsync(std::vector<std::filesystem::path> sums, std::filesystem::path archive = {}, bool closeWhenDone = false);
        fire_and_forget RunVerifyAsync(std::shared_ptr<VerifyState> st);
        fire_and_forget HashSaveAsync(std::vector<std::filesystem::path> paths, bool closeWhenDone);   // "calculate and save as name.sha256": nothing is shown unless it takes long
        void QueueVerify(std::vector<std::filesystem::path> paths, bool closeWhenDone);   // one process per file when Explorer starts us: collected into one dialog
        void FlushVerify();
        // A one-line notice above the list (the encoding that was detected, a checksum file next to the archive): icon, text, links, a close button
        // A window that was started for a request (the command line, a right-click, a forwarded request) does not show the home page first and then
        // change: it is created hidden and shown when its first screen is set (the loading view, the compact window, a dialog, the compress page),
        // or after a second at the latest so that it can never stay invisible.
        void DeferShow();
        void RevealWindow();
        bool deferredShow_ = false;
        bool compactHost_ = false;         // the compact window only hosts a dialog (hash values, verify): no options row
        double dialogWidthDip_ = 640;      // the compact window's width while a dialog is open
        Microsoft::UI::Dispatching::DispatcherQueueTimer revealTimer_{nullptr};
        void ShowNotice(Microsoft::UI::Xaml::Controls::Border const& strip, bool warning, hstring const& title, hstring const& message,
                        std::vector<std::pair<std::pair<hstring, hstring>, std::function<void()>>> const& links);   // link = ((text, tooltip), action)
        void ShowSumsBanner();                                                              // the bar above an opened archive: "a checksum file is next to it"
        fire_and_forget HashRequestAsync(std::vector<std::filesystem::path> paths, bool closeWhenDone);   // files and folders (their files, all levels)
        std::vector<std::filesystem::path> verifyPaths_;
        bool verifyClose_ = false;
        Microsoft::UI::Dispatching::DispatcherQueueTimer verifyTimer_{nullptr};
        struct HashDialogState;
        fire_and_forget ShowHashDialogAsync(HashRequest req, bool closeWhenDone = false);
        fire_and_forget SaveHashSumsAsync(std::shared_ptr<HashDialogState> st);
        void HashNodes(std::vector<uint32_t> const& nodes);
        void VirusTotalNode(uint32_t node);                                   // the SHA-256 of one file of the archive, then its page at VirusTotal
        void OnVirusTotalArchiveClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);   // the same for the archive file
        fire_and_forget ShowArchiveInfoAsync();
        void OnArchiveInfoClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnHashArchiveClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        std::map<uint64_t, std::function<void(lycan::TaskInfo const&)>> hashHandlers_;  // the dialog of a hash task (it has no card)
        std::set<uint64_t> scanShown_;   // finished scans whose result was shown
        void OnEditorPathLostFocus(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void ShowStatusNote(std::wstring const& text);
        void HideStatusNote();
        bool statusNoteActive_ = false;
        std::vector<uint32_t> lastNoteSelection_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer statusNoteTimer_{nullptr};
        void PreviewImages();
        // The welcome window (first start, or the button on the About page)
        bool MaybeShowWelcome();
        void MaybeOfferWelcome();   // the first full window opened with an archive: a notice that offers the welcome window
        fire_and_forget ShowWelcomeAsync();
        fire_and_forget ApplyWelcomeAsync(lycan::Settings s, lycan::Settings original, bool classicMenu, bool win11Menu, bool fileAssoc);
        fire_and_forget PreviewAudio(uint32_t pick = 0xFFFFFFFFu);   // pick: the audio file to start with (default: the selected one)
        void UpdateTempCard();
        fire_and_forget RefreshTempUsage();
        // The scratch folder has no room (tar.gz opened / compressed): offers a local disk or a folder. Returns the folder to
        // use (temporary files go to <it>\LycanZip) or an empty string when cancelled; "always" saves it in the settings.
        Windows::Foundation::IAsyncOperation<hstring> AskTempLocationAsync(uint64_t need, uint64_t have, std::wstring dir);
        fire_and_forget HandleTempSpaceTask(lycan::TaskInfo info);
        void RemoveTaskRow(uint64_t id);
        std::set<uint64_t> tempAsked_;
        // Opening an archive from nothing (Explorer / right-click): no home page flash. A blank view first, the spinner
        // with the file name after a short delay, then the archive; failures go back to the home page.
        void ShowLoading(std::wstring const& name);
        void EndLoading(bool goHome);
        void ApplyLoadingVisibility();
        bool loading_ = false, loadingShown_ = false;
        Microsoft::UI::Dispatching::DispatcherQueueTimer loadingTimer_{nullptr};
        fire_and_forget PreviewNode(uint32_t node);  // picture preview (separate window, read only when clicked)
        fire_and_forget HandleDroppedPaths(std::vector<std::filesystem::path> paths);  // paths dragged in / opened: archive -> open; self-extracting archive -> ask; anything else -> add to an archive
        void OnEntryDragCompleted(Microsoft::UI::Xaml::Controls::ListViewBase const&, Microsoft::UI::Xaml::Controls::DragItemsCompletedEventArgs const&);
        bool dragOutActive_ = false;
        void OnFindAccelerator(Microsoft::UI::Xaml::Input::KeyboardAccelerator const&, Microsoft::UI::Xaml::Input::KeyboardAcceleratorInvokedEventArgs const&);
        fire_and_forget OnExtractSelHereClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        fire_and_forget OnExtractSelToClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnCopyPathClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);
        void OnClearSelectionClick(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        struct ConflictRequest;
        struct LargeRequest;
        // the view of the archive that is changed: it comes back after the archive is opened again
        struct KeptView
        {
            bool valid = false;
            bool flat = false, asc = true, tree = true;
            lycan::ViewMode mode = lycan::ViewMode::Details;
            lycan::SortKey sort = lycan::SortKey::Name;
            std::wstring query;
        };
        // What is needed for what happens after a task finishes (open the folder, delete the original)
        struct TaskMeta
        {
            std::filesystem::path archive;
            bool wholeArchive = false;
            bool handled = false;
            std::filesystem::path openFile;  // not empty: this is a temporary extraction for "open with the default program"; open this file when done
            bool deletable = false;          // finished cleanly: the "delete the archive" button is offered
            bool isUpdate = false;           // changing the archive: after it, open it again
            std::wstring editEntry;          // non-empty: openFile is extracted to be edited (the item of the archive)
            std::wstring restoreFolder;      // ... in this folder
            std::vector<std::wstring> selectPaths;  // ... with these items selected
            KeptView keep;
        };
        enum class View { Home, Archive, Settings, Compress };
        // The compact window must not close by itself while an asynchronous flow is running
        struct OpGuard
        {
            MainWindow* w;
            explicit OpGuard(MainWindow* x) : w(x) { ++w->busyOps_; }
            ~OpGuard() { --w->busyOps_; w->CheckCompactAutoClose(); }
            OpGuard(const OpGuard&) = delete;
        };

        // a file that is being edited: extracted to a temporary folder, watched, offered back to the archive when it was saved
        struct EditSession
        {
            uint64_t id = 0;
            std::filesystem::path archive, file;
            std::wstring entryPath;
            FileStamp archiveStamp;               // the archive when the file was extracted
            FileStamp baseline, lastSeen;         // the file: what is in the archive / what the last look saw
            int stable = 0, missing = 0;
            bool prompting = false;
        };
        std::vector<EditSession> edits_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer editTimer_{nullptr};
        void StartEditSession(TaskMeta const& meta);
        void PollEdits();
        bool HasUnsavedEdits() const;
        fire_and_forget PromptWriteBack(uint64_t id);
        fire_and_forget AskCloseWithEditsAsync();
        bool CanModifyNow(bool explain);
        lycan::UpdateOptions MakeUpdateOptions(lycan::UpdateMode mode = lycan::UpdateMode::AddReplace);
        std::wstring CurrentFolderPath() const;
        fire_and_forget RunUpdateAsync(std::vector<lycan::UpdateOp> ops, TaskMeta meta, lycan::UpdateMode mode = lycan::UpdateMode::AddReplace);
        // Adding to an archive that has items of the same name: what to do with them (0..2 = AddReplace / UpdateAdd / Freshen, -1 = cancelled)
        Windows::Foundation::IAsyncOperation<int32_t> ChooseAddModeAsync(int clashes);
        // Making an archive whose file exists already: 0 = cancelled, 1 = save under a new name, 2 = replace it, 3 + UpdateMode = add to it (canUpdate: zip, 7z, tar)
        Windows::Foundation::IAsyncOperation<int32_t> ChooseExistingOutputAsync(std::filesystem::path output, bool canUpdate);
        // Synchronize deletes: say what goes (false = the user said no, or it did not work)
        Windows::Foundation::IAsyncOperation<bool> ConfirmSyncAsync(std::filesystem::path archive, std::vector<lycan::UpdateOp> ops, lycan::UpdateOptions uo);
        fire_and_forget StartCompressAsync();
        void FinishUpdateTask(lycan::TaskInfo const& info, TaskMeta& meta);
        Windows::Foundation::IAsyncOperation<hstring> AskEntryNameAsync(hstring title, hstring initial);
        uint64_t updateTask_ = 0;        // the Update task of this window that has not finished (one at a time)
        std::filesystem::path restoreFor_;
        std::wstring restoreFolder_;
        std::vector<std::wstring> restoreSelect_;
        KeptView keptView_;

        HWND Hwnd();
        static lycan::ShellRegOptions ShellOptions();
        static lycan::FileAssocOptions AssocOptions();
        void RefreshFileAssocStatus();
        void ApplyStoreChannelUi();   // the Store version: the Windows 11 menu and the file types are built into the package, so their switches are fixed (on) and the classic menu cannot be registered
        // Two copies on one PC (the Store version and an installed / zip / portable one): the Store version provides the menu and the file types, the other
        // copy switches its own off when it starts (StepAsideForStore) and says so once; the Store version names a copy that still has them (OtherCopyLocation)
        bool StepAsideForStore();
        static bool IsStoreUi();   // the Store version (or LYCAN_TEST_STORE_UI)
        std::wstring OtherCopyLocation();
        winrt::fire_and_forget CheckInstalledCopiesAsync();
        winrt::Windows::Foundation::IAsyncAction TwoCopiesDialogAsync(std::wstring text);
        void SetBusy(bool busy);
        void LoadBrandImages();
        void ResizeToDefault();
        void UpdateTitleDragRects();
        void UpdateArchiveIcon();
        void ResizeDip(int widthDip, int heightDip);
        fire_and_forget ShrinkCompactSoon();
        void UpdateTaskSummary();
        void UpdateLangNote();
        fire_and_forget OpenArchiveChecked(std::filesystem::path path);  // a later volume of a split archive: asks whether to open the first one
        void OpenFromUi(std::filesystem::path p);  // open an archive from inside the window: open a new window if this one is in use, otherwise open in place
        void StampSettings();
        fire_and_forget ShowCrashRecoveryAsync(std::vector<lycan::CrashReport> reports, int cleaned);
        void NotifyStateChanged(bool force);  // task progress updates are frequent: when not forced, notify at most once per second
        uint64_t stateTick_ = 0;
        // Tasks are still running when the window closes: confirm first (closing at once cancels all tasks)
        int UnfinishedTaskCount();
        fire_and_forget AskCloseWithTasksAsync(int count);
        bool closeConfirmed_ = false, askingClose_ = false;
        std::atomic<bool> compactFlag_{false};  // lower bound of the minimum size of the compact window (one per window, MinSizeProc reads it)
        int openPending_ = 0;                   // number of OpenArchiveAsync running (IsPristine must exclude them)
        std::filesystem::path openingPath_;     // the archive being opened (to detect duplicates; cleared when openPending_ is zero)
        std::filesystem::file_time_type settingsStamp_{};  // file time of the settings file when it was loaded / written last
        void UpdateTaskbar(lycan::TaskInfo const* finished);
        void FadeOutAndClose();
        void ShowView(View v);
        void ShowHome();
        void OnTaskUpdate(lycan::TaskInfo const& info);
        void FinishExtractTask(lycan::TaskInfo const& info, TaskMeta& meta);
        void Post(std::function<void()> fn);

        // Settings
        void LoadSettingsToUi();
        void ReadSettingsFromUi();
        void ApplySettings();
        void SaveSettings();

        // Dialogs (only one at a time; later ones wait in a queue)
        Windows::Foundation::IAsyncOperation<Microsoft::UI::Xaml::Controls::ContentDialogResult> ShowDialogAsync(
            Microsoft::UI::Xaml::Controls::ContentDialog dlg);
        Microsoft::UI::Xaml::Controls::ContentDialog MakeDialog(hstring const& title, const wchar_t* glyph = nullptr);
        fire_and_forget ShowMessage(hstring title, hstring body);

        // Password
        Windows::Foundation::IAsyncOperation<hstring> AskPasswordAsync(hstring name, bool previousWasWrong);
        Windows::Foundation::IAsyncOperation<bool> EnsurePasswordAsync();  // encrypted content: ask and verify, store in password_ on success

        // Conflict question (called from the worker thread, blocks until the user answers)
        lycan::ConflictAnswer AskConflict(lycan::ConflictInfo const& info, std::shared_ptr<std::atomic<int>> const& rule);
        fire_and_forget ShowConflictDialog(std::shared_ptr<ConflictRequest> req);
        // An archive that would unpack to far more than it is: asked on the thread of the task before anything is written (false = cancel)
        bool AskLarge(lycan::LargeExtractInfo const& info);
        fire_and_forget ShowLargeDialog(std::shared_ptr<LargeRequest> req);
        void ApplyLargeGuard(lycan::ExtractOptions& eo);

        // Context menu actions (compact window)
        void ActivateWindow();
        void EnterCompact();
        void ExitCompact();
        void CheckCompactAutoClose();
        void FlushPending();
        void RememberRecentExtract(std::filesystem::path const& dir);
        fire_and_forget PreviewOnlyAsync(std::filesystem::path path, bool audio);
        fire_and_forget ExtractToRequestAsync(lycan::LaunchRequest req);
        void LeavePreviewOnly(std::filesystem::path const& path, std::wstring const& note);
        fire_and_forget AutoCompress(std::vector<std::filesystem::path> paths, lycan::ArchiveType type, std::wstring preset = {}, bool askExisting = true, std::filesystem::path outDir = {});
        fire_and_forget RunQuickQueue();
        // One archive of a context-menu extraction / test: where it goes and how
        struct QuickJob
        {
            std::filesystem::path archive;
            bool folderMode = false;       // always into a folder named after the archive
            bool test = false;             // only test the integrity
            std::filesystem::path dest;    // empty = next to the archive
            bool noFolder = false;         // contents go straight into dest (merging several archives)
        };
        Windows::Foundation::IAsyncAction QuickExtractOne(QuickJob job);
        static std::filesystem::path DefaultArchiveOutput(std::vector<std::filesystem::path> const& inputs, const wchar_t* ext, bool unique = true,
                                                              std::filesystem::path const& outDir = {});

        fire_and_forget OpenArchiveAsync(std::filesystem::path path, unsigned codepage, std::wstring password, bool previousWasWrong = false);
        void ShowArchive(std::shared_ptr<lycan::ArchiveSource> src, std::filesystem::path path, unsigned codepage);
        // selectionOnly: extract only the items selected in the list (and strip the prefix of the current folder); otherwise extract the whole archive
        // mode: what to do about a folder named after the archive (default: the setting). Selections never get one.
        fire_and_forget StartExtract(std::filesystem::path dest, bool selectionOnly = false, std::optional<lycan::ExtractMode> mode = std::nullopt);
        void UpdateExtractButton();  // main button + drop-down follow the "extract here" setting and the open archive
        void ExtractHereWith(lycan::ExtractMode mode);
        void UpdateCompactOptions();  // the "after completion" row of the compact window follows the settings
        void UpdateCompactDone();
        void SetCompactResizable(bool resizable);
        void StartCompress(std::vector<std::filesystem::path> inputs);

        // "New archive" page
        void InitCompressPage();
        void UpdateCompressLayout();
        void LeaveCompressPage();
        void OnCompressStart();
        void RefreshCompressUi();
        void RebuildSources();
        void OnSourcesChanged();
        void RebuildMethodItems(lycan::ArchiveType type);
        void RebuildWordItems(bool ppmd);
        void FillPresetCombo(std::wstring const& select);
        void LoadPresetToUi(lycan::CompressPreset const& p);
        lycan::CompressPreset ReadCompressUi();
        std::wstring DefaultArchiveBaseName() const;
        bool PresetNameTaken(std::wstring const& name) const;
        fire_and_forget AddSourcesFromPicker(bool folder);
        bool TryPasteFiles();
        fire_and_forget PasteFilesAsync(std::wstring into = {}, bool targeted = false);   // targeted: into that folder of the archive (otherwise the one that is shown)
        static bool ClipboardHasFiles();
        fire_and_forget OnPresetSaveNew();
        void OnPresetUpdate();
        fire_and_forget OnPresetRename();
        fire_and_forget OnPresetDelete();
        Windows::Foundation::IAsyncOperation<hstring> AskNameAsync(hstring title, hstring initial);
        void FinishCompressTask(lycan::TaskInfo const& info);
        std::wstring CompressPassword();
        void SetCompressPassword(std::wstring const& pw);
        void ApplyPasswordMode();
        bool cVolInvalid_ = false;
        bool cPageActive_ = false, cLoading_ = false, cNameEdited_ = false, cFromCompact_ = false;
        View cReturnView_ = View::Home;
        std::vector<std::filesystem::path> cSources_;
        std::filesystem::path cOutDir_;
        std::vector<lycan::CompressPreset> cBuiltin_, cUser_;
        lycan::CompressPreset cBase_, cBaseNorm_;  // the preset selected now; cBaseNorm_ is how it looks after it is loaded into the controls and read back (to tell "modified")
        Microsoft::UI::Xaml::Controls::MenuFlyoutItem cMenuUpdate_{nullptr}, cMenuRename_{nullptr}, cMenuDelete_{nullptr};
        Microsoft::UI::Xaml::Controls::ToggleMenuFlyoutItem cMenuInMenu_{nullptr};  // "show in the right-click menu" of the chosen template
        // finished compressions: what the card's buttons need (guarded by metaM_)
        struct CompressMeta
        {
            std::wstring password;                       // for testing the new archive
            std::vector<std::filesystem::path> sources;  // the originals ("delete the originals" button)
            std::filesystem::path output;
            bool testAfter = false;
            bool sumsArchive = false, sumsFiles = false;   // write the checksum files next to the new archive when it is done
            int testState = 0;                           // 0 not tested, 1 passed, 2 failed
        };
        std::map<uint64_t, CompressMeta> compressMeta_;
        std::map<uint64_t, uint64_t> testFor_;  // follow-up test task -> the card (finished task) that shows it
        std::map<uint64_t, uint64_t> testOf_;   // card -> its running follow-up test task
        void StartFollowupTest(uint64_t cardId);
        fire_and_forget WriteSumsAfterCompressAsync(uint64_t cardId, std::vector<std::filesystem::path> outputs, std::vector<std::filesystem::path> sources, bool archiveSum, bool filesSum);
        void OnFollowupTestUpdate(uint64_t cardId, lycan::TaskInfo const& info);
        fire_and_forget DeleteOriginalsAsync(uint64_t cardId);
        uint64_t ActiveTaskFor(uint64_t cardId) const { auto it = testOf_.find(cardId); return it == testOf_.end() ? cardId : it->second; }
  // Sources to move to the recycle bin after a successful compression (protected by metaM_)
        std::vector<uint32_t> SelectedNodes();
        std::vector<uint32_t> SelectedIndices();

        // Archive view
        void BuildFolderTree();
        Microsoft::UI::Xaml::Controls::TreeViewNode MakeTreeNode(uint32_t node);
        void RealizeTreeChildren(Microsoft::UI::Xaml::Controls::TreeViewNode const& tn);
        void ExpandTreeTo(uint32_t node);
        void NavigateTo(uint32_t node, bool syncTree);
        void RefreshList(bool keepSelection = false);
        void UpdateSelectionUi();
        void UpdateTreeVisibility();
        void UpdateTaskPanelMax();

        // Details view: the columns have widths of their own (DIP); when they are wider than the window the list scrolls sideways and the header follows
        void UpdateListColumns();
        void ApplyRowColumns(Microsoft::UI::Xaml::FrameworkElement const& root, LycanZip::EntryRow const& row);
        struct ListCols { bool path = false, type = false, size = false, packed = false, ratio = false, modified = false, encrypted = false, method = false, crc = false; bool operator==(ListCols const&) const = default; };
        ListCols ComputeListCols();  // which columns are shown now: the ticked ones, and the folder column in "list everything" / search results
        ListCols colVis_;
        enum ListCol { LcName, LcPath, LcType, LcSize, LcPacked, LcRatio, LcModified, LcEncrypted, LcMethod, LcCrc, LcCount };   // the order of the columns in the header and in the rows
        static constexpr double kLcDefault[LcCount] = {300, 200, 62, 78, 78, 70, 136, 76, 110, 96};   // widths without what the user dragged
        static constexpr double kLcMin[LcCount] = {120, 80, 44, 56, 56, 44, 90, 44, 70, 76};
        static constexpr double kLcMax = 700;
        double colW_[LcCount] = {300, 200, 62, 78, 78, 70, 136, 76, 110, 96};   // the widths the user chose (settings_.listColWidths) or the defaults
        double curW_[LcCount] = {300, 200, 62, 78, 78, 70, 136, 76, 110, 96};   // the widths in use now: the hidden columns are 0, the name takes what is left when everything fits
        double rowW_ = 0;                                      // the width of a row (all columns)
        Microsoft::UI::Xaml::Controls::ScrollViewer listScroll_{nullptr};   // inside EntryList: the header follows its horizontal offset
        Microsoft::UI::Xaml::Controls::Border colGrip_[LcCount]{};           // the handles at the right edge of the column headers
        int colDrag_ = -1;
        double colDragX_ = 0, colDragW_ = 0;
        void LoadColWidths();
        std::string ColWidthsText() const;
        void InitListHeader();
        void HookListScroll();
        void ResetColWidths();
        bool colsInit_ = false;
        double treeWidth_ = 200;
        bool treeDragging_ = false;
        double treeDragStartX_ = 0, treeDragStartW_ = 0;
        void InitTreeSplitter();
        bool bandPending_ = false;  // pressed on an empty part of an item: the band selection starts after a small move (a simple click still selects that row)
        Windows::Foundation::Point bandPressPos_{};

        // Keyboard shortcuts (the shortcut list): implemented as KeyboardAccelerator, they work also when the focus is on an empty part
        void InitShortcuts();
        void BuildShellItemsUi();   // the check boxes of the context-menu entries (settings page)
        void UpdateShellItemsUi();
        void RebuildShellPresetRows();
        bool PresetInMenu(std::wstring const& name, bool builtin);
        void SetPresetInMenu(std::wstring const& name, bool builtin, bool on);
        bool FocusInTextInput();
        void ChooseViewMode(int n);

        // Band selection with the mouse: press and drag on an empty part of the list / icon view; the items inside the band are selected (Ctrl / Shift = add)
        void OnBandPressed(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnBandMoved(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnBandReleased(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        void OnBandCaptureLost(Windows::Foundation::IInspectable const&, Microsoft::UI::Xaml::Input::PointerRoutedEventArgs const&);
        bool bandRecapturing_ = false;
        void UpdateBand(Windows::Foundation::Point pos);
        void EndBand();
        bool bandActive_ = false, bandVisible_ = false;
        Windows::Foundation::Point bandStart_{}, bandLast_{};  // bandStart_ is in "content coordinates" (including the scroll offset)
        std::set<int32_t> bandBase_, bandHit_;
        Microsoft::UI::Xaml::Controls::ScrollViewer bandScroll_{nullptr};
        Microsoft::UI::Xaml::Controls::ListViewBase bandList_{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueueTimer bandTimer_{nullptr};

        // Motion and sound (see the design notes "Motion and sound")
        bool AnimOn() const;
        void GlowMoon(LycanZip::TaskRow const& row, bool failed);   // the moon of the task card glows once when the task ends
        void FadeIn(Microsoft::UI::Xaml::UIElement const& el, double dy = 10, int ms = 200, int delayMs = 0);
        void PlaySfx(const wchar_t* name);
        void SetDragGlow(bool on);
        void PulseTaskPanel();
        bool dragGlow_ = false;
        uint64_t listFxUntil_ = 0;
        std::set<uint64_t> fxDoneTasks_;
        lycan_app::ShellNotifier notifier_;   // taskbar progress, flashing, system notifications
        int batchOk_ = 0, batchBad_ = 0;      // number of tasks completed / failed in the current batch (from the first task until all are finished)
        std::wstring batchTitle_, batchError_;  // the last task that ended (used to compose the notification text when there was one task only)
        lycan::TaskKind batchKind_ = lycan::TaskKind::Extract;
        void ApplyDensity();
        void ApplyAccent();
        void UpdateBackdrop();
        void LayoutHomeScene();
        void UpdateArchiveStrip(bool dark, Windows::UI::Color base);
        void LayoutArchiveStrip();
        void UpdateToolbarText();
        void LockArchiveFile(std::filesystem::path const& path);
        void UnlockArchiveFile();
        HANDLE archiveLock_ = INVALID_HANDLE_VALUE;   // read-only handle that keeps others from changing or deleting the shown archive
        bool toolbarIconsOnly_ = false;
        double compactDialogHeight_ = 0;   // the height (dip) the compact window was given for the dialog that is open
        void BuildAccentPicker();
        void ApplyViewChoice(wchar_t category, int n);  // shared by the View menu and the context menu of an empty part: v view mode, t thumbnails on/off, z thumbnail size, s sort key, d ascending / descending
        void ApplyViewMode();
        // The icons of Windows (ShellIcons.h): the pictures are made once per kind of file; rows and tree nodes get them when they are there
        void ApplyShellIcon(LycanZip::EntryRow const& row);
        void RefreshShellIcons();
        winrt::fire_and_forget LoadShellIconsAsync(std::vector<std::wstring> keys);
        std::unordered_map<std::wstring, Microsoft::UI::Xaml::Media::ImageSource> shellIconCache_;   // "key|px"
        std::unordered_set<std::wstring> shellIconPending_;
        bool shellIconBusy_ = false;                 // shows the controls and the template that belong to viewMode_
        // Thumbnails of pictures in the icon view (only for archives with random access that are not solid; for other archives the screen says why)
        bool ThumbsActive() const { return settings_.iconThumbnails && viewMode_ == lycan::ViewMode::Icons; }
        std::wstring ThumbUnavailableReason() const;  // empty string = thumbnails can be made for this archive
        struct ThumbDims { double item, boxH; uint32_t px; };
        static ThumbDims ThumbDimsFor(unsigned size);
        static double IconDipFor(unsigned size);
        bool EnsureThumbSession();
        void BindThumb(LycanZip::EntryRow const& row);
        void UpdateViewMenuChecks();
        Microsoft::UI::Xaml::Controls::ListViewBase ActiveList();
        void ApplyViewSettings();
        std::wstring StripPrefixForView() const;
        void OpenNode(uint32_t node);
        fire_and_forget OpenEntryFileAsync(uint32_t node);
        void ShowProperties(std::vector<uint32_t> const& nodes);
        void CopyPaths(std::vector<uint32_t> const& nodes);
        void ShowNodeInFolder(uint32_t node);
        void CleanupStaleTemp();
        static LycanZip::EntryRow RowFromSource(Windows::Foundation::IInspectable const& source);
        lycan::OpenOptions MakeOpenOptions();

        Microsoft::UI::Dispatching::DispatcherQueue dispatcher_{nullptr};
        bool contrastBackdrop_ = false;   // the Mica backdrop is off (a contrast look)
        Windows::UI::ViewManagement::AccessibilitySettings hcWatch_{nullptr};   // keeps the high contrast notification alive
        lycan::Settings settings_;
        std::filesystem::path settingsPath_;
        bool loadingSettings_ = false;
        View view_ = View::Home;
        View viewBeforeSettings_ = View::Home;
        bool dialogOpen_ = false;
        std::atomic<bool> closing_{false};

        std::shared_ptr<lycan::ArchiveSource> current_;
        std::filesystem::path currentPath_;
        unsigned currentCp_ = 0;      // 0 = detect automatically
        std::wstring password_;
        bool passwordVerified_ = false;
        bool updatingEncoding_ = false;
        Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable> entries_{nullptr};

        // Archive view state
        std::shared_ptr<lycan::ArchiveTree> tree_;
        uint32_t curNode_ = 0;                                  // the current folder (one level) / the scope (list everything, search)
        // Changing the file name encoding reopens the archive; the path text changes with the encoding, so the folder is remembered as "order number of the first item below it + depth"
        struct RestorePos { std::filesystem::path archive; uint32_t entry = lycan::kNoEntry; int depth = 0; } restorePos_;
        lycan::ViewMode viewMode_ = lycan::ViewMode::Details;
        lycan_app::ListThumbs thumbs_;
        std::wstring thumbSessionKey_;
        std::wstring archiveInfo_;                              // archive information on the left of the status bar (format, number of items, size, solid / encrypted)
        void SetWindowTitle(std::wstring const& archiveName);
        void SetTitleProgress(int percent);   // -1 = none; the percentage goes in front of the window title (the taskbar and Alt+Tab show it too)
        std::wstring titleName_;
        int titlePct_ = -1;
        lycan_app::ProgressState lastTaskbarState_ = lycan_app::ProgressState::None;
        bool thumbNoteShown_ = false;
        bool flat_ = false;                                     // list everything
        bool treeWanted_ = true;                                // the user wants the folder tree (it is still hidden automatically in a narrow window)
        lycan::SortKey sortKey_ = lycan::SortKey::Name;
        bool sortAsc_ = true;
        std::wstring query_;
        std::vector<uint32_t> keepOverride_;                    // when switching the view: take the current selection first, so that RefreshList can restore it in the new control
        bool syncingUi_ = false;
        bool starsBuilt_ = false;
        bool archiveStarsBuilt_ = false;
        std::vector<std::pair<double, double>> archiveStarFrac_;
        std::vector<double> archiveStarSize_;
        struct ConStar { double rx, ry, size; };  // a star of the constellation: place inside its box (0..1) and its size
        std::vector<ConStar> conStars_;
        std::vector<std::pair<double, double>> starFrac_;
        // a few small stars of the home page twinkle slowly: only while the home page shows in the active window and animations are on
        std::vector<Microsoft::UI::Xaml::Media::Animation::Storyboard> twinkle_;
        bool windowActive_ = true;
        void UpdateTwinkle();
        std::vector<double> starSize_;                                // when the program changes a control itself, do not trigger the event handlers
        std::vector<uint32_t> crumbNodes_;
        std::map<uint32_t, Microsoft::UI::Xaml::Controls::TreeViewNode> treeNodes_;
        Microsoft::UI::Dispatching::DispatcherQueueTimer searchTimer_{nullptr};        Windows::Foundation::Collections::IVector<Windows::Foundation::IInspectable> taskRows_{nullptr};
        std::map<uint64_t, LycanZip::TaskRow> taskById_;
        std::map<uint64_t, TaskMeta> taskMeta_;
        std::mutex metaM_;

        // Compact window (context menu actions): shows only the task panel, closes by itself after all succeeded
        bool compact_ = false;
        bool compactExtract_ = false;  // the compact window runs an extraction or a test (otherwise a compression)
        std::vector<std::pair<std::string, Microsoft::UI::Xaml::Controls::CheckBox>> shellChecks_;
        Microsoft::UI::Xaml::Controls::StackPanel shellPresetRows_{nullptr};
        Microsoft::UI::Xaml::Controls::ComboBox shellFmt1_{nullptr}, shellFmt2_{nullptr};
        bool previewOnly_ = false;     // hidden window of --preview-images / --preview-audio
        std::wstring openNote_;        // shown in the info area once the next archive is on screen
        bool compactTest_ = false;     // ... and it is a test (no 'open folder' option)
        int busyOps_ = 0;  // number of asynchronous flows running (dialogs, opening an archive ...), so that it does not close by itself in the middle of one
        lycan::LaunchAction pendingKind_ = lycan::LaunchAction::None;
        lycan::ArchiveType pendingFormat_ = lycan::ArchiveType::Zip;
        std::wstring pendingPreset_;
        std::filesystem::path pendingDest_;  // --dest of the request: the folder for the archive (empty = next to the source)
        std::vector<std::filesystem::path> pendingPaths_;  // multi-select: paths of the same batch of requests, collected for a short time
        Microsoft::UI::Dispatching::DispatcherQueueTimer aggTimer_{nullptr};
        Microsoft::UI::Dispatching::DispatcherQueueTimer closeTimer_{nullptr};
        std::deque<QuickJob> quickQueue_;
        bool quickRunning_ = false;
        lycan::TaskQueue queue_{1, lycan_app::MakeTaskExecutor()};  // must be declared last: destroyed first, so that the worker threads (which call back into the UI) end first
    };
}

namespace winrt::LycanZip::factory_implementation
{
    struct MainWindow : MainWindowT<MainWindow, implementation::MainWindow> {};
}
